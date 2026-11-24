// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's touchscreen to Android Auto: UDP tap, touch state machine, socket taken while the picture shows.
#include "evo_touch.hpp"

#include <arpa/inet.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include "evo_log.hpp"

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

namespace evo {

static const uint16_t kTouchPort = 30567;

bool parseIpv4Udp(const uint8_t* p, size_t n, UdpView& out) {
    if (!p || n < 20 || (p[0] >> 4) != 4) {
        return false;
    }
    const size_t ihl = static_cast<size_t>(p[0] & 0x0f) * 4;
    const size_t total = (static_cast<size_t>(p[2]) << 8) | p[3];
    if (ihl < 20 || total < ihl + 8 || total > n || p[9] != 17) {
        return false;
    }
    if (((static_cast<unsigned>(p[6]) << 8) | p[7]) & 0x3fff) {
        return false;  // MF flag or fragment offset
    }
    const uint8_t* u = p + ihl;
    const size_t ulen = (static_cast<size_t>(u[4]) << 8) | u[5];
    if (ulen < 8 || ihl + ulen > total) {
        return false;
    }
    out.src = (static_cast<uint32_t>(p[12]) << 24) | (static_cast<uint32_t>(p[13]) << 16) |
              (static_cast<uint32_t>(p[14]) << 8) | p[15];
    out.sport = static_cast<uint16_t>((u[0] << 8) | u[1]);
    out.dport = static_cast<uint16_t>((u[2] << 8) | u[3]);
    out.data = u + 8;
    out.len = ulen - 8;
    return true;
}

bool isTouchPdu(const uint8_t* b, size_t n) {
    return n >= 4 && b[0] == 0xB0 && b[1] == 0x04 && b[2] == 0x80 && b[3] == 0x02;
}

static int clampi(double v, int lo, int hi) {
    int i = static_cast<int>(v < 0 ? v - 0.5 : v + 0.5);
    return i < lo ? lo : (i > hi ? hi : i);
}

TouchTracker::TouchTracker(double sx, double sy, double ox, double oy, int maxX, int maxY)
    : sx_(sx), sy_(sy), ox_(ox), oy_(oy), maxX_(maxX), maxY_(maxY), pictureStarted_(true) {
    reset();
}

void TouchTracker::reset() {
    for (int j = 0; j < NS; ++j) {
        active_[j] = needDown_[j] = blocked_[j] = false;
        cx_[j] = cy_[j] = 0;
    }
    reasm_ = aa::TouchReassembler();
    haveMsg_ = false;
    prevSeq0_ = prevSeq1_ = 0;
    messages_ = 0;
    lastN_ = 0;
}

int TouchTracker::activeCount() const {
    int n = 0;
    for (int j = 0; j < NS; ++j) {
        if (active_[j]) {
            ++n;
        }
    }
    return n;
}

int TouchTracker::lastContacts(aa::TouchContact* out, int max) const {
    int n = 0;
    for (; n < lastN_ && n < max; ++n) {
        out[n] = last_[n];
    }
    return n;
}

void TouchTracker::fillActive(aa::TouchEventOut& e, int which) const {
    e.n = 0;
    e.index = 0;
    e.action = 2;
    for (int j = 0; j < NS && e.n < NS; ++j) {
        if (active_[j]) {
            if (j == which) {
                e.index = e.n;
            }
            e.p[e.n].id = j;
            e.p[e.n].x = cx_[j];
            e.p[e.n].y = cy_[j];
            ++e.n;
        }
    }
}

void TouchTracker::emitUp(aa::TouchEventOut& e, int k, int x, int y) {
    cx_[k] = x;
    cy_[k] = y;
    const int cnt = activeCount();
    fillActive(e, k);
    e.action = cnt <= 1 ? 1 : 6;  // RELEASE / POINTER_UP
    active_[k] = false;
}

int TouchTracker::releaseAll(aa::TouchEventOut* evs, int max) {
    int ne = 0;
    for (int k = 0; k < NS && ne < max; ++k) {
        if (active_[k]) {
            emitUp(evs[ne++], k, cx_[k], cy_[k]);
            needDown_[k] = true;
        }
    }
    return ne;
}

int TouchTracker::feed(const uint8_t* buf, size_t n, aa::TouchEventOut* evs, int max) {
    if (!isTouchPdu(buf, n)) {
        return -1;
    }
    aa::TouchMsg msgs[aa::kTouchMaxMsgs];
    const int nm = reasm_.feed(buf, n, msgs, aa::kTouchMaxMsgs);
    if (nm < 0) {
        return -1;
    }
    int ne = 0;
    for (int mi = 0; mi < nm; ++mi) {
        if (haveMsg_ && msgs[mi].seq0 == prevSeq0_) {
            continue;  // duplicated message: never a second tap
        }
        const bool contiguous = haveMsg_ && msgs[mi].seq0 == static_cast<unsigned char>(prevSeq1_ + 1);
        haveMsg_ = true;
        prevSeq0_ = msgs[mi].seq0;
        prevSeq1_ = msgs[mi].seq1;
        aa::TouchContact c[NS];
        int declared = 0;
        const int nc = aa::parseTouchStream(msgs[mi], c, NS, &declared);
        if (nc < 0) {
            continue;
        }
        ++messages_;
        lastN_ = nc;
        for (int k = 0; k < nc; ++k) {
            last_[k] = c[k];
        }
        int nx[NS], ny[NS], st[NS];
        bool seen[NS];
        for (int j = 0; j < NS; ++j) {
            st[j] = 0;
            seen[j] = false;
            nx[j] = cx_[j];
            ny[j] = cy_[j];
        }
        for (int k = 0; k < nc; ++k) {
            const int id = c[k].id;
            seen[id] = true;
            st[id] = c[k].state;
            nx[id] = clampi(c[k].x * sx_ + ox_, 0, maxX_);
            ny[id] = clampi(c[k].y * sy_ + oy_, 0, maxY_);
        }
        // No gesture may start before the picture: a finger that touched down without it is ignored until it lifts,
        // even once the picture is up. A gesture under way when the picture goes is released where it was.
        for (int k = 0; k < NS; ++k) {
            if (!seen[k] || st[k] == 3) {
                blocked_[k] = false;
            } else if (blocked_[k] || !pictureStarted_) {
                blocked_[k] = true;
                seen[k] = false;
            }
        }
        // A repeated "down" with no message lost is the same touch: a move.
        for (int k = 0; k < NS; ++k) {
            if (seen[k] && st[k] == 1 && active_[k] && contiguous) {
                st[k] = 2;
            }
            if (seen[k] && st[k] != 2) {
                needDown_[k] = false;
            }
        }
        // Lifted (absent) fingers, or a new "down" on an active finger: release first.
        for (int k = 0; k < NS && ne < max; ++k) {
            if (active_[k] && (!seen[k] || st[k] == 1)) {
                emitUp(evs[ne++], k, cx_[k], cy_[k]);
                if (!seen[k]) {
                    needDown_[k] = true;
                }
            }
        }
        for (int k = 0; k < NS && ne < max; ++k) {
            if (seen[k] && st[k] == 2 && !active_[k] && needDown_[k] && contiguous) {
                continue;
            }
            if (seen[k] && (st[k] == 1 || (st[k] == 2 && !active_[k]))) {
                const int before = activeCount();
                active_[k] = true;
                needDown_[k] = false;
                cx_[k] = nx[k];
                cy_[k] = ny[k];
                aa::TouchEventOut& e = evs[ne++];
                fillActive(e, k);
                e.action = before == 0 ? 0 : 5;  // PRESS / POINTER_DOWN
            }
        }
        bool moved = false;
        for (int k = 0; k < NS; ++k) {
            if (seen[k] && st[k] == 2 && active_[k] && (nx[k] != cx_[k] || ny[k] != cy_[k])) {
                cx_[k] = nx[k];
                cy_[k] = ny[k];
                moved = true;
            }
        }
        if (moved && ne < max) {
            fillActive(evs[ne], -1);
            evs[ne++].action = 2;
        }
        for (int k = 0; k < NS && ne < max; ++k) {
            if (seen[k] && st[k] == 3 && active_[k]) {
                emitUp(evs[ne++], k, nx[k], ny[k]);
            }
        }
    }
    return ne;
}

// aa::TouchEventOut actions (aa_input.hpp).
static const int kPress = 0, kRelease = 1, kPointerDown = 5, kPointerUp = 6;

bool PhoneFingers::admit(const aa::TouchEventOut& ev, bool owned) {
    const bool down = ev.action == kPress || ev.action == kPointerDown;
    const bool up = ev.action == kRelease || ev.action == kPointerUp;
    const int id = ev.index >= 0 && ev.index < ev.n ? ev.p[ev.index].id : -1;
    if ((!down && !up) || id < 0 || id >= aa::kTouchMaxContacts) {
        return owned;  // a drag, or no finger to follow
    }
    const unsigned bit = 1u << id;
    if (!owned && !(up && (down_ & bit))) {
        return false;
    }
    if (down) {
        down_ |= bit;
    } else {
        down_ &= ~bit;
    }
    return true;
}

EvoTouch::EvoTouch()
    : mode_(INPUT_OFF),
      cb_(nullptr),
      user_(nullptr),
      started_(false),
      running_(false),
      owned_(false),
      pictureStarted_(true),
      tap_(-1),
      steal_(-1),
      ifindex_(0),
      peerIp_(0),
      peerPort_(0),
      havePeer_(false),
      stealRefused_(false),
      lastFrameMs_(0),
      tracker_(1.0, 1.0, 0.0, 0.0, 1279, 719),
      tapped_(0),
      touchPdus_(0),
      otherPdus_(0),
      events_(0),
      sent_(0),
      stolen_(0),
      rawLogged_(0),
      eventsLogged_(0) {}

EvoTouch::~EvoTouch() {
    stop();
}

static std::string ipText(uint32_t ip) {
    char b[24];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

bool EvoTouch::openTap() {
    tap_ = socket(PF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_IP));
    if (tap_ < 0) {
        ELOG("Touch", "cannot open the AF_PACKET copy: %s", strerror(errno));
        return false;
    }
    // IPv4/UDP, not a fragment, destination port 30567 (offsets from the network header).
    struct sock_filter code[] = {
        {BPF_LD | BPF_B | BPF_ABS, 0, 0, 9},
        {BPF_JMP | BPF_JEQ | BPF_K, 0, 6, 17},
        {BPF_LD | BPF_H | BPF_ABS, 0, 0, 6},
        {BPF_JMP | BPF_JSET | BPF_K, 4, 0, 0x3fff},
        {BPF_LDX | BPF_B | BPF_MSH, 0, 0, 0},
        {BPF_LD | BPF_H | BPF_IND, 0, 0, 2},
        {BPF_JMP | BPF_JEQ | BPF_K, 0, 1, kTouchPort},
        {BPF_RET | BPF_K, 0, 0, 0xffff},
        {BPF_RET | BPF_K, 0, 0, 0},
    };
    struct sock_fprog prog;
    prog.len = sizeof(code) / sizeof(code[0]);
    prog.filter = code;
    if (setsockopt(tap_, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) != 0) {
        ELOG("Touch", "BPF filter refused: %s", strerror(errno));
        close(tap_);
        tap_ = -1;
        return false;
    }
    // Packets queued before the filter was attached are re-checked by parseIpv4Udp; drain them anyway.
    uint8_t junk[64];
    while (recv(tap_, junk, sizeof(junk), MSG_DONTWAIT) > 0) {
    }
    return true;
}

bool EvoTouch::start(InputMode mode, Callback cb, void* user) {
    if (started_ || mode == INPUT_OFF) {
        return mode == INPUT_OFF;
    }
    mode_ = mode;
    cb_ = cb;
    user_ = user;
    if (!openTap()) {
        return false;
    }
    running_ = true;
    if (pthread_create(&thread_, nullptr, threadMain, this) != 0) {
        running_ = false;
        close(tap_);
        tap_ = -1;
        ELOG("Touch", "cannot start the thread");
        return false;
    }
    started_ = true;
    ELOG("Touch", "copy of UDP %u on (read only, nothing sent); mode %s", kTouchPort, inputModeName(mode_));
    return true;
}

void EvoTouch::setOwned(bool owned) {
    owned_ = owned;
}

void EvoTouch::stop() {
    if (!started_) {
        return;
    }
    running_ = false;
    pthread_join(thread_, nullptr);
    started_ = false;
    if (steal_ >= 0) {
        close(steal_);
        steal_ = -1;
        ELOG("Touch", "UDP %u given back to iDrive (stop)", kTouchPort);
    }
    if (tap_ >= 0) {
        close(tap_);
        tap_ = -1;
    }
}

void EvoTouch::summary() const {
    ELOG("Touch",
         "summary: mode=%s datagrams_copied=%u touch_frames=%u others=%u events=%u sent_to_aa=%u "
         "received_by_our_socket=%u sender=%s:%u socket_refused=%u",
         inputModeName(mode_), tapped_, touchPdus_, otherPdus_, events_, sent_, stolen_,
         havePeer_ ? ipText(peerIp_).c_str() : "?", peerPort_, stealRefused_ ? 1 : 0);
}

void* EvoTouch::threadMain(void* self) {
    static_cast<EvoTouch*>(self)->run();
    return nullptr;
}

void EvoTouch::updateSteal(bool want) {
    if (!want && steal_ >= 0) {
        close(steal_);
        steal_ = -1;
        ELOG("Touch", "AA picture hidden or session over: UDP %u given back to iDrive", kTouchPort);
    }
    if (!want || steal_ >= 0 || stealRefused_) {
        return;
    }
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        stealRefused_ = true;
        ELOG("Touch", "cannot create the UDP socket: %s", strerror(errno));
        return;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(kTouchPort);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ELOG("Touch", "UDP %u cannot be shared (%s): copy only, the hidden iDrive gets the touches too", kTouchPort,
             strerror(errno));
        close(s);
        stealRefused_ = true;
        return;
    }
    sockaddr_in p;
    memset(&p, 0, sizeof(p));
    p.sin_family = AF_INET;
    p.sin_addr.s_addr = htonl(peerIp_);
    p.sin_port = htons(peerPort_);
    if (connect(s, reinterpret_cast<sockaddr*>(&p), sizeof(p)) != 0) {
        ELOG("Touch", "connection to the sender %s:%u refused (%s): copy only", ipText(peerIp_).c_str(), peerPort_,
             strerror(errno));
        close(s);
        stealRefused_ = true;
        return;
    }
    steal_ = s;
    ELOG("Touch", "UDP %u connected to the sender %s:%u while the AA picture shows: touches go to AA only", kTouchPort,
         ipText(peerIp_).c_str(), peerPort_);
}

