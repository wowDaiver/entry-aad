// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Golden test of the cluster guidance: every SOME/IP frame the module sends, compared byte for byte with a fixture.

// The module is driven from Android Auto navigation messages to the bytes a fake KOMBI receives on 127.0.0.1: the
// decoder, the graph builder, the guidance state machine with the real SOME/IP client, and the service runtime with
// its threads and files. tests/fixtures/kombi-golden.txt was written by this test from the module as it was before
// its rewrite, so any change in what reaches the car shows up as a difference here.
//   test-kombi-golden                 compare with tests/fixtures/kombi-golden.txt (run from the repository root)
//   test-kombi-golden --write <file>  write the output instead
// Only the adapter section names the module's API.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kombi_nav.hpp"
#include "pb.hpp"

namespace {

using Bytes = std::vector<uint8_t>;

double nowSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

void sleepSeconds(double s) {
    usleep(static_cast<useconds_t>(s * 1e6));
}

std::string hex(const uint8_t* p, size_t n) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += kDigits[p[i] >> 4];
        s += kDigits[p[i] & 15];
    }
    return s;
}

// Text as it appears in the fixture: printable ASCII as is, anything else as \xNN.
std::string quoted(const std::string& s) {
    std::string o = "'";
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7F && c != '\\' && c != '\'') {
            o += static_cast<char>(c);
        } else {
            char b[8];
            snprintf(b, sizeof(b), "\\x%02x", c);
            o += b;
        }
    }
    return o + "'";
}

// ---- the fixture text ----------------------------------------------------------------------------------------------

// Frames are listed one per line; each distinct (method, body) gets a number, and its bytes are written once, where
// it first appears. A request header is written out only when it isn't the expected one (service 0xB006, length,
// client 0x7E01, protocol 1, interface 1, REQUEST, E_OK): the session number is always on the line.
class Golden {
public:
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        text_ += b;
        text_ += '\n';
    }

    void frame(const std::string& where, uint16_t method, const Bytes& body, const uint8_t* header = nullptr) {
        Bytes key;
        key.push_back(static_cast<uint8_t>(method >> 8));
        key.push_back(static_cast<uint8_t>(method));
        key.insert(key.end(), body.begin(), body.end());
        std::map<Bytes, int>::iterator it = ids_.find(key);
        const bool fresh = it == ids_.end();
        const int id = fresh ? static_cast<int>(ids_.size()) + 1 : it->second;
        if (fresh) {
            ids_[key] = id;
        }
        std::string line = "  " + where;
        char b[64];
        if (header) {
            snprintf(b, sizeof(b), " %02x%02x", header[10], header[11]);
            line += b;
        }
        snprintf(b, sizeof(b), " #%d", id);
        line += b;
        if (fresh) {
            snprintf(b, sizeof(b), " %04x:", method);
            line += b + (body.empty() ? std::string("-") : hex(&body[0], body.size()));
        }
        if (header) {
            const uint32_t len = static_cast<uint32_t>(body.size() + 8);
            const uint8_t expect[16] = {0xB0,
                                        0x06,
                                        static_cast<uint8_t>(method >> 8),
                                        static_cast<uint8_t>(method),
                                        static_cast<uint8_t>(len >> 24),
                                        static_cast<uint8_t>(len >> 16),
                                        static_cast<uint8_t>(len >> 8),
                                        static_cast<uint8_t>(len),
                                        0x7E,
                                        0x01,
                                        header[10],
                                        header[11],
                                        1,
                                        1,
                                        0,
                                        0};
            if (memcmp(expect, header, 16) != 0) {
                line += " header=" + hex(header, 16);
            }
        }
        text_ += line + '\n';
    }

    const std::string& text() const { return text_; }

private:
    std::string text_;
    std::map<Bytes, int> ids_;
};

// ---- a fake KOMBI: records every request and answers it -----------------------------------------------------------

struct Received {
    int conn;
    uint8_t header[16];
    Bytes body;
};

class FakeKombi {
public:
    FakeKombi() : listen_(-1), port_(0), stop_(false), total_(0), conns_(0), active_(0), taken_(0) {
        pthread_mutex_init(&mu_, nullptr);
        listen_ = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listen_, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) != 0 || listen(listen_, 16) != 0) {
            fprintf(stderr, "fake KOMBI: cannot listen\n");
            exit(2);
        }
        socklen_t l = sizeof(a);
        getsockname(listen_, reinterpret_cast<struct sockaddr*>(&a), &l);
        port_ = ntohs(a.sin_port);
        pthread_create(&acceptor_, nullptr, &FakeKombi::acceptMain, this);
    }

    ~FakeKombi() {
        pthread_mutex_lock(&mu_);
        stop_ = true;
        pthread_mutex_unlock(&mu_);
        pthread_join(acceptor_, nullptr);
        close(listen_);
        pthread_mutex_lock(&mu_);
        for (std::set<int>::iterator it = live_.begin(); it != live_.end(); ++it) {
            shutdown(*it, SHUT_RDWR);
        }
        pthread_mutex_unlock(&mu_);
        for (int i = 0; i < 500 && activeConnections() > 0; ++i) {
            usleep(10000);
        }
        pthread_mutex_destroy(&mu_);
    }

    uint16_t port() const { return port_; }

    // The n-th request (counted from 1 over every connection) is answered with return code 1.
    void answerWithError(size_t n) {
        pthread_mutex_lock(&mu_);
        errors_.insert(n);
        pthread_mutex_unlock(&mu_);
    }
    // The n-th request gets no answer: its connection is closed instead.
    void hangUpAt(size_t n) {
        pthread_mutex_lock(&mu_);
        hangups_.insert(n);
        pthread_mutex_unlock(&mu_);
    }

    size_t count() {
        pthread_mutex_lock(&mu_);
        const size_t n = total_;
        pthread_mutex_unlock(&mu_);
        return n;
    }

    bool waitFor(size_t n, double seconds) {
        const double end = nowSeconds() + seconds;
        while (count() < n) {
            if (nowSeconds() > end) {
                return false;
            }
            usleep(5000);
        }
        return true;
    }

    std::vector<Received> takeNew() {
        pthread_mutex_lock(&mu_);
        std::vector<Received> out(seen_.begin() + static_cast<long>(taken_), seen_.end());
        taken_ = seen_.size();
        pthread_mutex_unlock(&mu_);
        return out;
    }

