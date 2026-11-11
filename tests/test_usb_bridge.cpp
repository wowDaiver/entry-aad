// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The USB bridge between a fake phone and a socket: frames both ways, the empty packet, and each way it ends.
#include <sys/socket.h>

#include "aa_frame.hpp"
#include "usb_accessory.hpp"
#include "usb_bridge.hpp"
#include "usb_fakes.hpp"

namespace {

typedef std::vector<uint8_t> Bytes;

evo::AccessoryEndpoints endpoints() {
    evo::AccessoryEndpoints e;
    e.in = 0x81;
    e.out = 0x01;
    e.outPacket = 512;
    return e;
}

// A whole frame of that size on the wire (4 header bytes included).
Bytes frame(size_t wireSize, uint8_t fill) {
    const Bytes payload(wireSize - 4, fill);
    Bytes out;
    CHECK(aa::encodeFrame(3, aa::frame::kPlain, &payload[0], payload.size(), out) && out.size() == wireSize);
    return out;
}

struct Link {
    fake::Phone phone;
    int session;  // the session's end of the socket pair
    evo::UsbBridge* bridge;
    Link() {
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        session = fds[1];
        bridge = new evo::UsbBridge(new fake::Device(phone), endpoints(), fds[0]);
        CHECK(bridge->start());
    }
    ~Link() {
        delete bridge;
        if (session >= 0) {
            close(session);
        }
    }
    void sessionSends(const Bytes& bytes) { CHECK(send(session, &bytes[0], bytes.size(), 0) == ssize_t(bytes.size())); }
    // What the session reads within 2 s, up to `size` bytes; fewer when the bridge closed its end.
    Bytes sessionReads(size_t size) {
        Bytes got;
        pollfd p = {session, POLLIN, 0};
        while (got.size() < size && poll(&p, 1, 2000) == 1) {
            uint8_t bytes[4096];
            const ssize_t n = recv(session, bytes, std::min(sizeof(bytes), size - got.size()), 0);
            if (n <= 0) {
                break;
            }
            got.insert(got.end(), bytes, bytes + n);
        }
        return got;
    }
    bool sessionSeesEnd() {
        pollfd p = {session, POLLIN, 0};
        uint8_t byte;
        return poll(&p, 1, 2000) == 1 && recv(session, &byte, 1, 0) <= 0;
    }
    bool finished() {
        return fake::within(2000, [this]() { return bridge->finished(); });
    }
};

void toThePhone() {
    Link link;
    // Two frames and the start of a third in one write: one transfer per whole frame, the third when it is whole.
    const Bytes a = frame(20, 0xa1), b = frame(300, 0xb2), c = frame(40, 0xc3);
    Bytes glued = a;
    glued.insert(glued.end(), b.begin(), b.end());
    glued.insert(glued.end(), c.begin(), c.begin() + 10);
    link.sessionSends(glued);
    CHECK(fake::within(2000, [&]() { return link.phone.received().size() == 2; }));
    usleep(50000);
    CHECK(link.phone.received().size() == 2);
    link.sessionSends(Bytes(c.begin() + 10, c.end()));
    CHECK(fake::within(2000, [&]() { return link.phone.received().size() == 3; }));
    // A frame that fills its last packet is followed by an empty one; the others are not.
    link.sessionSends(frame(1024, 0xd4));
    link.sessionSends(frame(21, 0xe5));
    CHECK(fake::within(2000, [&]() { return link.phone.received().size() == 6; }));
    const std::vector<Bytes> got = link.phone.received();
    CHECK(got[0] == a && got[1] == b && got[2] == c && got[3] == frame(1024, 0xd4) && got[4].empty() &&
          got[5] == frame(21, 0xe5));
    CHECK(fake::within(2000, [&]() { return link.bridge->bytesToPhone() == 20 + 300 + 40 + 1024 + 21; }));
}

void fromThePhone() {
    Link link;
    Bytes all;
    for (unsigned i = 0; i < 40; ++i) {
        const Bytes chunk(i % 3 == 0 ? 16384 : 700 + i, static_cast<uint8_t>(i));
        link.phone.say(chunk);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    CHECK(link.sessionReads(all.size()) == all);
    CHECK(link.bridge->bytesFromPhone() == all.size() && !link.bridge->finished());
}

// The session does not read: the phone's data waits, nothing is lost, and frames still go to the phone.
void slowSession() {
    Link link;
    Bytes all;
    for (unsigned i = 0; i < 64; ++i) {
        const Bytes chunk(16384, static_cast<uint8_t>(i));
        link.phone.say(chunk);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    usleep(200000);
    link.sessionSends(frame(100, 0x11));
    CHECK(fake::within(2000, [&]() { return link.phone.received().size() == 1; }));
    CHECK(link.sessionReads(all.size()) == all && !link.bridge->finished());
}

void phoneUnplugged() {
    Link link;
    link.phone.say(Bytes(10, 1));
    CHECK(link.sessionReads(10).size() == 10);
    link.phone.set(&fake::Phone::unplugged, true);
    CHECK(link.sessionSeesEnd() && link.finished() && link.bridge->phoneLeft());
    CHECK(link.phone.count(&fake::Phone::releases) == 1);
}

void sessionCloses() {
    Link link;
    link.sessionSends(frame(50, 7));
    CHECK(fake::within(2000, [&]() { return link.phone.received().size() == 1; }));
    close(link.session);
    link.session = -1;
    CHECK(link.finished() && !link.bridge->phoneLeft());
    CHECK(link.phone.count(&fake::Phone::releases) == 1);
}

// A phone that takes no data ends the bridge after 5 s, not before.
void phoneTakesNothing() {
    Link link;
    link.phone.set(&fake::Phone::stuckOut, true);
    const uint32_t begin = fake::nowMs();
    link.sessionSends(frame(60, 9));
    CHECK(fake::within(8000, [&]() { return link.bridge->finished(); }));
    const uint32_t lasted = fake::nowMs() - begin;
    CHECK(lasted >= 4500 && lasted < 7000 && link.bridge->phoneLeft() && link.sessionSeesEnd());
}

// Stopped by the service with a read in flight, and without ever being started.
void stopped() {
    {
        Link link;
        usleep(50000);
        delete link.bridge;
        link.bridge = nullptr;
        CHECK(link.phone.count(&fake::Phone::releases) == 1 && link.sessionSeesEnd());
    }
    fake::Phone phone;
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    delete new evo::UsbBridge(new fake::Device(phone), endpoints(), fds[0]);
    uint8_t byte;
    CHECK(recv(fds[1], &byte, 1, 0) == 0);
    close(fds[1]);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const int sink = open("/dev/null", O_WRONLY);  // the bridge's own log lines
    const int console = dup(1);
    CHECK(sink >= 0 && console >= 0 && dup2(sink, 1) >= 0);
    toThePhone();
    fromThePhone();
    slowSession();
    phoneUnplugged();
    sessionCloses();
    stopped();
    phoneTakesNothing();
    fflush(stdout);
    dup2(console, 1);
    puts("PASS USB bridge: one frame per transfer, empty packet, both ways under load, unplug, session end, stop");
    return 0;
}
