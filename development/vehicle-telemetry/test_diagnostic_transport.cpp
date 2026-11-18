// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "egs_reader.hpp"
namespace telemetry {
struct EgsReaderTest {
    static int setup(EgsReader& r) {
        r.start(0);
        int f[2];
        assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, f));
        for (unsigned i = 0; i < 2; ++i) {
            assert(fcntl(f[i], F_SETFL, O_NONBLOCK) >= 0);
        }
        r.fd_ = f[0];
        r.phase_ = EgsReader::Awaiting;
        r.did_ = 0xf150;
        r.deadline_ = 3;
        r.target_ = r.ecu_ ? r.ecu_ : 0x12;
        return f[1];
    }
    static void frame(int peer, unsigned ecu, unsigned did, const unsigned char* data, unsigned size) {
        unsigned char raw[32] = {0, 0, 0, 0, 0, 1};
        raw[3] = 5 + size;
        raw[6] = ecu;
        raw[7] = 0xf4;
        raw[8] = 0x62;
        raw[9] = did >> 8;
        raw[10] = did;
        memcpy(raw + 11, data, size);
        assert(send(peer, raw, 11 + size, 0) == 11 + size);
    }
    static void request(int peer, unsigned ecu, unsigned did) {
        unsigned char raw[32];
        assert(recv(peer, raw, sizeof(raw), 0) == (did == 0x0104 ? 10 : 11));
        assert(raw[3] == (did == 0x0104 ? 4 : 5) && raw[5] == 1 && raw[6] == 0xf4 && raw[7] == ecu);
        if (did == 0x0104) {
            assert(raw[8] == 1 && raw[9] == 4);
        } else {
            assert(raw[8] == 0x22 && raw[9] == did >> 8 && raw[10] == (did & 255));
        }
    }
    static void run() {
        EgsReader dde(0x12, true);
        int peer = setup(dde);
        const unsigned char identity[] = {15, 31, 240};
        frame(peer, 0x12, 0xf150, identity, 3);
        dde.poll(.1);
        assert(dde.identified_);
        dde.poll(.2);
        request(peer, 0x12, 0x42b9);
        const unsigned char oil[] = {5, 7};
        frame(peer, 0x12, 0x42b9, oil, 2);
        dde.poll(.4);
        DiagnosticReply reply = {};
        assert(dde.take_record(reply) && reply.valid && reply.did == 0x42b9 && reply.ecu == 0x12);
        dde.poll(.6);
        request(peer, 0x12, 0x43c0);
        const unsigned char nrc[] = {0, 0, 0, 5, 0, 1, 0x12, 0xf4, 0x7f, 0x22, 0x31};
        assert(send(peer, nrc, sizeof(nrc), 0) == sizeof(nrc));
        dde.poll(.8);
        assert(dde.take_record(reply) && !reply.valid && reply.did == 0x43c0 && dde.fd_ >= 0);
        assert(dde.next_due_[1] >= 30.8);  // one bad record cannot starve the others
        dde.poll(1);
        request(peer, 0x12, 0x5868);
        const unsigned char temp[] = {0x86};
        frame(peer, 0x12, 0x5868, temp, 1);
        dde.poll(1.2);
        assert(dde.take_record(reply) && reply.valid);
        dde.poll(1.4);
        request(peer, 0x12, 0x5866);
        const unsigned char voltage[] = {0x39, 0xe1};
        frame(peer, 0x12, 0x5866, voltage, 2);
        dde.poll(1.6);
        assert(dde.take_record(reply) && reply.valid);
        dde.poll(1.8);
        request(peer, 0x12, 0x0104);
        const unsigned char obd[] = {0, 0, 0, 5, 0, 1, 0x12, 0xf4, 0x41, 4, 0x85};
        assert(send(peer, obd, sizeof(obd), 0) == sizeof(obd));
        dde.poll(2);
        assert(dde.take_record(reply) && reply.valid);
        dde.poll(2.2);
        request(peer, 0x12, 0x43c2);
        const unsigned char soc[] = {0xd6};
        frame(peer, 0x12, 0x43c2, soc, 1);
        dde.poll(2.4);
        assert(dde.take_record(reply) && reply.valid);
        dde.poll(2.6);
        request(peer, 0x12, 0x48a5);
        const unsigned char torque[] = {0x5d, 0x19};
        frame(peer, 0x12, 0x48a5, torque, 2);
        dde.poll(2.8);
        assert(dde.take_record(reply) && reply.valid);
        dde.poll(3);
        request(peer, 0x12, 0x40c0);
        const unsigned char igr[16] = {3, 0x68, 0x99, 0xfc};
        frame(peer, 0x12, 0x40c0, igr, 16);
        dde.poll(3.2);
        assert(dde.take_record(reply) && reply.valid && reply.size == 16);
        assert(!memcmp(reply.data, igr, 16));
        dde.poll(3.4);
        request(peer, 0x12, 0x42b9);
        frame(peer, 0x12, 0x42b9, oil, 2);
        dde.poll(3.6);
        assert(dde.take_record(reply) && reply.valid);
        unsigned char buffer[64];
        close(peer);
        dde.poll(3.8);
        assert(dde.take_record(reply) && !reply.valid && reply.did == 0);
        assert(dde.phase_ == EgsReader::Backoff && dde.due_ >= 4.8 && dde.due_ < 4.9);
        dde.stop();
        dde.poll(100);
        assert(dde.fd_ < 0 && dde.phase_ == EgsReader::Off);
        EgsReader wrong(0x12, true);
        peer = setup(wrong);
        const unsigned char other[] = {15, 31, 232};
        frame(peer, 0x12, 0xf150, other, 3);
        wrong.poll(.1);
        assert(wrong.fd_ < 0 && !wrong.identified_);
        assert(recv(peer, buffer, sizeof(buffer), 0) == 0);
        close(peer);
        wrong.stop();
        // One socket, two strictly ordered identities, then mixed-ECU requests.
        EgsReader unified(0, true);
        peer = setup(unified);
        frame(peer, 0x12, 0xf150, identity, 3);
        unified.poll(.1);
        assert(!unified.identified_ && unified.identity_mask_ == 1);
        request(peer, 0x18, 0xf150);
        const unsigned char second_id[] = {15, 33, 208};
        frame(peer, 0x18, 0xf150, second_id, 3);
        unified.poll(.3);
        assert(unified.identified_);
        const unsigned mixed[] = {0x42b9, 0x43c0, 0x5868, 0x5866, 0x0104, 0x6000, 0xda2e, 0xda22, 0xda2a,
                                  0x6009, 0x600a, 0x600c, 0x43c2, 0x48a5, 0xda27, 0x40c0, 0xda31};
        unsigned count = 0;
        const DiagnosticSource* all = diagnostic_sources(0, true, count);
        assert(count == 17);
        for (unsigned i = 0; i < count; ++i) {
            unsigned ecu = diagnostic_ecu(mixed[i]);
            request(peer, ecu, mixed[i]);
            unsigned char sample[16] = {};
            if (mixed[i] == 0x0104) {
                assert(send(peer, obd, sizeof(obd), 0) == sizeof(obd));
            } else {
                frame(peer, ecu, mixed[i], sample, all[i].size);
            }
            unified.poll(.5 + i * .2);
            assert(unified.take_record(reply) && reply.valid && reply.did == mixed[i] && reply.ecu == ecu);
        }
        double completed = .5 + (count - 1) * .2;
        request(peer, 0x12, 0x42b9);
        unified.poll(completed + .1);
        assert(recv(peer, buffer, sizeof(buffer), 0) < 0);  // no pipelined/catch-up requests
        unified.poll(completed + 3.01);
        assert(unified.take_record(reply) && !reply.valid && reply.ecu == 0 && reply.failure == ResponseTimeout);
        assert(unified.due_ > completed + 4 && unified.due_ < completed + 4.1);
        unified.stop();
        close(peer);
        EgsReader egs(0x18, true);
        peer = setup(egs);
        const unsigned char egs_id[] = {15, 33, 208};
        frame(peer, 0x18, 0xf150, egs_id, 3);
        egs.poll(.1);
        assert(egs.identified_);
        const unsigned dids[] = {0x6000, 0xda2e, 0xda22, 0xda2a, 0x6009, 0x600a, 0x600c, 0xda27, 0xda31};
        const unsigned sizes[] = {1, 2, 2, 4, 1, 1, 1, 2, 2};
        const unsigned char data[][4] = {{0x4a}, {0, 0}, {0, 0}, {2, 0xf1, 0, 0}, {0x17}, {0x18}, {1}, {7, 0}, {0, 1}};
        for (unsigned i = 0; i < 9; ++i) {
            double now = .2 + i * .4;
            egs.poll(now);
            request(peer, 0x18, dids[i]);
            frame(peer, 0x18, dids[i], data[i], sizes[i]);
            egs.poll(now + .2);
            assert(egs.take_record(reply) && reply.valid && reply.did == dids[i]);
        }
        // Bad length affects only the matching source, not identity/connection.
        for (unsigned i = 0; i < 9; ++i) {
            egs.next_due_[i] = 100;
        }
        egs.next_due_[3] = 3.8;
        egs.poll(3.8);
        request(peer, 0x18, 0xda2a);
        frame(peer, 0x18, 0xda2a, oil, 2);
        egs.poll(4);
        assert(egs.take_record(reply) && !reply.valid && reply.did == 0xda2a && egs.identified_);
        assert(egs.next_due_[3] >= 34);
        egs.stop();
        close(peer);
    }
};
}  // namespace telemetry
int main() {
    telemetry::EgsReaderTest::run();
    puts(
        "diagnostic transport: DDE/EGS identity, fixed allowlists, fair rotation, NRC/length isolation, no bursts, "
        "teardown/backoff");
}