private:
    struct Conn {
        FakeKombi* self;
        int fd;
        int id;
    };

    int activeConnections() {
        pthread_mutex_lock(&mu_);
        const int n = active_;
        pthread_mutex_unlock(&mu_);
        return n;
    }

    static void* acceptMain(void* p) {
        FakeKombi* self = static_cast<FakeKombi*>(p);
        for (;;) {
            pthread_mutex_lock(&self->mu_);
            const bool stop = self->stop_;
            pthread_mutex_unlock(&self->mu_);
            if (stop) {
                return nullptr;
            }
            struct pollfd pf;
            pf.fd = self->listen_;
            pf.events = POLLIN;
            pf.revents = 0;
            if (poll(&pf, 1, 20) <= 0) {
                continue;
            }
            const int fd = accept(self->listen_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            Conn* c = new Conn;
            c->self = self;
            c->fd = fd;
            pthread_mutex_lock(&self->mu_);
            c->id = ++self->conns_;
            self->live_.insert(fd);
            ++self->active_;
            pthread_mutex_unlock(&self->mu_);
            pthread_t t;
            pthread_create(&t, nullptr, &FakeKombi::connMain, c);
            pthread_detach(t);
        }
    }

    static bool readAll(int fd, uint8_t* p, size_t n) {
        while (n) {
            const ssize_t r = recv(fd, p, n, 0);
            if (r <= 0) {
                return false;
            }
            p += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }

    static void* connMain(void* p) {
        Conn* c = static_cast<Conn*>(p);
        FakeKombi* self = c->self;
        const int fd = c->fd;
        const int id = c->id;
        delete c;
        for (;;) {
            Received r;
            r.conn = id;
            if (!readAll(fd, r.header, 16)) {
                break;
            }
            const uint32_t len =
                (static_cast<uint32_t>(r.header[4]) << 24) | (r.header[5] << 16) | (r.header[6] << 8) | r.header[7];
            if (len < 8 || len > 65536) {
                break;
            }
            r.body.resize(len - 8);
            if (len > 8 && !readAll(fd, &r.body[0], r.body.size())) {
                break;
            }
            pthread_mutex_lock(&self->mu_);
            const size_t n = ++self->total_;
            const bool error = self->errors_.count(n) != 0;
            const bool hangUp = self->hangups_.count(n) != 0;
            self->seen_.push_back(r);
            pthread_mutex_unlock(&self->mu_);
            if (hangUp) {
                break;
            }
            uint8_t reply[16];
            memcpy(reply, r.header, 16);
            reply[4] = 0;
            reply[5] = 0;
            reply[6] = 0;
            reply[7] = 8;
            reply[14] = 0x80;
            reply[15] = error ? 1 : 0;
            if (send(fd, reply, sizeof(reply), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(reply))) {
                break;
            }
        }
        pthread_mutex_lock(&self->mu_);
        self->live_.erase(fd);
        pthread_mutex_unlock(&self->mu_);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        pthread_mutex_lock(&self->mu_);
        --self->active_;
        pthread_mutex_unlock(&self->mu_);
        return nullptr;
    }

    int listen_;
    uint16_t port_;
    pthread_t acceptor_;
    pthread_mutex_t mu_;
    bool stop_;  // guarded by mu_, like everything below
    size_t total_;
    int conns_;
    int active_;
    std::set<int> live_;
    std::set<size_t> errors_, hangups_;
    std::vector<Received> seen_;
    size_t taken_;
};

void printReceived(Golden& out, const std::vector<Received>& frames) {
    for (size_t i = 0; i < frames.size(); ++i) {
        char where[16];
        snprintf(where, sizeof(where), "c%d", frames[i].conn);
        const uint16_t method = static_cast<uint16_t>((frames[i].header[2] << 8) | frames[i].header[3]);
        out.frame(where, method, frames[i].body, frames[i].header);
    }
}

// ---- Android Auto navigation messages (protobuf, as the phone sends them) ----------------------------------------

Bytes statusMessage(uint64_t status) {
    Bytes o;
    aa::pb::appendUint(o, 1, status);
    return o;
}

struct LaneDir {
    int shape;
    int highlight;  // -1: field absent
};
using Lane = std::vector<LaneDir>;

Lane lane(int shape, int highlight = -1) {
    LaneDir d;
    d.shape = shape;
    d.highlight = highlight;
    return Lane(1, d);
}

Lane lane2(int s1, int h1, int s2, int h2) {
    Lane l = lane(s1, h1);
    l.push_back(lane(s2, h2)[0]);
    return l;
}

// One NavigationStep: {1 maneuver {1 type, 2 exit number, 3 exit angle}, 2 road {1 name}, 3 lanes[], 4 cue {1 text}}.
struct Step {
    int type;
    std::string road;
    std::string cue;
    int exitNumber;  // -1: absent
    bool hasAngle;
    int64_t angle;
    std::vector<Lane> lanes;
    Step(int t, const std::string& r) : type(t), road(r), cue("cue text"), exitNumber(-1), hasAngle(false), angle(0) {}
};

Bytes stepMessage(const Step& s) {
    Bytes maneuver, road, cue, step;
    aa::pb::appendUint(maneuver, 1, static_cast<uint64_t>(static_cast<int64_t>(s.type)));
    if (s.exitNumber >= 0) {
        aa::pb::appendUint(maneuver, 2, static_cast<uint64_t>(s.exitNumber));
    }
    if (s.hasAngle) {
        aa::pb::appendUint(maneuver, 3, static_cast<uint64_t>(s.angle));
    }
    aa::pb::appendMessage(step, 1, maneuver);
    aa::pb::appendString(road, 1, s.road);
    aa::pb::appendMessage(step, 2, road);
    for (size_t i = 0; i < s.lanes.size(); ++i) {
        Bytes l;
        for (size_t k = 0; k < s.lanes[i].size(); ++k) {
            Bytes d;
            aa::pb::appendUint(d, 1, static_cast<uint64_t>(s.lanes[i][k].shape));
            if (s.lanes[i][k].highlight >= 0) {
                aa::pb::appendUint(d, 2, static_cast<uint64_t>(s.lanes[i][k].highlight));
            }
            aa::pb::appendMessage(l, 1, d);
        }
        aa::pb::appendMessage(step, 3, l);
    }
    if (!s.cue.empty()) {
        aa::pb::appendString(cue, 1, s.cue);
        aa::pb::appendMessage(step, 4, cue);
    }
    return step;
}

// NavigationState: the step, the next ones, and a destination.
Bytes stateMessage(const Step& s, const std::vector<Step>& next = std::vector<Step>()) {
    Bytes o, dest;
    aa::pb::appendMessage(o, 1, stepMessage(s));
    for (size_t i = 0; i < next.size(); ++i) {
        aa::pb::appendMessage(o, 1, stepMessage(next[i]));
    }
    aa::pb::appendString(dest, 1, "1 DEMO WAY");
    aa::pb::appendMessage(o, 2, dest);
    return o;
}

// NavigationCurrentPosition: {1 step {1 distance {1 metres, 2 text, 3 unit}, 2 seconds}, 2 destination {...}}.
Bytes positionMessage(bool hasMetres, int64_t metres, const std::string& text, int unit) {
    Bytes d, step, dd, dest, o;
    if (hasMetres) {
        aa::pb::appendUint(d, 1, static_cast<uint64_t>(metres));
    }
    aa::pb::appendString(d, 2, text);
    aa::pb::appendUint(d, 3, static_cast<uint64_t>(unit));
    aa::pb::appendMessage(step, 1, d);
    aa::pb::appendUint(step, 2, 30);
    aa::pb::appendMessage(o, 1, step);
    aa::pb::appendUint(dd, 1, 5000);
    aa::pb::appendString(dd, 2, "3.1");
    aa::pb::appendUint(dd, 3, 5);
    aa::pb::appendMessage(dest, 1, dd);
    aa::pb::appendString(dest, 2, "12:17 AM");
    aa::pb::appendUint(dest, 3, 400);
    aa::pb::appendMessage(o, 2, dest);
    return o;
}

Bytes positionMessage(int64_t metres, const std::string& text, int unit) {
    return positionMessage(true, metres, text, unit);
}

// The display text a phone sends for `metres`: 'm' metric, 'i' miles + feet, 'y' miles + yards.
void phoneText(double metres, char system, std::string& text, int& unit) {
    char b[32];
    const double miles = metres / 1609.344;
    if (system == 'm') {
        if (metres >= 10000) {
            snprintf(b, sizeof(b), "%d", static_cast<int>(metres / 1000));
            unit = 2;
        } else if (metres >= 1000) {
            snprintf(b, sizeof(b), "%.1f", metres / 1000);
            unit = 3;
        } else {
            const int step = metres >= 100 ? 50 : 10;
            snprintf(b, sizeof(b), "%d", static_cast<int>(metres) / step * step);
            unit = 1;
        }
    } else if (miles >= 10) {
        snprintf(b, sizeof(b), "%d", static_cast<int>(miles));
        unit = 4;
    } else if (miles >= 0.1) {
        snprintf(b, sizeof(b), "%.1f", miles);
        unit = 5;
    } else if (system == 'y') {
        snprintf(b, sizeof(b), "%d", static_cast<int>(metres / 0.9144) / 10 * 10);
        unit = 7;
    } else {
        snprintf(b, sizeof(b), "%d", static_cast<int>(metres * 3.28084) / 50 * 50);
        unit = 6;
    }
    text = b;
}

// ---- adapter: the module's API ---------------------------------------------------------------------------------

namespace km = evo::kombi;

// The latest navigation messages, as the service keeps them, and as the phone sent them.
class Inbox {
public:
    Inbox& put(uint16_t id, const Bytes& body) {
        inbox_.keep(id, body.empty() ? reinterpret_cast<const uint8_t*>("") : &body[0], body.size());
        latest_[id] = body;
        return *this;
    }
    Inbox& status(uint64_t s) { return put(0x8003, statusMessage(s)); }
    Inbox& state(const Bytes& b) { return put(0x8006, b); }
    Inbox& position(const Bytes& b) { return put(0x8007, b); }
    const km::NavInbox& inbox() const { return inbox_; }
    const std::map<uint16_t, Bytes>& latest() const { return latest_; }

private:
    km::NavInbox inbox_;
    std::map<uint16_t, Bytes> latest_;
};

Inbox guiding(const Step& s, const Bytes& position = Bytes()) {
    Inbox in;
    in.status(1).state(stateMessage(s));
    if (!position.empty()) {
        in.position(position);
    }
    return in;
}

struct Decoded {
    bool guidance;
    bool ok;
    km::Maneuver view;
};

Decoded decode(const Inbox& in) {
    Decoded d;
    d.guidance = in.inbox().guiding();
    d.ok = in.inbox().nextManeuver(d.view);
    return d;
}

std::string describe(const Decoded& d) {
    const km::Maneuver& v = d.view;
    char b[256];
    std::string step = "-", angle = "-";
    if (v.hasMetres) {
        snprintf(b, sizeof(b), "%lld", static_cast<long long>(v.metres));
        step = b;
    }
    if (v.hasExitAngle) {
        snprintf(b, sizeof(b), "%lld", static_cast<long long>(v.exitAngle));
        angle = b;
    }
    snprintf(b, sizeof(b), "%u[", static_cast<unsigned>(v.lanes.size()));
    std::string lanes = b;
    for (size_t i = 0; i < v.lanes.size(); ++i) {
        if (i) {
            lanes += ',';
        }
        for (size_t k = 0; k < v.lanes[i].size(); ++k) {
            snprintf(b, sizeof(b), "%s%d%s", k ? "/" : "", v.lanes[i][k].shape, v.lanes[i][k].highlighted ? "*" : "");
            lanes += b;
        }
    }
    lanes += "]";
    snprintf(b, sizeof(b), "guidance=%d ok=%d type=%d unit=%d step=%s angle=%s exit=%d lanes=", d.guidance, d.ok,
             v.type, v.distanceUnit, step.c_str(), angle.c_str(), v.exitNumber);
    return b + lanes + " road=" + quoted(v.road) + " dist=" + quoted(v.distanceText) + " cue=" + quoted(v.cue);
}

struct Settings {
    km::GuideSettings s;
    Settings() {}
    // The settings the full-graph tests used: no approach, distances at once, no in-place update.
    static Settings fullGraphs() {
        Settings x;
        x.s.approachMiles = 0;
        x.s.distanceEveryS = 0;
        x.s.inPlace = 0;
        return x;
    }
    Settings& inPlace(int mode) {
        s.inPlace = mode;
        return *this;
    }
    Settings& approach(double miles) {
        s.approachMiles = miles;
        return *this;
    }
    Settings& lanes(bool on) {
        s.lanes = on;
        return *this;
    }
};

// One run of the guidance state machine with the real client, against its own fake KOMBI.
class CoreRun : public km::GuideOutput {
public:
    CoreRun(Golden& out, const char* name, const Settings& s = Settings(), uint16_t port = 0)
        : out_(out),
          graph_(0xC8, s.s.hideBar ? 255 : -1),
          client_("127.0.0.1", port ? port : kombi_.port()),
          core_(graph_, *this, s.s) {
        out_.add("== core: %s", name);
    }

    FakeKombi& kombi() { return kombi_; }

    // One service tick: decoded as the service does it, then a step.
    void tick(double t, const Inbox& in) {
        Decoded d = decode(in);
        core_.step(d.guidance && d.ok ? &d.view : nullptr, t);
        flush("t", t);
    }
    void none(double t) {
        core_.step(nullptr, t);
        flush("t", t);
    }
    void clear(double t) {
        core_.clear("test");
        flush("clear", t);
    }
    void yield(double t) {
        core_.yieldToFactory();
        flush("yield", t);
    }
    void resetStop(double t) {
        core_.resumeAfterStop();
        flush("reset", t);
    }

private:
    km::SendOutcome send(const std::vector<km::Request>& frames, std::string* detail) override {
        return client_.send(frames, detail);
    }
    bool cancel(std::string* detail) override { return client_.cancel(graph_, 0.0, 0.2, nullptr, nullptr, detail); }

    void flush(const char* what, double t) {
        std::vector<Received> frames = kombi_.takeNew();
        char state[96];
        snprintf(state, sizeof(state), "  state shown=%d sent=%d stop=%d cancel=%d", core_.showing(), core_.sentAny(),
                 static_cast<int>(core_.stopped()), core_.lastCancelOk());
        const bool changed = lastState_ != state;
        if (frames.empty() && !changed) {
            return;
        }
        out_.add("%s=%.3f", what, t);
        printReceived(out_, frames);
        if (changed) {
            out_.add("%s", state);
            lastState_ = state;
        }
    }

    Golden& out_;
    FakeKombi kombi_;
    km::RouteGraph graph_;
    km::ClusterLink client_;
    km::Guide core_;
    std::string lastState_;
};

// Graph builder output, without a connection. Nothing is sent after a failure, so only the failure is written.
void printFrames(Golden& out, const char* what, bool ok, const std::vector<km::Request>& frames) {
    if (!ok) {
        out.add("%s: refused", what);
        return;
    }
    out.add("%s: %u frames", what, static_cast<unsigned>(frames.size()));
    for (size_t i = 0; i < frames.size(); ++i) {
        out.frame("g", frames[i].method, frames[i].body);
    }
}

km::ClusterLanes kombiLanes(const char* spec) {  // "1:6 2:1": kind:arrow per lane
    km::ClusterLanes l;
    int kind, arrow, n;
    while (sscanf(spec, " %d:%d%n", &kind, &arrow, &n) == 2) {
        l.push_back(km::ClusterLane(static_cast<uint8_t>(kind), static_cast<uint8_t>(arrow)));
        spec += n;
    }
    return l;
}

// The graph builder through the call shapes the lines below were frozen with.
struct Builder {
    km::RouteGraph graph;
    km::LaneSection section;
    bool sectioned;
    Builder(uint32_t handle, int bar) : graph(handle, bar), sectioned(false) {}
    bool start(int dir, const std::string& road, const std::string& dist, int unit, std::vector<km::Request>& f,
               const km::ClusterLanes* lanes = nullptr) {
        km::LaneSection s;
        const bool ok = graph.routeStart(maneuver0(dir, road, dist, unit, lanes), f, &s);
        keep(ok, lanes, s);
        return ok;
    }
    bool maneuver(int dir, const std::string& road, const std::string& dist, int unit, std::vector<km::Request>& f,
                  const km::ClusterLanes* lanes = nullptr, bool guiding = true) {
        km::LaneSection s;
        const bool ok = graph.maneuverUpdate(maneuver0(dir, road, dist, unit, lanes), guiding, f, &s);
        keep(ok, lanes, s);
        return ok;
    }
    bool distance(const std::string& dist, int unit, std::vector<km::Request>& f) {
        return graph.distanceUpdate(dist, unit, f);
    }
    bool position(double at, std::vector<km::Request>& f) { return graph.positionUpdate(at, f); }
    void cancel(std::vector<km::Request>& f) { graph.cancel(f); }
    void cancelState0(std::vector<km::Request>& f) { graph.cancelGuidance(f); }
    bool feedPosition(bool has, double step, double& pos) {
        if (sectioned) {
            pos = km::lanePosition(section, has, step);
        }
        return sectioned;
    }

private:
    static km::ClusterManeuver maneuver0(int dir, const std::string& road, const std::string& dist, int unit,
                                         const km::ClusterLanes* lanes) {
        return km::ClusterManeuver(dir, road, dist, unit, lanes ? *lanes : km::ClusterLanes());
    }
    void keep(bool ok, const km::ClusterLanes* lanes, const km::LaneSection& s) {
        if (ok && lanes && !lanes->empty()) {
            section = s;
            sectioned = true;
        }
    }
};

void graphSection(Golden& out) {
    out.add("== graph builder");
    std::vector<km::Request> f;
    {
        Builder g(0xC9, 255);
        printFrames(out, "start handle=c9 bar=255 dir=6 'ELM ST' '0.4' unit=1", g.start(6, "ELM ST", "0.4", 1, f), f);
        printFrames(out, "cancel handle=c9", true, (g.cancel(f), f));
        printFrames(out, "cancel state 0 handle=c9", true, (g.cancelState0(f), f));
    }
    {
        Builder g(0xC8, -1);
        printFrames(out, "start bar=template dir=7 'Main St' '600' unit=5", g.start(7, "Main St", "600", 5, f), f);
        printFrames(out, "start bar=template untouched", g.start(7, "", "", -1, f), f);
        printFrames(out, "distance bar=template '12.5' unit=1", g.distance("12.5", 1, f), f);
        printFrames(out, "distance bar=template '' unit=-1", g.distance("", -1, f), f);
    }
    {
        Builder g(0xC8, 255);
        printFrames(out, "start dir=10 utf-8 and emoji '-' unit=-1",
                    g.start(10,
                            "Stra\xc3\x9f"
                            "e \xf0\x9f\x9a\x97",
                            "-", -1, f),
                    f);
        printFrames(out, "start road of 126 characters", g.start(1, std::string(126, 'R'), "1", 3, f), f);
        printFrames(out, "start road of 125 characters", g.start(1, std::string(125, 'R'), "1", 3, f), f);
        printFrames(out, "start unit 0 needs the template's unit byte 1 or 5", g.start(1, "R", "1", 0, f), f);
        printFrames(out, "distance '450' unit=5", g.distance("450", 5, f), f);
        printFrames(out, "distance of 130 characters", g.distance(std::string(130, '9'), 3, f), f);
        const double positions[] = {0.0, 0.5, -1.0, 3952.0, 4132.0, 1e9};
        for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
            char what[64];
            snprintf(what, sizeof(what), "position %.17g", positions[i]);
            printFrames(out, what, g.position(positions[i], f), f);
        }
        km::ClusterLanes one = kombiLanes("1:6"), eight = kombiLanes("2:6 2:6 2:1 1:1 1:1 2:7 2:7 2:9");
        printFrames(out, "start one lane", g.start(6, "TEST ST", "0.3", 1, f, &one), f);
        printFrames(out, "start eight lanes", g.start(7, "MAIN ST", "0.2", 5, f, &eight), f);
        printFrames(out, "maneuver eight lanes", g.maneuver(7, "MAIN ST", "0.2", 5, f, &eight, true), f);
        printFrames(out, "maneuver eight lanes, no guiding model", g.maneuver(7, "MAIN ST", "0.2", 5, f, &eight, false),
                    f);
        printFrames(out, "maneuver no lanes", g.maneuver(2, "OAK", "300", 3, f), f);
        const struct {
            bool has;
            double step;
        } feeds[] = {{true, 395.0}, {false, 0.0}, {true, 10.0}, {true, 500.0},
                     {true, -5.0},  {true, 0.0},  {true, 180.0}};
        for (size_t i = 0; i < sizeof(feeds) / sizeof(feeds[0]); ++i) {
            double pos = -1;
            const bool ok = g.feedPosition(feeds[i].has, feeds[i].step, pos);
            out.add("lane feed has=%d step=%.17g -> ok=%d pos=%.17g", feeds[i].has, feeds[i].step, ok, pos);
        }
    }
}