void EvoTouch::emit(const aa::TouchEventOut* ev, int n) {
    const bool forward = mode_ == INPUT_FORWARD && cb_;
    const bool owned = owned_;
    for (int i = 0; i < n; ++i) {
        ++events_;
        const bool send = forward && phone_.admit(ev[i], owned);
        if (eventsLogged_ < 300 && (ev[i].action != 2 || eventsLogged_ < 60)) {
            ++eventsLogged_;
            char line[160];
            int o = 0;
            for (int j = 0; j < ev[i].n && o < 120; ++j) {
                o += snprintf(line + o, sizeof(line) - o, " (%d: %d,%d)", ev[i].p[j].id, ev[i].p[j].x, ev[i].p[j].y);
            }
            line[o < static_cast<int>(sizeof(line)) ? o : static_cast<int>(sizeof(line)) - 1] = 0;
            ELOG("Touch", "%s action=%d idx=%d n=%d:%s", send ? "-> AA" : "decoded (not sent)", ev[i].action,
                 ev[i].index, ev[i].n, line);
        }
        if (send) {
            cb_(ev[i], user_);
            ++sent_;
        }
    }
}

void EvoTouch::handle(const UdpView& u, uint32_t now) {
    if (u.dport != kTouchPort) {
        return;
    }
    if (!isTouchPdu(u.data, u.len)) {
        if (++otherPdus_ <= 10) {
            unsigned id = u.len >= 4
                              ? (static_cast<unsigned>(u.data[0]) << 24 | static_cast<unsigned>(u.data[1]) << 16 |
                                 static_cast<unsigned>(u.data[2]) << 8 | u.data[3])
                              : 0;
            ELOG("Touch", "other PDU 0x%08X (%u B) from %s:%u (left to iDrive)", id, (unsigned)u.len,
                 ipText(u.src).c_str(), u.sport);
        }
        return;
    }
    if (!havePeer_) {
        havePeer_ = true;
        peerIp_ = u.src;
        peerPort_ = u.sport;
        ELOG("Touch", "touch sender: %s:%u", ipText(peerIp_).c_str(), peerPort_);
    } else if (u.src != peerIp_ || u.sport != peerPort_) {
        if (++otherPdus_ <= 10) {
            ELOG("Touch", "touch frame from another sender %s:%u ignored", ipText(u.src).c_str(), u.sport);
        }
        return;
    }
    ++touchPdus_;
    lastFrameMs_ = now;
    const unsigned rawMax = mode_ == INPUT_OBSERVE ? 80 : 12;
    if (rawLogged_ < rawMax) {
        ++rawLogged_;
        char hex[3 * 128 + 1];
        size_t o = 0;
        for (size_t i = 0; i < u.len && i < 128; ++i) {
            o += snprintf(hex + o, sizeof(hex) - o, "%02x ", u.data[i]);
        }
        hex[o] = 0;
        ELOG("Touch", "raw %u B: %s", (unsigned)u.len, hex);
    }
    aa::TouchEventOut ev[16];
    tracker_.setPictureStarted(pictureStarted_);
    const int n = tracker_.feed(u.data, u.len, ev, 16);
    if (n < 0 && rawLogged_ <= rawMax) {
        ELOG("Touch", "touch frame incomplete or not recognised");
    }
    if (n > 0) {
        emit(ev, n);
    }
}

