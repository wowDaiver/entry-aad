// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Car GNSS ring reader and location feed on fixture rings of each layout; runs on the PC and on ARM under QEMU.

#include <assert.h>
#include <stdio.h>

#include <limits>
#include <string>

#include "evo_gps.hpp"

static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    memcpy(&b[at], &v, 4);
}
static void save(const char* path, const std::vector<uint8_t>& b) {
    FILE* f = fopen(path, "wb");
    assert(f);
    assert(fwrite(&b[0], 1, b.size(), f) == b.size());
    assert(fclose(f) == 0);
}

static uint64_t varint(const std::vector<uint8_t>& b, size_t& p) {
    uint64_t v = 0;
    for (int s = 0; s < 64 && p < b.size(); s += 7) {
        uint8_t c = b[p++];
        v |= uint64_t(c & 127) << s;
        if (!(c & 128)) {
            return v;
        }
    }
    assert(false);
    return 0;
}
int main(int argc, char** argv) {
    // PC/QEMU oracle driver: emit the actual native-ring decoder's degree conversion + AA encoder.
    // tests/test_gps_wire_oracle.py compares this with independently generated aasdk protobuf messages.
    if (argc == 4 && !strcmp(argv[1], "--wire")) {
        const aa::CarLocation p = evo::gpsDegrees(strtod(argv[2], 0), strtod(argv[3], 0), 1000);
        assert(p.valid);
        const std::vector<uint8_t> b = aa::locationEvent(p);
        for (size_t i = 0; i < b.size(); ++i) {
            printf("%02x", unsigned(b[i]));
        }
        puts("");
        return 0;
    }
    const uint64_t now = aa::locationClockMs();
    if (argc == 9 && !strcmp(argv[1], "--wire-full")) {
        aa::CarLocation f = evo::gpsDegrees(strtod(argv[2], 0), strtod(argv[3], 0), 100000);
        f.hasAccuracy = f.hasAltitude = f.hasSpeed = f.hasBearing = true;
        f.accuracyM = strtod(argv[4], 0);
        f.altitudeM = strtod(argv[5], 0);
        f.speedMps = strtod(argv[6], 0);
        f.bearingDeg = strtod(argv[7], 0);
        f.utcSeconds = strtoull(argv[8], 0, 10);
        const std::vector<uint8_t> w = aa::locationEvent(f);
        for (size_t i = 0; i < w.size(); ++i) {
            printf("%02x", w[i]);
        }
        printf("\n");
        return 0;
    }
    aa::CarLocation p = evo::gpsDegrees(48.123456, -2.123456, now);
    assert(p.valid && p.latitudeE6 == 48123456 && p.longitudeE6 == -2123456);
    assert(p.fresh(now) && !p.fresh(now + 5001) && !p.fresh(now - 1));
    assert(!evo::gpsDegrees(NAN, 2, now).valid && !evo::gpsDegrees(91, 2, now).valid);
    assert(!evo::gpsDegrees(48, 181, now).valid && !evo::gpsDegrees(48, INFINITY, now).valid);
    const std::vector<uint8_t> bytes = aa::locationEvent(p);
    assert(bytes[0] == 0x80 && bytes[1] == 3 && bytes[2] == 0x0a);
    size_t pos = 3;
    const size_t length = varint(bytes, pos);
    assert(length == bytes.size() - pos && bytes[pos++] == 0x10);
    assert(varint(bytes, pos) == 481234560 && bytes[pos++] == 0x18);
    assert(int64_t(varint(bytes, pos)) == -21234560 && pos == bytes.size());  // E7, sign-extended int32, not zigzag
    aa::LocationFeed feed;
    feed.latest = p;
    assert(feed.take(now).empty());  // no unsolicited sensor traffic
    feed.subscribe(10);              // clamp to one fix/second
    assert(feed.take(now).size() == 1 && feed.take(now + 100).empty());
    feed.latest.sampledMs = now + 300;
    assert(feed.take(now + 300).empty());  // new fix, too soon
    feed.latest.sampledMs = now + 850;
    assert(feed.take(now + 850).size() == 1);   // new fix: sent at once
    assert(feed.take(now + 1800).empty());      // same fix: no repeat
    assert(feed.take(now + 2350).size() == 1);  // clear silence: repeated
    feed.latest.sampledMs = now;
    std::vector<std::vector<uint8_t>> m = feed.take(now + 5001);
    assert(m.size() == 1 && m[0] == aa::locationStatus(false));
    assert(feed.take(now + 6000).empty());  // no stale fixes and no error storm
    feed.latest.sampledMs = now + 6500;
    m = feed.take(now + 6500);
    assert(m.size() == 2 && m[0] == aa::locationStatus(true) && m[1][1] == 3);
    feed.reset();
    assert(feed.take(now + 6501).empty());
    feed.subscribe(3000);
    assert(feed.take(now + 6501).size() == 1 && feed.take(now + 7501).empty());
    char path[96];
    snprintf(path, sizeof(path), "/tmp/entry-gps-test-%d", int(getpid()));
    evo::setCarGpsSourceForTest(path);
    // Internal GPS ring: newest complete packet precedes the producer index; deg + minutes/100000.
    std::vector<uint8_t> ring(0x4258, 0);
    put32(ring, 0, 1);
    ring[0x44] = 4;
    ring[0x45] = 0x11;
    put32(ring, 0x48, 0x3a);
    ring[0x4c] = 2;
    ring[0x4c + 5] = 1;
    ring[0x4c + 6] = 48;
    ring[0x4c + 12] = 2;
    put32(ring, 0x4c + 8, 740736);
    put32(ring, 0x4c + 16, 740736);
    save(path, ring);
    assert(evo::carGpsPresent());
    evo::CarGpsReader reader;
    assert(!reader.poll(now).valid);  // initial cache is not proof of a live receiver
    ring[0x4c + 0x30] = 1;
    save(path, ring);  // clock advances while parked; same coordinates
    p = reader.poll(now + 1000);
    assert(p.valid && p.latitudeE6 == 48123456 && p.longitudeE6 == -2123456);
    assert(!reader.poll(now + 6001).valid);  // repeated reads never refresh the timestamp
    ring[0x4c + 0x30] = 2;
    save(path, ring);
    assert(reader.poll(now + 7000).valid);
    // Speed m/s*1000 (+28), course deg*100 u16 (+44), altitude m*10 (+20), as libgnsssv decodes record type 0x11.
    put32(ring, 0x4c + 28, 25000);
    ring[0x4c + 44] = 0x39;
    ring[0x4c + 45] = 0x30;
    put32(ring, 0x4c + 20, 2305);
    save(path, ring);
    p = reader.poll(now + 7250);
    assert(p.valid && p.hasSpeed && p.speedMps == 25.0 && p.hasBearing && p.bearingDeg == 123.45);
    assert(p.hasAltitude && p.altitudeM == 230.5 && p.hasAccuracy && p.accuracyM == 10);
    const std::vector<uint8_t> w25 = aa::locationEvent(p);
    bool speedAsRead = false;  // field 6, 25 m/s = 25000: 0x30 0xA8 0xC3 0x01, never the knots value 48596
    for (size_t i = 0; i + 3 < w25.size(); ++i) {
        if (w25[i] == 0x30 && w25[i + 1] == 0xA8 && w25[i + 2] == 0xC3 && w25[i + 3] == 0x01) {
            speedAsRead = true;
        }
    }
    assert(speedAsRead);
    ring[0x4c + 0x30] = 9;
    save(path, ring);
    p = reader.poll(now + 7500);
    assert(p.hasSpeed && p.speedMps == 25);  // sent as read: no correction factor
    ring[0x4c + 0x30] = 10;
    save(path, ring);
    p = reader.poll(now + 7750);
    assert(p.valid);
    put32(ring, 0x4c + 28, 200);
    ring[0x4c + 0x30] = 3;
    save(path, ring);
    p = reader.poll(now + 8000);
    assert(p.valid && p.hasSpeed && !p.hasBearing);  // nearly stopped: no course
    put32(ring, 0x4c + 28, 200000);
    ring[0x4c + 0x30] = 4;
    save(path, ring);
    p = reader.poll(now + 8250);
    assert(p.valid && !p.hasSpeed);  // impossible value: not sent
    // Android Auto ignores every later speed of the session after one of 70 m/s: just under it goes, 70 does not, nor
    // what rounds to 70000 mm/s on the wire.
    assert(evo::gpsSpeedSendable(0) && evo::gpsSpeedSendable(69.999) && aa::locationScaled(69.999, 1000) == 69999);
    assert(!evo::gpsSpeedSendable(69.9996) && !evo::gpsSpeedSendable(70) && !evo::gpsSpeedSendable(-0.1));
    put32(ring, 0x4c + 28, 69999);
    ring[0x4c + 0x30] = 5;
    save(path, ring);
    p = reader.poll(now + 8400);
    assert(p.valid && p.hasSpeed && p.speedMps < evo::kGpsSpeedLimitMps && p.hasBearing);
    put32(ring, 0x4c + 28, 70000);
    ring[0x4c + 0x30] = 6;
    save(path, ring);
    p = reader.poll(now + 8550);
    assert(p.valid && !p.hasSpeed && !p.hasBearing && p.hasAltitude);  // the position and altitude still go
    put32(ring, 0x4c + 28, 0);
    put32(ring, 0x4c + 20, 0);
    ring[0x4c + 44] = 0;
    ring[0x4c + 45] = 0;
    ring[0x4c] = 0;
    save(path, ring);
    assert(!reader.poll(now + 8700).valid);  // explicit fix loss
    put32(ring, 0, 64);
    save(path, ring);
    assert(!reader.poll(now + 9000).valid);
    // External GPS ring: timestamp, lat/lon doubles, altitude, validity bits; same ABI as CarPlay callback.
    ring.assign(0x6048, 0);
    put32(ring, 0x48, 100);
    double lat = -33.9, lon = 151.2;
    memcpy(&ring[0x50], &lat, 8);
    memcpy(&ring[0x58], &lon, 8);
    put32(ring, 0x64, 3);
    save(path, ring);
    assert(!reader.poll(now + 10000).valid);
    put32(ring, 0x48, 101);
    save(path, ring);
    p = reader.poll(now + 11000);
    assert(p.valid && p.latitudeE6 == -33900000 && p.longitudeE6 == 151200000);
    // This layout has no speed or course: both come from two fixes (0.0004 deg north in 1 s, about 44.5 m/s).
    lat = -33.8996;
    put32(ring, 0x48, 103);
    memcpy(&ring[0x50], &lat, 8);
    save(path, ring);
    p = reader.poll(now + 12000);
    assert(p.valid && p.hasSpeed && p.speedMps > 44 && p.speedMps < 45 && p.hasBearing &&
           (p.bearingDeg < 1 || p.bearingDeg > 359));
    // A fix seen late: 0.0003 deg (33 m) only 450 ms after the last poll, about 74 m/s. Past Android Auto's limit, the
    // estimate gives neither speed nor course.
    lat = -33.8993;
    put32(ring, 0x48, 104);
    memcpy(&ring[0x50], &lat, 8);
    save(path, ring);
    p = reader.poll(now + 12450);
    assert(p.valid && !p.hasSpeed && !p.hasBearing);
    // This car's layout: the receiver's own speed and heading (TGNSSCourse after the position) win over any estimate.
    float realSpeed = 13.9f, realHeading = 87.5f, realAltitude = 231.5f;
    memcpy(&ring[0x70], &realSpeed, 4);
    memcpy(&ring[0x78], &realHeading, 4);
    memcpy(&ring[0x60], &realAltitude, 4);
    put32(ring, 0x7c, 5);
    put32(ring, 0x48, 105);
    save(path, ring);
    p = reader.poll(now + 12600);
    assert(p.valid && p.hasSpeed && fabs(p.speedMps - 13.9) < 0.001 && p.hasBearing && p.bearingDeg == 87.5);
    assert(p.hasAltitude && p.altitudeM == 231.5);
    realSpeed = 0.2f;
    memcpy(&ring[0x70], &realSpeed, 4);
    put32(ring, 0x48, 106);
    save(path, ring);
    p = reader.poll(now + 12750);
    assert(p.valid && p.hasSpeed && !p.hasBearing);  // nearly stopped: no heading
    realSpeed = 75.0f;                               // the receiver's own speed past the limit: left out as well
    memcpy(&ring[0x70], &realSpeed, 4);
    put32(ring, 0x48, 107);
    save(path, ring);
    p = reader.poll(now + 12900);
    assert(p.valid && !p.hasSpeed && !p.hasBearing && p.hasAltitude);
    realSpeed = 13.9f;
    memcpy(&ring[0x70], &realSpeed, 4);
    put32(ring, 0x7c, 0);
    put32(ring, 0x48, 108);
    save(path, ring);
    p = reader.poll(now + 13050);
    assert(p.valid && !p.hasSpeed);  // flags off, fixes too close to estimate: no speed sent
    put32(ring, 0x48, 109);
    put32(ring, 0x64, 1);
    save(path, ring);
    assert(!reader.poll(now + 13200).valid);  // latitude alone is insufficient
    char choice[96];                          // the gps-telephone file: the phone keeps its own GPS
    snprintf(choice, sizeof(choice), "/tmp/entry-gps-phone-%d", int(getpid()));
    evo::phoneGpsFilePath() = choice;
    save(choice, std::vector<uint8_t>());
    assert(!reader.poll(now + 13500).valid && !evo::carGpsPresent());
    unlink(choice);
    unlink(path);
    assert(!reader.poll(now + 14000).valid && !evo::carGpsPresent());
    puts(
        "PASS GPS BMW: native internal/external rings, parked clock, cold cache, validity, expiry/recovery, signed AA "
        "wire, subscription/rate, reset, phone GPS file, every speed sent under 70 m/s");
}