// ---- decoder ------------------------------------------------------------------------------------------------------

void decodeLine(Golden& out, const char* name, const Inbox& in) {
    out.add("%s: %s", name, describe(decode(in)).c_str());
}

uint64_t fnv(uint64_t h, const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        h ^= static_cast<uint8_t>(s[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

// Random bytes in which no more than three bytes in a row have the high bit set: every varint stays within four
// bytes, so field numbers stay below 2^25 (see the explicit cases for longer ones).
class Random {
public:
    Random() : x_(0x2545F4914F6CDD1DULL), high_(0) {}
    uint32_t next() {
        x_ = x_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(x_ >> 33);
    }
    Bytes bytes(size_t n) {
        Bytes b(n);
        for (size_t i = 0; i < n; ++i) {
            uint8_t c = static_cast<uint8_t>(next() >> 8);
            if (c & 0x80) {
                if (++high_ > 3) {
                    c &= 0x7F;
                    high_ = 0;
                }
            } else {
                high_ = 0;
            }
            b[i] = c;
        }
        return b;
    }

private:
    uint64_t x_;
    int high_;
};

// Bytes from hex digits; spaces are only for reading.
Bytes raw(const char* hexText) {
    Bytes b;
    int high = -1;
    for (const char* p = hexText; *p; ++p) {
        const char c = *p;
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) {
            continue;
        }
        if (high < 0) {
            high = v;
        } else {
            b.push_back(static_cast<uint8_t>(high << 4 | v));
            high = -1;
        }
    }
    return b;
}

void decodeSection(Golden& out) {
    out.add("== decoder");
    // A Google Maps pair seen with the car parked: no metres on the step.
    {
        Bytes man, step, st, d, sd, dd, ddist, cp, road, cue, dest;
        aa::pb::appendUint(man, 1, 1);
        aa::pb::appendMessage(step, 1, man);
        aa::pb::appendString(road, 1, "toward Main St");
        aa::pb::appendMessage(step, 2, road);
        aa::pb::appendString(cue, 1, "toward Main St");
        aa::pb::appendMessage(step, 4, cue);
        aa::pb::appendMessage(st, 1, step);
        aa::pb::appendString(dest, 1, "1 Some Place");
        aa::pb::appendMessage(st, 2, dest);
        aa::pb::appendString(d, 2, "0");
        aa::pb::appendUint(d, 3, 6);
        aa::pb::appendMessage(sd, 1, d);
        aa::pb::appendMessage(cp, 1, sd);
        aa::pb::appendUint(ddist, 1, 44306);
        aa::pb::appendString(ddist, 2, "28");
        aa::pb::appendUint(ddist, 3, 4);
        aa::pb::appendMessage(dd, 1, ddist);
        aa::pb::appendString(dd, 2, "12:17 AM");
        aa::pb::appendUint(dd, 3, 1844);
        aa::pb::appendMessage(cp, 2, dd);
        decodeLine(out, "parked", Inbox().status(1).state(st).position(cp));
    }
    for (uint64_t s = 0; s < 5; ++s) {
        char name[32];
        snprintf(name, sizeof(name), "status %u", static_cast<unsigned>(s));
        decodeLine(out, name, guiding(Step(7, "R"), positionMessage(100, "100", 1)).status(s));
    }
    decodeLine(out, "no status", Inbox().state(stateMessage(Step(7, "R"))));
    decodeLine(out, "status as text", Inbox().put(0x8003, raw("0a0131")).state(stateMessage(Step(7, "R"))));
    decodeLine(out, "status twice, first wins", Inbox().put(0x8003, raw("08010802")).state(stateMessage(Step(7, "R"))));
    decodeLine(out, "status bytes then varint",
               Inbox().put(0x8003, raw("0a00 0801")).state(stateMessage(Step(7, "R"))));
    decodeLine(out, "empty state", Inbox().status(1).put(0x8006, Bytes()));
    decodeLine(out, "unknown message id", Inbox().status(1).put(0x8001, stateMessage(Step(7, "R"))));
    {
        Step s(35, "CIRCLE RD");
        s.exitNumber = 2;
        s.hasAngle = true;
        s.angle = 90;
        s.cue = "Au rond-point, prenez la 2e sortie";
        decodeLine(out, "roundabout", guiding(s, positionMessage(320, "0.2", 5)));
        s.angle = -5;
        s.exitNumber = 0x7FFFFFFF;
        decodeLine(out, "negative angle, huge exit", guiding(s, positionMessage(320, "0.2", 5)));
    }
    {
        Step s(7, "R");
        s.type = -1;
        decodeLine(out, "type -1 (ten-byte varint)", guiding(s));
        s.type = 0x7FFFFFFF;
        decodeLine(out, "type 2^31-1", guiding(s));
    }
    decodeLine(out, "no metres", guiding(Step(8, "MAIN ST"), positionMessage(false, 0, "0.3", 5)));
    decodeLine(out, "negative metres", guiding(Step(8, "MAIN ST"), positionMessage(-5, "0", 1)));
    decodeLine(out, "no position yet", guiding(Step(8, "MAIN ST")));
    {
        Step s(7, "ignored");
        Bytes st = stateMessage(s);
        Bytes text;
        aa::pb::appendString(text, 1, "text");
        decodeLine(out, "step is a string", Inbox().status(1).state(text));
        decodeLine(out, "garbage", Inbox().status(1).state(raw("ffff07")));
        Bytes nested, inner, step, wrapped;
        aa::pb::appendUint(inner, 9, 9);
        aa::pb::appendMessage(nested, 1, inner);
        aa::pb::appendMessage(step, 1, nested);
        aa::pb::appendMessage(wrapped, 1, step);
        decodeLine(out, "unexpected fields", Inbox().status(1).state(wrapped));
        decodeLine(out, "truncated state", Inbox().status(1).state(Bytes(st.begin(), st.begin() + 9)));
        Bytes cut = positionMessage(800, "0.5", 5);
        decodeLine(out, "truncated position", guiding(Step(7, "R"), Bytes(cut.begin(), cut.begin() + 7)));
    }
    {
        Step s(8, "R");
        s.road = std::string(
            "A\x01"
            "B");
        decodeLine(out, "road with a control character", guiding(s));
        s.road = "A\tB";
        decodeLine(out, "road with a tab", guiding(s));
        s.road = "\xff\xfe";
        decodeLine(out, "road not UTF-8", guiding(s));
        s.road =
            "Stra\xc3\x9f"
            "e \xf0\x9f\x9a\x97";
        decodeLine(out, "road UTF-8 and emoji", guiding(s));
        s.road = "";
        decodeLine(out, "empty road", guiding(s));
        s.road = "R";
        s.cue = "";
        decodeLine(out, "no cue", guiding(s));
    }
    {
        // A maneuver whose bytes all look like text: {5: 40 x 'a'} is taken for a string, so there is no maneuver.
        Bytes man, step, st, road;
        aa::pb::appendString(man, 5, std::string(40, 'a'));
        aa::pb::appendMessage(step, 1, man);
        aa::pb::appendString(road, 1, "R");
        aa::pb::appendMessage(step, 2, road);
        aa::pb::appendMessage(st, 1, step);
        decodeLine(out, "maneuver that looks like text", Inbox().status(1).state(st));
        Bytes man2, step2, st2;
        aa::pb::appendUint(man2, 1, 7);
        aa::pb::appendUint(man2, 1, 8);
        aa::pb::appendMessage(step2, 1, man2);
        aa::pb::appendMessage(step2, 1, man);
        aa::pb::appendMessage(st2, 1, step2);
        aa::pb::appendMessage(st2, 1, step);
        decodeLine(out, "repeated fields, first wins", Inbox().status(1).state(st2));
        Bytes man3, step3, st3;
        aa::pb::appendString(man3, 1, "7");
        aa::pb::appendUint(man3, 1, 8);
        aa::pb::appendMessage(step3, 1, man3);
        aa::pb::appendMessage(st3, 1, step3);
        decodeLine(out, "type as bytes then varint", Inbox().status(1).state(st3));
    }
    {
        Step s(7, "R");
        s.lanes.push_back(lane(4, 1));
        s.lanes.push_back(lane(1));
        s.lanes.push_back(lane(1, 0));
        s.lanes.push_back(lane2(1, -1, 5, 2));
        decodeLine(out, "lanes", guiding(s, positionMessage(320, "0.2", 5)));
        s.lanes.push_back(Lane());
        decodeLine(out, "lanes with an empty lane", guiding(s));
        Bytes st = stepMessage(Step(7, "R")), wrapped;
        aa::pb::appendUint(st, 3, 4);                              // a lane as a varint: skipped
        aa::pb::appendMessage(st, 3, raw("0802 0a04 0805 1001"));  // a direction as a varint, then a real one
        aa::pb::appendMessage(st, 3, raw("0a02 0806 1201"));       // a lane cut short after its first direction
        aa::pb::appendMessage(wrapped, 1, st);
        decodeLine(out, "lanes with odd wire types", Inbox().status(1).state(wrapped));
    }
    // The older messages: NextTurnEvent {1 road, 2 side, 3 event, 5 exit, 6 angle}, NextTurnDistanceEvent {1 metres,
    // 2 seconds, 3 display e3, 4 unit}.
    for (int event = 0; event <= 20; ++event) {
        std::string line;
        for (int side = 0; side <= 3; ++side) {
            Bytes ev;
            aa::pb::appendString(ev, 1, "ELM ST");
            if (side) {
                aa::pb::appendUint(ev, 2, static_cast<uint64_t>(side));
            }
            aa::pb::appendUint(ev, 3, static_cast<uint64_t>(event));
            aa::pb::appendUint(ev, 6, 45);
            Decoded d = decode(Inbox().status(1).put(0x8004, ev));
            char b[32];
            snprintf(b, sizeof(b), " %d/%d", d.ok, d.view.type);
            line += b;
        }
        out.add("legacy event %d sides -,1,2,3:%s", event, line.c_str());
    }
    {
        Bytes ev;
        aa::pb::appendString(ev, 1, "ELM ST");
        aa::pb::appendUint(ev, 2, 1);
        aa::pb::appendUint(ev, 3, 4);
        aa::pb::appendUint(ev, 6, 45);
        decodeLine(out, "legacy turn", Inbox().status(1).put(0x8004, ev));
        const int64_t e3s[] = {0, 499, 500, 501, 1500, 2500, 2501, 3500, 200000, 300, 12345, -2500, -3500};
        for (int unit = 0; unit <= 8; ++unit) {
            std::string line;
            for (size_t i = 0; i < sizeof(e3s) / sizeof(e3s[0]); ++i) {
                Bytes de;
                aa::pb::appendUint(de, 1, 300);
                aa::pb::appendUint(de, 2, 20);
                aa::pb::appendUint(de, 3, static_cast<uint64_t>(e3s[i]));
                aa::pb::appendUint(de, 4, static_cast<uint64_t>(unit));
                Decoded d = decode(Inbox().status(1).put(0x8004, ev).put(0x8005, de));
                line += " " + d.view.distanceText;
            }
            out.add("legacy distance unit %d:%s", unit, line.c_str());
        }
        Bytes de;
        aa::pb::appendUint(de, 1, 300);
        decodeLine(out, "legacy metres only", Inbox().status(1).put(0x8004, ev).put(0x8005, de));
        decodeLine(out, "state wins over the legacy turn",
                   Inbox().status(1).put(0x8004, ev).state(stateMessage(Step(8, "MAIN ST"))));
        decodeLine(out, "position wins over the legacy distance",
                   guiding(Step(8, "MAIN ST"), positionMessage(480, "0.3", 5)).put(0x8005, de));
        decodeLine(out, "empty state falls back to the legacy turn",
                   Inbox().status(1).put(0x8004, ev).put(0x8006, Bytes()));
    }
    // Keys of five bytes and more: field numbers above 2^29 - 1, which protobuf does not allow.
    decodeLine(out, "status after a field number of 2^29", Inbox().put(0x8003, raw("8080808010 00 0801")));
    decodeLine(out, "status after a field number of 2^32",
               Inbox().put(0x8003, raw("8080808080 0100 0801")).state(stateMessage(Step(7, "R"))));
    decodeLine(out, "status as field number 2^32 + 1",
               Inbox().put(0x8003, raw("888080808001 01")).state(stateMessage(Step(7, "R"))));
    {
        Bytes st = raw("8080808080 0100");
        Bytes body = stateMessage(Step(7, "R"));
        st.insert(st.end(), body.begin(), body.end());
        decodeLine(out, "state after a field number of 2^32", Inbox().status(1).state(st));
    }
    // Random input, both message generations; one hash per thousand cases.
    Random rnd;
    for (int batch = 0; batch < 20; ++batch) {
        uint64_t h = 1469598103934665603ULL;
        int ok = 0;
        for (int i = 0; i < 1000; ++i) {
            Inbox in;
            in.status(1);
            const uint32_t which = rnd.next();
            if (which & 1) {
                in.state(rnd.bytes(rnd.next() % 48));
            }
            if (which & 2) {
                in.position(rnd.bytes(rnd.next() % 48));
            }
            if (which & 4) {
                in.put(0x8004, rnd.bytes(rnd.next() % 24));
            }
            if (which & 8) {
                in.put(0x8005, rnd.bytes(rnd.next() % 24));
            }
            Decoded d = decode(in);
            ok += d.ok;
            h = fnv(h, describe(d));
        }
        out.add("random batch %d: %d decoded, hash %016llx", batch, ok, static_cast<unsigned long long>(h));
    }
}

// ---- the guidance state machine -----------------------------------------------------------------------------------

struct RouteLeg {
    int type;
    const char* road;
    double metres;
    int angle;  // -1: none
};

// Drives a route at `speed` metres per second, one tick per second, the phone's text in `system` units. The first
// tick has guidance on but no step yet, the last turns it off.
void drive(CoreRun& run, const RouteLeg* legs, size_t n, double speed, char system, double& t) {
    run.tick(t, Inbox().status(1));
    t += 1;
    for (size_t k = 0; k < n; ++k) {
        Step s(legs[k].type, legs[k].road);
        if (legs[k].angle >= 0) {
            s.hasAngle = true;
            s.angle = legs[k].angle;
            s.exitNumber = 1;
        }
        std::vector<Step> next;
        for (size_t j = k + 1; j < n && j < k + 3; ++j) {
            next.push_back(Step(legs[j].type, legs[j].road));
        }
        const Bytes st = stateMessage(s, next);
        for (double left = legs[k].metres; left > 0; left -= speed) {
            std::string text;
            int unit;
            phoneText(left, system, text, unit);
            Inbox in;
            in.status(1).state(st).position(positionMessage(static_cast<int64_t>(left), text, unit));
            run.tick(t, in);
            t += 1;
        }
    }
    run.tick(t, Inbox().status(2));
}

const RouteLeg kDemo[] = {{7, "TEST ST", 800, -1},
                          {8, "MAIN ST", 480, -1},
                          {35, "CIRCLE RD", 320, 90},
                          {5, "OAK AVE", 400, -1},
                          {42, "DESTINATION", 160, -1}};

// One approach to a maneuver: distances from `from` down to 0 every `every` seconds.
void approachSweep(CoreRun& run, double& t, const Step& s, double from, double stepMetres, double every, char system,
                   bool withMetres = true, int forceUnit = -1) {
    const Bytes st = stateMessage(s);
    for (double m = from; m >= 0; m -= stepMetres) {
        std::string text;
        int unit;
        phoneText(m, system, text, unit);
        if (forceUnit >= 0) {
            unit = forceUnit;
        }
        run.tick(
            t, Inbox().status(1).state(st).position(positionMessage(withMetres, static_cast<int64_t>(m), text, unit)));
        t += every;
    }
}

void coreSection(Golden& out) {
    {
        CoreRun r(out, "demo route, full graphs", Settings::fullGraphs());
        double t = 0;
        drive(r, kDemo, 5, 15.0, 'i', t);
        r.none(t + 100);
    }
    {
        CoreRun r(out, "demo route, car settings");
        double t = 0;
        drive(r, kDemo, 5, 15.0, 'i', t);
        r.none(t + 100);
    }
    {
        CoreRun r(out, "demo route, in place without the guiding model", Settings().inPlace(2));
        double t = 0;
        drive(r, kDemo, 5, 15.0, 'i', t);
        r.none(t + 100);
    }
    {
        CoreRun r(out, "demo route, metric, lanes off", Settings().lanes(false));
        double t = 0;
        drive(r, kDemo, 5, 12.0, 'm', t);
        r.none(t + 7.9);
        r.none(t + 8.0);
    }
    {
        CoreRun r(out, "every maneuver type");
        for (int type = 0; type <= 50; ++type) {
            char road[16];
            snprintf(road, sizeof(road), "TYPE %d", type);
            Step s(type, road);
            r.tick(type, guiding(s, positionMessage(400, "0.2", 5)));
        }
    }
    {
        CoreRun r(out, "roundabouts and exits");
        const struct {
            int type;
            const char* cue;
            int exitNumber;
            const char* road;
        } cases[] = {
            {34, "Au rond-point, prenez la 2e sortie", 1, "RUE A"},
            {34, "Prenez la 3\xc3\xa8me sortie", -1, ""},
            {35, "Prenez la deuxi\xc3\xa8me sortie", 4, "RUE B"},
            {33, "Take the 2nd exit", 1, "MAIN ST"},
            {32, "take the third exit", -1, "X"},
            {30, "", 3, "ROUTE"},
            {31, "", 1, "ROUTE"},
            {43, "Prenez la premi\xc3\xa8re sortie", -1, "R"},
            {44, "", 13, "R"},
            {45, "Take the 12th exit", 12, "R"},
            {46, "cinquieme sortie", -1, "ABCDEFGHIJKLMNOPQRSTUVWXYZ ABCDEFGHIJKLMNOPQRS"},
            {8, "prenez la 2e sortie", 4, "PLAIN"},
            {34, "Rue du 8 mai", 2, "R"},
            {34, "A7 sortie 9e", -1, "R"},
            {35, "Latin-1 3\xe8me", -1, "R"},
            {35, "SECOND EXIT", -1, "R"},
            {35, "sixi\xc3\xa8me", 0, "R"},
        };
        double t = 0;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            Step s(cases[i].type, cases[i].road);
            s.cue = cases[i].cue;
            s.exitNumber = cases[i].exitNumber;
            s.hasAngle = true;
            s.angle = static_cast<int64_t>(i * 20);
            r.tick(t, guiding(s, positionMessage(300, "0.2", 5)));
            t += 1;
        }
        // Approach hysteresis: the same roundabout drifting back past 1 mi compares its raw road with the shown one.
        Step s(34, "RUE A");
        s.cue = "prenez la 2e sortie";
        r.tick(t, guiding(s, positionMessage(1500, "0.9", 5)));
        r.tick(t + 1, guiding(s, positionMessage(1700, "1.1", 5)));
        r.tick(t + 2, guiding(s, positionMessage(1500, "0.9", 5)));
        Step plain(7, "RUE A");
        r.tick(t + 3, guiding(plain, positionMessage(1500, "0.9", 5)));
        r.tick(t + 4, guiding(plain, positionMessage(1700, "1.1", 5)));
    }
    {
        CoreRun r(out, "lanes, full graphs", Settings::fullGraphs());
        Step s(7, "TEST ST");
        s.lanes.push_back(lane(4, 1));
        s.lanes.push_back(lane(1));
        s.lanes.push_back(lane(1));
        s.lanes.push_back(lane2(1, -1, 5, -1));
        r.tick(0.0, guiding(s, positionMessage(320, "0.2", 5)));
        r.tick(0.5, guiding(s, positionMessage(150, "500", 6)));
        r.tick(1.2, guiding(s, positionMessage(150, "500", 6)));
        r.tick(2.5, guiding(s, positionMessage(150, "500", 6)));
        r.tick(3.6, guiding(s, positionMessage(10, "50", 6)));
        r.tick(4.0, guiding(s, positionMessage(10, "50", 6)));
        r.tick(4.7, guiding(s, positionMessage(9, "50", 6)));
        r.tick(5.8, guiding(s, positionMessage(-5, "0", 6)));
        r.tick(7.0, guiding(s, positionMessage(false, 0, "0", 6)));
        r.tick(8.2, guiding(s, positionMessage(false, 0, "", 6)));
        Step next(7, "OAK AVE");
        next.lanes.push_back(lane(4));
        next.lanes.push_back(lane(1));
        next.lanes.push_back(lane(5, 1));
        r.tick(8.5, guiding(next, positionMessage(300, "0.2", 5)));
        Step bad(5, "ELM WAY");
        bad.lanes.push_back(lane(0, 1));
        bad.lanes.push_back(lane(2, 1));
        r.tick(9.0, guiding(bad, positionMessage(300, "0.2", 5)));
        Step eight(6, "EIGHT");
        for (int i = 0; i < 8; ++i) {
            eight.lanes.push_back(lane(1 + i % 9, i == 3 ? 1 : 0));
        }
        r.tick(10.0, guiding(eight, positionMessage(300, "0.2", 5)));
        Step nine(6, "NINE");
        for (int i = 0; i < 9; ++i) {
            nine.lanes.push_back(lane(1 + i, i == 3 ? 1 : 0));
        }
        r.tick(11.0, guiding(nine, positionMessage(300, "0.2", 5)));
        Step shapes(6, "SHAPES");
        for (int i = 1; i <= 7; i += 2) {
            shapes.lanes.push_back(lane2(i, 0, i + 1, i == 5 ? 1 : 0));
        }
        r.tick(12.0, guiding(shapes, positionMessage(300, "0.2", 5)));
        r.none(13.0);
        r.none(21.0);
    }
    {
        CoreRun r(out, "lanes appear, change and go, full graphs", Settings::fullGraphs());
        Step plain(8, "MAIN ST");
        Step with = plain;
        with.lanes.push_back(lane(4));
        with.lanes.push_back(lane(1));
        with.lanes.push_back(lane(1));
        with.lanes.push_back(lane2(5, 1, 1, -1));
        r.tick(0.0, guiding(plain, positionMessage(600, "0.4", 5)));
        r.tick(1.0, guiding(with, positionMessage(400, "0.2", 5)));
        r.tick(3.0, guiding(with, positionMessage(390, "0.2", 5)));
        r.tick(3.5, guiding(with, positionMessage(380, "0.2", 5)));
        r.tick(5.0, guiding(with, positionMessage(100, "0.1", 5)));
        Step changed = plain;
        changed.lanes.push_back(lane(4, 1));
        changed.lanes.push_back(lane(1));
        r.tick(6.0, guiding(changed, positionMessage(90, "300", 6)));
        r.tick(7.0, guiding(plain, positionMessage(80, "250", 6)));
        r.tick(9.0, guiding(plain, positionMessage(50, "150", 6)));
        r.tick(10.0, guiding(plain, positionMessage(40, "100", 6)));
    }
    {
        CoreRun r(out, "lanes in place");
        Step plain(8, "MAIN ST");
        Step with = plain;
        with.lanes.push_back(lane(4));
        with.lanes.push_back(lane(1));
        with.lanes.push_back(lane(1));
        with.lanes.push_back(lane2(5, 1, 1, -1));
        r.tick(0.0, guiding(plain, positionMessage(600, "0.4", 5)));
        r.tick(3.5, guiding(with, positionMessage(380, "0.2", 5)));
        r.tick(4.1, guiding(with, positionMessage(370, "0.2", 5)));
        r.tick(4.6, guiding(with, positionMessage(300, "0.2", 5)));
        Step next(7, "OAK AVE");
        next.lanes.push_back(lane(4));
        next.lanes.push_back(lane(1));
        next.lanes.push_back(lane(5, 1));
        r.tick(5.0, guiding(next, positionMessage(300, "0.2", 5)));
        r.tick(9.0, guiding(Step(7, "OAK AVE"), positionMessage(150, "500", 6)));
        r.tick(11.0, guiding(Step(7, "OAK AVE"), positionMessage(50, "150", 6)));
    }
    {
        CoreRun r(out, "metric distances");
        double t = 0;
        approachSweep(r, t, Step(8, "RUE A"), 1700, 47, 0.6, 'm');
        approachSweep(r, t, Step(7, "RUE B"), 1650, 53, 0.4, 'm');
        Settings far;
        far.approach(100);
        CoreRun f(out, "metric distances, far approach", far);
        t = 0;
        approachSweep(f, t, Step(8, "RUE A"), 25000, 731, 0.6, 'm');
        approachSweep(f, t, Step(7, "RUE B"), 1200, 37, 0.6, 'm');
        f.tick(t, guiding(Step(6, "RUE C"), positionMessage(1000, "1,000", 1)));
        f.tick(t + 1, guiding(Step(6, "RUE C"), positionMessage(12500, "12.5", 3)));
        f.tick(t + 2, guiding(Step(6, "RUE C"), positionMessage(5432, "xxxx", 1)));
        f.tick(t + 3, guiding(Step(6, "RUE C"), positionMessage(999, "999", 1)));
        f.tick(t + 4, guiding(Step(6, "RUE C"), positionMessage(999, "999", 2)));
        f.tick(t + 5, guiding(Step(6, "RUE C"), positionMessage(false, 0, "16 km", 3)));
        f.tick(t + 6, guiding(Step(6, "RUE C"), positionMessage(false, 0, "0x10", 1)));
        f.tick(t + 7, guiding(Step(6, "RUE C"), positionMessage(false, 0, "500 ", 1)));
        f.tick(t + 8, guiding(Step(6, "RUE C"), positionMessage(false, 0, "1e3", 1)));
        f.tick(t + 9, guiding(Step(6, "RUE C"), positionMessage(false, 0, "1e4", 1)));
    }
    {
        Settings far;
        far.approach(100);
        CoreRun r(out, "imperial distances", far);
        double t = 0;
        approachSweep(r, t, Step(8, "MAIN ST"), 30000, 913, 0.6, 'i');
        approachSweep(r, t, Step(7, "ELM ST"), 400, 9, 0.6, 'i');
        approachSweep(r, t, Step(6, "YARD RD"), 3000, 97, 0.6, 'y');
        approachSweep(r, t, Step(5, "YARD RD"), 140, 7, 0.6, 'y');
        approachSweep(r, t, Step(4, "UNKNOWN UNIT"), 2500, 113, 0.6, 'i', true, 0);
        approachSweep(r, t, Step(3, "UNIT 8"), 500, 61, 0.6, 'i', true, 8);
        r.tick(t, guiding(Step(9, "TEXT"), positionMessage(305, "1,000", 6)));
        r.tick(t + 1, guiding(Step(9, "TEXT"), positionMessage(false, 0, "12.5", 5)));
        r.tick(t + 2, guiding(Step(9, "TEXT"), positionMessage(false, 0, "10.5", 4)));
        r.tick(t + 3, guiding(Step(9, "TEXT"), positionMessage(false, 0, "1,250", 6)));
        r.tick(t + 4, guiding(Step(9, "TEXT"), positionMessage(false, 0, "abc", 0)));
        r.tick(t + 5, guiding(Step(9, "TEXT"), positionMessage(false, 0, "", 0)));
        r.tick(t + 6, guiding(Step(9, "TEXT"), positionMessage(false, 0, "350", 7)));
        r.tick(t + 7, guiding(Step(9, "TEXT"), positionMessage(false, 0, "1500", 7)));
    }
    {
        CoreRun r(out, "distances without the approach", Settings::fullGraphs().inPlace(1));
        double t = 0;
        r.tick(t, guiding(Step(8, "LONG"), positionMessage(1234, "1,234.5 km", 3)));
        r.tick(t + 1, guiding(Step(8, "LONG"), positionMessage(1234,
                                                               "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                                                               "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9",
                                                               3)));
        r.tick(t + 2, guiding(Step(8, "LONG"), positionMessage(false, 0, "abcdefgh", 0)));
        r.tick(t + 3, guiding(Step(8, "LONG"), positionMessage(5000, "", 0)));
        approachSweep(r, t, Step(7, "UNKNOWN UNIT"), 2000, 211, 0.3, 'i', true, 0);
        approachSweep(r, t, Step(6, "KM"), 2000, 211, 0.3, 'm');
    }
    {
        CoreRun r(out, "approach and hysteresis");
        const double mi = 1609.344;
        Step x(7, "X"), y(8, "Y");
        r.tick(0, guiding(x, positionMessage(static_cast<int64_t>(2.1 * mi), "2.1", 5)));
        r.tick(1, guiding(x, positionMessage(1610, "1.0", 5)));
        r.tick(2, guiding(x, positionMessage(1609, "1.0", 5)));
        r.tick(3, guiding(x, positionMessage(1850, "1.1", 5)));
        r.tick(4, guiding(x, positionMessage(1851, "1.2", 5)));
        r.tick(5, guiding(x, positionMessage(1700, "1.1", 5)));
        r.tick(6, guiding(x, positionMessage(1600, "1.0", 5)));
        r.tick(7, guiding(y, positionMessage(1700, "1.1", 5)));
        r.tick(8, guiding(y, positionMessage(false, 0, "", 0)));
        r.tick(10, guiding(y, positionMessage(false, 0, "", 0)));
        r.tick(11.5, guiding(y, positionMessage(false, 0, "", 0)));
        r.tick(12, guiding(y, positionMessage(false, 0, "1.6", 5)));
        r.tick(13, guiding(y, positionMessage(false, 0, "1.7", 5)));
        r.tick(14, guiding(y, positionMessage(false, 0, "2,000", 6)));
        r.tick(15, guiding(y, positionMessage(false, 0, "6,000", 6)));
        Settings two;
        two.approach(2.0);
        CoreRun f(out, "approach of 2 miles", two);
        const RouteLeg far[] = {{7, "FAR ST", 4500, -1}, {8, "NEAR ST", 400, -1}, {5, "NEXT AVE", 5200, -1}};
        double t = 0;
        drive(f, far, 3, 100.0, 'i', t);
    }
    {
        CoreRun r(out, "distance timing");
        Step s(7, "TEST ST");
        r.tick(0.0, guiding(s));
        r.tick(1.0, guiding(s));
        r.tick(2.99, guiding(s));
        r.tick(3.0, guiding(s));
        r.tick(3.2, guiding(s, positionMessage(480, "0.3", 5)));
        r.tick(3.49, guiding(s, positionMessage(480, "0.3", 5)));
        r.tick(3.5, guiding(s, positionMessage(470, "0.3", 5)));
        r.tick(3.7, guiding(s, positionMessage(300, "0.2", 5)));
        r.tick(4.0, guiding(s, positionMessage(300, "0.2", 5)));
        r.tick(4.5, guiding(s));
        r.tick(5.5, guiding(s, positionMessage(150, "500", 6)));
        r.tick(6.0, guiding(s));
        Step n(8, "NEXT");
        r.tick(7.0, guiding(n));
        r.tick(8.0, guiding(n, positionMessage(100, "300", 6)));
        r.tick(9.0, guiding(n));
        r.tick(11.0, guiding(n, positionMessage(90, "300", 6)));
    }
    {
        CoreRun r(out, "street names");
        const std::string names[] = {
            "",
            std::string(300, 'X'),
            "Stra\xc3\x9f"
            "e",
            "Stra\xc3\x9f"
            "e \xf0\x9f\x9a\x97",
            std::string(39, 'A') + "\xc3\xa9" + "B",
            std::string(41, 'C'),
            "A\x01"
            "B",
            "\xff",
            "A\tB",
            [] {
                std::string e;
                for (int i = 0; i < 60; ++i) {
                    e += "\xc3\xa9";
                }
                return e;
            }(),
            [] {
                std::string e;
                for (int i = 0; i < 45; ++i) {
                    e += "\xf0\x9f\x9a\x97";
                }
                return e;
            }(),
            "Sortie 2 - RUE",
            "same",
            "same ",
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            r.tick(static_cast<double>(i), guiding(Step(7, names[i]), positionMessage(400, "0.2", 5)));
        }
    }
    {
        CoreRun r(out, "guidance gaps, clear and yield");
        Step s(7, "TEST ST"), n(8, "MAIN ST");
        Inbox off;
        off.status(3);
        r.none(0.0);
        r.clear(0.5);
        r.tick(1.0, guiding(s, positionMessage(800, "0.5", 5)));
        r.tick(2.0, off);
        r.tick(9.9, off);
        r.tick(10.0, guiding(s, positionMessage(780, "0.5", 5)));
        r.tick(11.0, guiding(n, positionMessage(700, "0.4", 5)));
        r.tick(12.0, off);
        r.tick(13.0, guiding(n, positionMessage(690, "0.4", 5)));
        r.tick(14.0, guiding(s, positionMessage(690, "0.4", 5)));
        r.tick(15.0, off);
        r.tick(16.0, guiding(n, positionMessage(600, "0.4", 5)));
        r.tick(17.0, guiding(s, positionMessage(600, "0.4", 5)));
        r.tick(18.0, off);
        r.tick(26.0, off);
        r.tick(27.0, guiding(s, positionMessage(500, "0.3", 5)));
        r.tick(28.0, Inbox().status(2));
        r.tick(35.99, Inbox().status(2));
        r.tick(36.0, Inbox().status(2));
        r.tick(37.0, guiding(s, positionMessage(500, "0.3", 5)));
        r.yield(38.0);
        r.none(39.0);
        r.clear(40.0);
        r.tick(41.0, guiding(s, positionMessage(400, "0.2", 5)));
        r.yield(42.0);
        r.tick(43.0, guiding(s, positionMessage(400, "0.2", 5)));
        r.clear(44.0);
        r.clear(45.0);
    }
    {
        CoreRun r(out, "rejected first frame");
        r.kombi().answerWithError(1);
        r.tick(0.0, guiding(Step(7, "TEST ST"), positionMessage(800, "0.5", 5)));
        r.tick(1.0, guiding(Step(7, "TEST ST"), positionMessage(700, "0.4", 5)));
        r.resetStop(2.0);
        r.tick(3.0, guiding(Step(7, "TEST ST"), positionMessage(700, "0.4", 5)));
    }
    {
        CoreRun r(out, "rejected distance", Settings::fullGraphs());
        r.kombi().answerWithError(98);
        r.tick(0.0, guiding(Step(7, "TEST ST"), positionMessage(800, "0.5", 5)));
        r.tick(5.0, guiding(Step(7, "TEST ST"), positionMessage(790, "0.4", 5)));
    }
    {
        CoreRun r(out, "rejected in-place update");
        r.tick(0.0, guiding(Step(7, "TEST ST"), positionMessage(400, "0.3", 5)));
        r.kombi().answerWithError(98);
        r.tick(1.0, guiding(Step(8, "MAIN ST"), positionMessage(400, "0.3", 5)));
        r.tick(2.0, guiding(Step(7, "OAK AVE"), positionMessage(400, "0.3", 5)));
        r.clear(2.5);
        r.tick(3.0, guiding(Step(7, "TEST ST"), positionMessage(400, "0.3", 5)));
        r.tick(4.0, guiding(Step(8, "MAIN ST"), positionMessage(400, "0.3", 5)));
    }
    {
        CoreRun r(out, "cancel falls back to state 0, then fails");
        r.tick(0.0, guiding(Step(7, "TEST ST"), positionMessage(400, "0.3", 5)));
        r.kombi().answerWithError(97 + 3);
        r.clear(1.0);
        r.tick(2.0, guiding(Step(7, "TEST ST"), positionMessage(400, "0.3", 5)));
        const size_t at = 97 + 3 + 2 + 97;
        r.kombi().answerWithError(at + 1);
        r.kombi().answerWithError(at + 2);
        r.clear(3.0);
        r.tick(4.0, guiding(Step(7, "TEST ST"), positionMessage(400, "0.3", 5)));
    }
    {
        CoreRun r(out, "connection lost: one reconnect");
        r.tick(0.0, guiding(Step(7, "TEST ST"), positionMessage(800, "0.5", 5)));
        r.kombi().hangUpAt(98);
        r.tick(1.0, guiding(Step(7, "TEST ST"), positionMessage(700, "0.4", 5)));
        r.kombi().hangUpAt(150);
        r.tick(2.0, guiding(Step(8, "MAIN ST"), positionMessage(700, "0.4", 5)));
        r.kombi().hangUpAt(r.kombi().count() + 1);
        r.kombi().hangUpAt(r.kombi().count() + 2);
        r.tick(3.0, guiding(Step(8, "MAIN ST"), positionMessage(600, "0.3", 5)));
        r.resetStop(4.0);
        r.tick(5.0, guiding(Step(8, "MAIN ST"), positionMessage(600, "0.3", 5)));
    }
    {
        const int closed = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(closed, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
        socklen_t l = sizeof(a);
        getsockname(closed, reinterpret_cast<struct sockaddr*>(&a), &l);
        {
            CoreRun r(out, "nothing listening", Settings(), ntohs(a.sin_port));
            r.tick(0.0, guiding(Step(7, "TEST ST"), positionMessage(800, "0.5", 5)));
            r.tick(1.0, guiding(Step(7, "TEST ST"), positionMessage(700, "0.4", 5)));
        }
        close(closed);
    }
    {
        CoreRun r(out, "older messages");
        double t = 0;
        const struct {
            int event, side;
            const char* road;
        } turns[] = {{3, 1, "ELM ST"}, {3, 2, "OAK ST"}, {11, 2, "CIRCLE"}, {14, 0, "STRAIGHT"}, {19, 0, "END"}};
        for (size_t k = 0; k < sizeof(turns) / sizeof(turns[0]); ++k) {
            Bytes ev;
            aa::pb::appendString(ev, 1, turns[k].road);
            if (turns[k].side) {
                aa::pb::appendUint(ev, 2, static_cast<uint64_t>(turns[k].side));
            }
            aa::pb::appendUint(ev, 3, static_cast<uint64_t>(turns[k].event));
            aa::pb::appendUint(ev, 6, 90);
            for (int m = 600; m > 0; m -= 140) {
                Bytes de;
                aa::pb::appendUint(de, 1, static_cast<uint64_t>(m));
                aa::pb::appendUint(de, 2, 20);
                aa::pb::appendUint(de, 3, static_cast<uint64_t>(m * 1000));
                aa::pb::appendUint(de, 4, k % 2 ? 1 : 6);
                r.tick(t, Inbox().status(1).put(0x8004, ev).put(0x8005, de));
                t += 1;
            }
        }
    }
}

// ---- the service runtime: threads, setting file, marker file, BMW guidance -----------------------------------------

int g_probe = -1;
int probe() {
    return g_probe;
}

class RuntimeRun {
public:
    RuntimeRun(Golden& out, const char* name) : out_(out) {
        out_.add("== runtime: %s", name);
        snprintf(dir_, sizeof(dir_), "/tmp/kombi-goldenXXXXXX");
        if (!mkdtemp(dir_)) {
            fprintf(stderr, "mkdtemp failed\n");
            exit(2);
        }
        off_ = std::string(dir_) + "/kombi-off";
        marker_ = std::string(dir_) + "/kombi-nav.shown";
    }
    ~RuntimeRun() {
        unlink(off_.c_str());
        unlink(marker_.c_str());
        rmdir(dir_);
    }

    FakeKombi& kombi() { return kombi_; }
    void setOff(bool off) { touch(off_, off); }
    void setMarker(bool on) { touch(marker_, on); }
    bool marker() const { return access(marker_.c_str(), F_OK) == 0; }

    km::Arrows* start(bool withProbe) {
        km::ArrowsConfig config;
        config.clusterAddress = "127.0.0.1";
        config.clusterPort = kombi_.port();
        config.offFile = off_;
        config.markerFile = marker_;
        config.singleFile = std::string(dir_) + "/kombi-single-maneuver";
        config.guide.approachMiles = 1.0;
        nav_ = new km::Arrows(config);
        nav_->start(withProbe ? &probe : nullptr);
        return nav_;
    }
    void send(const Inbox& in) {
        const uint16_t order[] = {0x8003, 0x8006, 0x8007};
        for (size_t i = 0; i < 3; ++i) {
            std::map<uint16_t, Bytes>::const_iterator it = in.latest().find(order[i]);
            if (it != in.latest().end()) {
                km::Arrows::onNavStatus(it->first, &it->second[0], it->second.size(), nav_);
            }
        }
    }
    void sessionEnded() { nav_->onSessionEnded(); }
    void fatalSignal() { km::Arrows::onFatalSignal(); }
    void stop() {
        nav_->stop();
        delete nav_;
        nav_ = nullptr;
    }

    // Waits for the KOMBI to have received `total` frames, then `settle` seconds more, and prints what came.
    void expect(const char* what, size_t total, double settle = 0.3) {
        const bool ok = kombi_.waitFor(total, 30.0);
        sleepSeconds(settle);
        out_.add("%s:%s marker=%d", what, ok ? "" : " TIMEOUT", marker());
        printReceived(out_, kombi_.takeNew());
    }

private:
    static void touch(const std::string& path, bool on) {
        if (on) {
            FILE* f = fopen(path.c_str(), "w");
            if (f) {
                fclose(f);
            }
        } else {
            unlink(path.c_str());
        }
    }

    Golden& out_;
    FakeKombi kombi_;
    char dir_[64];
    std::string off_, marker_;
    km::Arrows* nav_ = nullptr;
};

void runtimeSection(Golden& out) {
    const Inbox test = guiding(Step(7, "TEST ST"), positionMessage(800, "0.5", 5));
    {
        RuntimeRun r(out, "marker left by a killed run, setting off");
        r.setMarker(true);
        r.setOff(true);
        r.start(false);
        r.expect("start", 6);
        r.send(test);
        r.expect("guidance while off", 6, 1.5);
        r.stop();
        r.expect("stop", 6, 0.1);
    }
    {
        RuntimeRun r(out, "car settings end to end");
        r.start(false);
        r.send(test);
        r.expect("guidance", 97, 0.6);
        r.send(guiding(Step(7, "TEST ST"), positionMessage(700, "0.4", 5)));
        r.expect("distance", 98);
        r.fatalSignal();
        r.expect("fatal signal", 98 + 6);
        r.sessionEnded();
        r.expect("session ended", 98 + 12);
        r.send(guiding(Step(8, "MAIN ST"), positionMessage(300, "0.2", 5)));
        r.expect("next session", 98 + 12 + 97);
        r.setOff(true);
        r.expect("setting off", 98 + 12 + 97 + 6, 1.5);
        r.send(guiding(Step(8, "MAIN ST"), positionMessage(200, "0.1", 5)));
        r.expect("guidance while off", 98 + 12 + 97 + 6, 1.0);
        r.stop();
        r.expect("stop", 98 + 12 + 97 + 6, 0.1);
    }
    {
        RuntimeRun r(out, "exit with an arrow up");
        r.start(false);
        r.send(guiding(Step(8, "MAIN ST"), positionMessage(300, "0.2", 5)));
        r.expect("guidance", 97, 0.6);
        r.stop();
        r.expect("stop", 97 + 6, 0.1);
        r.fatalSignal();
        r.expect("fatal signal after stop", 97 + 6, 0.5);
    }
    {
        RuntimeRun r(out, "BMW navigation guides");
        g_probe = -1;
        r.start(true);
        r.send(test);
        r.expect("unknown: arrows allowed", 97, 0.6);
        g_probe = 1;
        sleepSeconds(2.6);
        r.send(guiding(Step(7, "TEST ST"), positionMessage(700, "0.4", 5)));
        r.expect("BMW guidance: nothing written", 97, 0.6);
        g_probe = 0;
        r.expect("BMW guidance ended", 97 * 2, 0.3);
        r.stop();
        r.expect("stop", 97 * 2 + 6, 0.1);
        g_probe = -1;
    }
    {
        RuntimeRun r(out, "rejected frame: off until the next session");
        r.kombi().answerWithError(1);
        r.start(false);
        r.send(test);
        r.expect("rejected", 1 + 6, 0.6);
        r.send(guiding(Step(7, "TEST ST"), positionMessage(700, "0.4", 5)));
        r.expect("still off", 1 + 6, 1.0);
        r.sessionEnded();
        sleepSeconds(0.6);
        r.send(test);
        r.expect("next session", 1 + 6 + 97, 0.3);
        r.stop();
        r.expect("stop", 1 + 6 + 97 + 6, 0.1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* fixture = "tests/fixtures/kombi-golden.txt";
    bool write = false;
    if (argc >= 3 && !strcmp(argv[1], "--write")) {
        write = true;
        fixture = argv[2];
    } else if (argc >= 2) {
        fixture = argv[1];
    }
    Golden out;
    out.add("# Frames the cluster guidance sends, written by tests/test_kombi_golden.cpp: see that file.");
    decodeSection(out);
    graphSection(out);
    coreSection(out);
    runtimeSection(out);
    if (write) {
        FILE* f = fopen(fixture, "wb");
        if (!f || fwrite(out.text().data(), 1, out.text().size(), f) != out.text().size()) {
            fprintf(stderr, "cannot write %s\n", fixture);
            return 2;
        }
        fclose(f);
        printf("written %s (%u bytes)\n", fixture, static_cast<unsigned>(out.text().size()));
        return 0;
    }
    std::string want;
    FILE* f = fopen(fixture, "rb");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", fixture);
        return 2;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        want.append(buf, n);
    }
    fclose(f);
    if (want == out.text()) {
        printf("PASS kombi golden: %u bytes of frames identical to %s\n", static_cast<unsigned>(want.size()), fixture);
        return 0;
    }
    size_t line = 1, i = 0;
    while (i < want.size() && i < out.text().size() && want[i] == out.text()[i]) {
        line += want[i] == '\n';
        ++i;
    }
    const size_t a = want.rfind('\n', i == 0 ? 0 : i - 1), b = out.text().rfind('\n', i == 0 ? 0 : i - 1);
    const size_t from = a == std::string::npos ? 0 : a + 1, fromB = b == std::string::npos ? 0 : b + 1;
    fprintf(stderr, "FAIL kombi golden: line %u differs\n  want: %s\n  got:  %s\n", static_cast<unsigned>(line),
            want.substr(from, want.find('\n', from) - from).c_str(),
            out.text().substr(fromB, out.text().find('\n', fromB) - fromB).c_str());
    const char* dump = "/tmp/kombi-golden-actual.txt";
    FILE* d = fopen(dump, "wb");
    if (d) {
        fwrite(out.text().data(), 1, out.text().size(), d);
        fclose(d);
        fprintf(stderr, "  whole output in %s\n", dump);
    }
    return 1;
}