void EvoTouch::run() {
    uint8_t buf[2048];
    while (running_) {
        const bool own = owned_;
        if (mode_ == INPUT_FORWARD) {
            updateSteal(own && havePeer_);
        }
        if (mode_ == INPUT_FORWARD && !own && tracker_.activeCount()) {
            // The picture left with a finger down: the phone gets the lift its press was promised, iDrive nothing.
            aa::TouchEventOut ev[aa::kTouchMaxContacts];
            int n = tracker_.releaseAll(ev, aa::kTouchMaxContacts);
            const bool fingerSent = phone_.any();
            for (int i = 0; i < n; ++i) {
                ++events_;
                if (fingerSent && cb_) {
                    cb_(ev[i], user_);
                    ++sent_;
                }
            }
            if (n && fingerSent) {
                ELOG("Touch", "picture gone with a finger down: %d release(s) sent to AA", n);
            }
            phone_.clear();
        }
        struct pollfd p[2];
        int np = 0;
        p[np].fd = tap_;
        p[np].events = POLLIN;
        p[np].revents = 0;
        ++np;
        if (steal_ >= 0) {
            p[np].fd = steal_;
            p[np].events = POLLIN;
            p[np].revents = 0;
            ++np;
        }
        int r = poll(p, np, 100);
        const uint32_t now = monoMs();
        if (r < 0 && errno != EINTR) {
            ELOG("Touch", "poll: %s", strerror(errno));
            usleep(100000);
            continue;
        }
        if (r > 0 && (p[0].revents & POLLIN)) {
            for (int burst = 0; burst < 64; ++burst) {
                sockaddr_ll ll;
                socklen_t sl = sizeof(ll);
                memset(&ll, 0, sizeof(ll));
                ssize_t got = recvfrom(tap_, buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&ll), &sl);
                if (got <= 0) {
                    break;
                }
                if (ll.sll_pkttype != PACKET_HOST) {
                    continue;
                }
                if (!ifindex_) {
                    ifindex_ = ll.sll_ifindex;
                }
                if (ll.sll_ifindex != ifindex_) {
                    continue;  // the same datagram seen on a bridge/VLAN twin
                }
                UdpView u;
                if (!parseIpv4Udp(buf, static_cast<size_t>(got), u)) {
                    continue;
                }
                ++tapped_;
                handle(u, now);
            }
        }
        if (np > 1 && (p[1].revents & POLLIN)) {
            while (recv(steal_, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
                if (++stolen_ == 1) {
                    ELOG("Touch", "first touch received by our socket: iDrive no longer gets it");
                }
            }
        }
        // Lost "up" frames: release after 400 ms of silence.
        if (tracker_.activeCount() && now - lastFrameMs_ >= 400) {
            aa::TouchEventOut ev[aa::kTouchMaxContacts];
            int n = tracker_.releaseAll(ev, aa::kTouchMaxContacts);
            ELOG("Touch", "no frame for 400 ms: %d finger(s) released", n);
            emit(ev, n);
        }
    }
}

}  // namespace evo
