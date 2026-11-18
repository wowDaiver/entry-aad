// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "egs_reader.hpp"
#include "telemetry_model.hpp"
namespace telemetry {
struct EgsReaderTest {
    static int awaiting(EgsReader& r, unsigned did, double now) {
        r.stop();
        int f[2];
        assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, f));
        for (int i = 0; i < 2; ++i) {
            assert(fcntl(f[i], F_SETFL, O_NONBLOCK) >= 0);
        }
        r.fd_ = f[0];
        r.phase_ = EgsReader::Awaiting;
        r.did_ = did;
        r.identified_ = did == 0x6000;
        r.deadline_ = now + 3;
        r.last_ = now;
        r.frames_ = 0;
        return f[1];
    }
    static void send_bytes(int fd, const unsigned char* p, size_t n) {
        assert(send(fd, p, n, 0) == static_cast<ssize_t>(n));
    }
    static void frame(int fd, const unsigned char* p, size_t n, unsigned control = 1) {
        unsigned char b[512] = {0, 0, 0, 0, 0, 0};
        assert(n <= 506);
        b[2] = n >> 8;
        b[3] = n;
        b[4] = control >> 8;
        b[5] = control;
        memcpy(b + 6, p, n);
        send_bytes(fd, b, n + 6);
    }
    static void unavailable(EgsReader& r) {
        double v = 0;
        bool valid = true;
        assert(r.take(v, valid) && !valid);
        assert(!r.take(v, valid));
        assert(r.fd_ == -1);
    }
    static void run() {
        double v = 999;
        unsigned char b = 0x71;
        assert(EgsReader::temperature(&b, 1, v) && v == 73);
        b = 0;
        assert(EgsReader::temperature(&b, 1, v) && v == -40);
        b = 245;
        assert(EgsReader::temperature(&b, 1, v) && v == 205);
        b = 246;
        assert(!EgsReader::temperature(&b, 1, v));
        b = 255;
        assert(!EgsReader::temperature(&b, 1, v));
        assert(!EgsReader::temperature(0, 1, v) && !EgsReader::temperature(&b, 0, v) &&
               !EgsReader::temperature(&b, 2, v));
        EgsReader r;
        int peer = awaiting(r, 0xf150, 1);
        const unsigned char foreign[] = {0x18, 0xf4, 0x62, 0xf1, 0x50, 0x0f, 0x25, 0xd0};
        frame(peer, foreign, sizeof(foreign));
        r.poll(1.1);
        unavailable(r);
        close(peer);
        peer = awaiting(r, 0xf150, 2);
        const unsigned char ident[] = {0, 0, 0, 8, 0, 1, 0x18, 0xf4, 0x62, 0xf1, 0x50, 0x0f, 0x21, 0xd0};
        for (size_t i = 0; i < sizeof(ident); ++i) {
            send_bytes(peer, ident + i, 1);
            r.poll(2.0 + i * .01);
        }
        assert(r.identified_ && r.phase_ == EgsReader::Idle);
        r.poll(2.2);
        unsigned char got[64];
        ssize_t n = recv(peer, got, sizeof(got), 0);
        const unsigned char wanted[] = {0, 0, 0, 5, 0, 1, 0xf4, 0x18, 0x22, 0x60, 0};
        assert(n == 11 && !memcmp(got, wanted, 11));
        const unsigned char pending[] = {0x18, 0xf4, 0x7f, 0x22, 0x78};
        frame(peer, pending, sizeof(pending));
        r.poll(2.3);
        const unsigned char wrong[] = {0x63, 0xf4, 0x62, 0x60, 0, 0x71};
        frame(peer, wrong, sizeof(wrong));
        r.poll(2.4);
        bool absent = false;
        assert(!r.take(v, absent));  // no value from another ECU
        const unsigned char temp[] = {0x18, 0xf4, 0x62, 0x60, 0, 0x71};
        frame(peer, temp, sizeof(temp));
        r.poll(2.5);
        bool valid = false;
        assert(r.take(v, valid) && valid && v == 73);
        assert(!r.take(v, valid));
        r.poll(7.49);
        assert(recv(peer, got, sizeof(got), 0) < 0 && errno == EAGAIN);
        r.poll(7.5);
        assert(recv(peer, got, sizeof(got), 0) == 11 && !memcmp(got, wanted, 11));
        r.stop();
        r.poll(100);
        assert(r.fd_ == -1 && r.phase_ == EgsReader::Off);
        close(peer);
        peer = awaiting(r, 0x6000, 10);
        const unsigned char huge[] = {0, 0, 2, 0, 0, 1};
        send_bytes(peer, huge, 6);
        r.poll(10.1);
        unavailable(r);
        close(peer);
        peer = awaiting(r, 0x6000, 20);
        frame(peer, pending, sizeof(pending));
        r.poll(22.9);
        r.poll(23);
        unavailable(r);
        close(peer);
        peer = awaiting(r, 0x6000, 30);
        const unsigned char malformed[] = {0x18, 0xf4, 0x62, 0x60, 0, 0x71, 0};
        frame(peer, malformed, sizeof(malformed));
        r.poll(30.1);
        unavailable(r);
        close(peer);
        peer = awaiting(r, 0x6000, 40);
        for (unsigned i = 0; i < 33; ++i) {
            frame(peer, pending, sizeof(pending));
            r.poll(40.0 + i * .01);
        }
        unavailable(r);
        close(peer);
        peer = awaiting(r, 0x6000, 50);
        const unsigned char nrc[] = {0x18, 0xf4, 0x7f, 0x22, 0x31};
        frame(peer, nrc, sizeof(nrc));
        r.poll(50.1);
        unavailable(r);
        close(peer);
        peer = awaiting(r, 0x6000, 60);
        frame(peer, 0, 0, 0x12);
        r.poll(60.1);
        r.poll(60.2);
        n = recv(peer, got, sizeof(got), 0);
        const unsigned char alive[] = {0, 0, 0, 2, 0, 0x13, 0, 0xf4};
        assert(n == 8 && !memcmp(got, alive, 8));
        r.stop();
        close(peer);
        peer = awaiting(r, 0x6000, 70);
        r.poll(69);
        assert(r.phase_ == EgsReader::Off);
        close(peer);
        // TCP may coalesce a transport-alive request and the positive UDS reply.
        // The ACK still has to leave while the temperature transaction is idle.
        peer = awaiting(r, 0x6000, 80);
        const unsigned char alive_and_temperature[] = {0, 0, 0, 0,    0,    0x12, 0,    0, 0,
                                                       6, 0, 1, 0x18, 0xf4, 0x62, 0x60, 0, 0x71};
        send_bytes(peer, alive_and_temperature, sizeof(alive_and_temperature));
        r.poll(80.1);
        assert(r.take(v, valid) && valid && v == 73);
        assert(r.phase_ == EgsReader::Idle);
        r.poll(80.2);
        n = recv(peer, got, sizeof(got), 0);
        assert(n == 8 && !memcmp(got, alive, 8));
        r.stop();
        close(peer);
        // An idle connection must still answer an alive request before the next poll.
        peer = awaiting(r, 0x6000, 90);
        frame(peer, temp, sizeof(temp));
        r.poll(90.1);
        assert(r.take(v, valid) && valid && v == 73);
        frame(peer, 0, 0, 0x12);
        r.poll(90.2);
        r.poll(90.3);
        n = recv(peer, got, sizeof(got), 0);
        assert(n == 8 && !memcmp(got, alive, 8));
        r.poll(95.1);
        n = recv(peer, got, sizeof(got), 0);
        assert(n == 11 && !memcmp(got, wanted, 11));
        r.stop();
        close(peer);
        // Multiple alive requests before identification: ACKs cannot overwrite the
        // queued next DID or erase the verified identity. One ACK per tick is bounded.
        peer = awaiting(r, 0xf150, 100);
        frame(peer, 0, 0, 0x12);
        frame(peer, 0, 0, 0x12);
        send_bytes(peer, ident, sizeof(ident));
        r.poll(100.1);
        assert(r.identified_ && r.phase_ == EgsReader::Idle);
        assert(!r.take(v, valid));
        r.poll(100.2);
        n = recv(peer, got, sizeof(got), 0);
        assert(n == 8 && !memcmp(got, alive, 8));
        r.poll(100.3);
        n = recv(peer, got, sizeof(got), 0);
        assert(n == 19 && !memcmp(got, alive, 8) && !memcmp(got + 8, wanted, 11));
        assert(r.phase_ == EgsReader::Awaiting);
        frame(peer, temp, sizeof(temp));
        r.poll(100.4);
        assert(r.take(v, valid) && valid && v == 73);
        r.stop();
        close(peer);
        // Positive response followed by a fragmented transport ACK is valid TCP.
        peer = awaiting(r, 0x6000, 110);
        frame(peer, temp, sizeof(temp));
        const unsigned char transport_ack[] = {0, 0, 0, 0, 0, 2};
        send_bytes(peer, transport_ack, 3);
        r.poll(110.1);
        assert(r.take(v, valid) && valid && v == 73);
        send_bytes(peer, transport_ack + 3, 3);
        r.poll(110.2);
        assert(r.phase_ == EgsReader::Idle && !r.take(v, valid));
        r.stop();
        close(peer);
        // The reverse ordering (temperature then alive) must not drop a good result.
        peer = awaiting(r, 0x6000, 120);
        frame(peer, temp, sizeof(temp));
        frame(peer, 0, 0, 0x12);
        r.poll(120.1);
        assert(r.take(v, valid) && valid && v == 73);
        r.poll(120.2);
        n = recv(peer, got, sizeof(got), 0);
        assert(n == 8 && !memcmp(got, alive, 8));
        r.stop();
        close(peer);
        // Transport traffic must not extend an active UDS transaction deadline.
        peer = awaiting(r, 0x6000, 130);
        frame(peer, 0, 0, 0x12);
        r.poll(132.8);
        r.poll(132.9);
        n = recv(peer, got, sizeof(got), 0);
        assert(n == 8 && !memcmp(got, alive, 8));
        r.poll(133);
        unavailable(r);
        close(peer);
        // Unsolicited diagnostic replies remain rejected, even after a valid sample.
        peer = awaiting(r, 0x6000, 140);
        frame(peer, temp, sizeof(temp));
        frame(peer, temp, sizeof(temp));
        r.poll(140.1);
        unavailable(r);
        close(peer);
        // A completed transaction no longer owns the deadline. An alive request in
        // its final bytes gets its own bounded send window; waiting UDS does not.
        peer = awaiting(r, 0x6000, 150);
        send_bytes(peer, alive_and_temperature, sizeof(alive_and_temperature));
        r.poll(152.99);
        assert(r.take(v, valid) && valid && v == 73);
        r.poll(153.1);
        n = recv(peer, got, sizeof(got), 0);
        assert(n == 8 && !memcmp(got, alive, 8));
        r.stop();
        close(peer);
    }
};
}  // namespace telemetry
int main() {
    telemetry::EgsReaderTest::run();
    using namespace telemetry;
    Model m(1);
    m.enable_gearbox(1);
    assert(m.status(GearboxTemperature, 1) == Loading);
    m.gearbox(73, true, 2);
    assert(m.status(GearboxTemperature, 2) == Valid && m.reading(GearboxTemperature).value == 73);
    assert(!m.reading(OilTemperature).present);
    assert(m.status(GearboxTemperature, 32) == Stale);
    m.gearbox(72, true, 33);
    assert(m.status(GearboxTemperature, 33) == Valid);
    m.gearbox(0, false, 34);
    assert(m.status(GearboxTemperature, 34) == Stale);
    assert(m.reading(GearboxTemperature).value == 72);
    m.stop(35);
    m.gearbox(71, true, 36);
    assert(m.reading(GearboxTemperature).value == 72);
    puts(
        "EGS: identity gate, 73 C fixture, coalesced/fragmented alive ACKs, idle keepalive, deadlines/budget, 5s "
        "cadence, hide teardown, stale model");
}
