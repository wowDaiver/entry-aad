// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Passive reader of the GNSS ring consumed by BMW navigation and CarPlay.

// Record layouts as this car's libgnsssv.so.1.0.0 writes them. Read-only: no registration, semaphore or network write.
#ifndef ENTRY_AAD_PLATFORM_EVO_GPS_HPP
#define ENTRY_AAD_PLATFORM_EVO_GPS_HPP
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "car_location.hpp"
#include "evo_log.hpp"
#include "service_contract.hpp"

namespace evo {
// Where the GNSS ring is read: the car's shared memory. Tests point it at a fixture (setCarGpsSourceForTest).
inline std::string& carGpsSourcePath() {
    static std::string path("/dev/shm/Shm_GnsssvGpsInfo");
    return path;
}
inline const char* carGpsSource() {
    return carGpsSourcePath().c_str();
}
inline void setCarGpsSourceForTest(const char* path) {
    carGpsSourcePath() = path;
}
// Present = the driver chose the phone's own GPS (a car whose receiver stays poor). Tests point it elsewhere.
inline std::string& phoneGpsFilePath() {
    static std::string path(contract::kPhoneGps);
    return path;
}
inline bool phoneGpsChosen() {
    return access(phoneGpsFilePath().c_str(), F_OK) == 0;
}
inline bool gpsLayout(size_t n) {
    return n == 0x4258 || n == 0x6048;
}
inline bool carGpsPresent() {
    struct stat s;
    return !phoneGpsChosen() && stat(carGpsSource(), &s) == 0 && gpsLayout(s.st_size);
}
// Android Auto 17.9 stops using the speed of the car's fixes for the rest of the session once one says 70 m/s or more
// (found in its location code by Supra-iDrive6-Android-Auto, c2ad815). Every speed sent stays below it, once rounded
// to the mm/s it goes in; a faster one is left out, never clamped.
const double kGpsSpeedLimitMps = 70;
inline bool gpsSpeedSendable(double speed) {
    return speed >= 0 && speed < kGpsSpeedLimitMps - 0.0005;  // 69.9996 would go as 70000 mm/s
}
inline uint32_t gpsU32(const uint8_t* p) {
    uint32_t n;
    memcpy(&n, p, 4);
    return n;
}
inline aa::CarLocation gpsDegrees(double lat, double lon, uint64_t now) {
    aa::CarLocation p;
    if (!isfinite(lat) || !isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        return p;
    }
    p.latitudeE6 = static_cast<int32_t>(lat * 1000000 + (lat < 0 ? -0.5 : 0.5));
    p.longitudeE6 = static_cast<int32_t>(lon * 1000000 + (lon < 0 ? -0.5 : 0.5));
    p.sampledMs = now;
    p.valid = true;
    return p;
}
// Keep the latest record even if invalid: never resurrect an older fix.
// External slot: TGNSSPosition (32 bytes) then TGNSSCourse (24 bytes), the two structs libgnsssv hands to CarPlay
// (its SOME/IP data thread takes the position at +0x48 and the course at +0x68 of the slot's shared-memory address).
const size_t kGpsExternalRecord = 32 + 24;
inline std::vector<uint8_t> gpsRecord(const uint8_t* b, size_t size) {
    if (size == 0x4258) {
        const uint32_t next = gpsU32(b);
        if (next >= 64) {
            return std::vector<uint8_t>();
        }
        for (unsigned age = 1; age <= 63; ++age) {
            const uint8_t* r = b + 0x44 + ((next + 64 - age) % 64) * 0x108;
            const uint32_t len = gpsU32(r + 4);
            if (r[0] == 4 && r[1] == 0x11 && len >= 0x3a && len <= 256) {
                return std::vector<uint8_t>(r + 8, r + 8 + 0x3a);
            }
        }
    } else if (size == 0x6048) {
        const uint8_t* newest = nullptr;
        uint64_t stamp = 0;
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t* r = b + 0x48 + i * 0x600;
            uint64_t t;
            memcpy(&t, r, 8);
            if (t > stamp) {
                newest = r;
                stamp = t;
            }
        }
        if (newest) {
            return std::vector<uint8_t>(newest, newest + kGpsExternalRecord);
        }
    }
    return std::vector<uint8_t>();
}
inline aa::CarLocation decodeGpsRecord(const std::vector<uint8_t>& r, uint64_t now) {
    if (r.size() == kGpsExternalRecord) {
        if ((gpsU32(&r[28]) & 3) != 3) {
            return aa::CarLocation();
        }
        double lat, lon;
        memcpy(&lat, &r[8], 8);
        memcpy(&lon, &r[16], 8);
        aa::CarLocation p = gpsDegrees(lat, lon, now);
        if (!p.valid) {
            return p;
        }
        float altitude, speed, heading;
        memcpy(&altitude, &r[24], 4);
        memcpy(&speed, &r[32 + 8], 4);
        memcpy(&heading, &r[32 + 16], 4);
        const uint32_t courseFlags =
            gpsU32(&r[32 + 20]);  // CarPlay cbCourse/RMC: bit 0 speed (m/s), bit 2 heading (degrees)
        if ((courseFlags & 1) && isfinite(speed) && gpsSpeedSendable(speed)) {
            p.hasSpeed = true;
            p.speedMps = speed;
            if ((courseFlags & 4) && isfinite(heading) && heading >= 0 && heading < 360 && speed >= 0.5f) {
                p.hasBearing = true;
                p.bearingDeg = heading;
            }
        }
        if (isfinite(altitude) && altitude >= -500 && altitude <= 9000) {
            p.hasAltitude = true;
            p.altitudeM = altitude;
        }
        return p;
    }
    if (r.size() != 0x3a || (r[0] != 1 && r[0] != 2) || r[4] > 1 || r[5] > 1 || gpsU32(&r[8]) >= 6000000 ||
        gpsU32(&r[16]) >= 6000000) {
        return aa::CarLocation();
    }
    double lat = r[6] + gpsU32(&r[8]) / 6000000.0;
    double lon = r[12] + gpsU32(&r[16]) / 6000000.0;
    aa::CarLocation p = gpsDegrees(r[4] ? -lat : lat, r[5] ? -lon : lon, now);
    if (!p.valid) {
        return p;
    }
    // Same fields as libgnsssv case 0x11 -> CarPlay TGNSSCourse: speed/1000 in m/s (CarPlay's RMC: * 1.948 = knots),
    // course/100 in degrees (u16 at +44), altitude/10 in metres. Course means nothing when nearly stopped.
    const double speed = gpsU32(&r[28]) / 1000.0;
    uint16_t course;
    memcpy(&course, &r[44], 2);
    int32_t altitude;
    memcpy(&altitude, &r[20], 4);
    if (gpsSpeedSendable(speed)) {
        p.hasSpeed = true;
        p.speedMps = speed;
        if (speed >= 0.5 && course < 36000) {
            p.hasBearing = true;
            p.bearingDeg = course / 100.0;
        }
    }
    if (altitude >= -5000 && altitude <= 90000) {
        p.hasAltitude = true;
        p.altitudeM = altitude / 10.0;
    }
    return p;
}
// Fallback when the receiver gives no valid speed: two consecutive fixes. Coarse: the poll time is the only clock here,
// so a fix seen late makes a jump; one that would be too fast to send gives neither speed nor course.
inline void gpsDerive(const aa::CarLocation& prev, aa::CarLocation& cur) {
    if (cur.hasSpeed || !prev.valid || cur.sampledMs < prev.sampledMs + 400) {
        return;
    }
    const double dt = (cur.sampledMs - prev.sampledMs) / 1000.0;
    if (dt > 5) {
        return;
    }
    const double metresPerE6 = 0.111195;
    const double dy = (cur.latitudeE6 - prev.latitudeE6) * metresPerE6;
    const double dx =
        (cur.longitudeE6 - prev.longitudeE6) * metresPerE6 * cos(cur.latitudeE6 / 1000000.0 * M_PI / 180.0);
    const double d = sqrt(dx * dx + dy * dy);
    const double speed = d / dt;
    if (!gpsSpeedSendable(speed)) {
        return;
    }
    cur.hasSpeed = true;
    cur.speedMps = speed;
    if (d >= 2) {
        double b = atan2(dx, dy) * 180.0 / M_PI;
        if (b < 0) {
            b += 360;
        }
        cur.hasBearing = true;
        cur.bearingDeg = b;
    }
}
class CarGpsReader {
    std::vector<uint8_t> previous_;
    aa::CarLocation latest_;
    uint64_t lastPoll_;
    size_t layout_;

public:
    CarGpsReader() : lastPoll_(0), layout_(0) {}
    const char* layoutName() const {
        return layout_ == 0x4258 ? "internal" : layout_ == 0x6048 ? "external" : "unknown";
    }
    aa::CarLocation poll(uint64_t now) {
        if (phoneGpsChosen()) {
            previous_.clear();
            latest_ = aa::CarLocation();
            return latest_;
        }
        if (!lastPoll_ || now - lastPoll_ >= 100) {  // the main loop's pace: a 1 Hz fix is seen within ~100 ms
            lastPoll_ = now;
            int fd = open(carGpsSource(), O_RDONLY | O_CLOEXEC);
            struct stat s;
            bool ok = fd >= 0 && fstat(fd, &s) == 0 && gpsLayout(s.st_size);
            std::vector<uint8_t> a, b;
            if (ok) {
                a.resize(s.st_size);
                b.resize(s.st_size);
                ok = pread(fd, &a[0], a.size(), 0) == static_cast<ssize_t>(a.size()) &&
                     pread(fd, &b[0], b.size(), 0) == static_cast<ssize_t>(b.size());
            }
            if (fd >= 0) {
                close(fd);
            }
            if (ok) {
                const std::vector<uint8_t> ra = gpsRecord(&a[0], a.size()), rb = gpsRecord(&b[0], b.size());
                // Consistent double read; no fresh timestamp for cached data or the first observation.
                if (gpsU32(&a[0]) == gpsU32(&b[0]) && ra == rb && !rb.empty()) {
                    if (layout_ == b.size() && !previous_.empty() && rb != previous_) {
                        const aa::CarLocation before = latest_;
                        latest_ = decodeGpsRecord(rb, now);
                        if (latest_.valid) {
                            gpsDerive(before, latest_);
                            // Estimate, not a measurement: the receiver gives no metres, and CarPlay sends none.
                            latest_.hasAccuracy = true;
                            latest_.accuracyM = 10;
                            const time_t t = time(nullptr);
                            latest_.utcSeconds = t > 1600000000 ? static_cast<uint64_t>(t) : 0;
                        }
                    }
                    if (layout_ != b.size()) {
                        latest_ = aa::CarLocation();
                    }
                    previous_ = rb;
                    layout_ = b.size();
                }
            } else {
                previous_.clear();
                latest_ = aa::CarLocation();
                layout_ = 0;
            }
        }
        aa::CarLocation p = latest_;
        if (!p.fresh(now)) {
            p.valid = false;
        }
        return p;
    }
};
// The car's fix for the phone, one call per main-loop turn: a log line when the fix comes or goes, and every 30 s while
// it is valid.
class CarGpsFeed {
public:
    CarGpsFeed() : valid_(false), lastLogMs_(0) {}
    // Once, at start: whether the car's GNSS ring is there.
    void logSource() const {
        ELOG("GPS", "car GNSS source (BMW navigation, CarPlay): %s, read only",
             carGpsPresent() ? "present" : "missing or turned off");
    }
    aa::CarLocation next(CarGpsReader& reader) {
        const aa::CarLocation gps = reader.poll(aa::locationClockMs());
        if (gps.valid != valid_) {
            ELOG("GPS", "BMW fix %s", gps.valid ? "valid and fresh" : "unavailable or stale");
            valid_ = gps.valid;
        }
        if (gps.valid && (!lastLogMs_ || monoMs() - lastLogMs_ >= 30000)) {
            lastLogMs_ = monoMs();
            ELOG("GPS", "layout %s: speed sent %.1f m/s (x3.6 = %.0f km/h), heading %.0f, altitude %.0f m (%s)",
                 reader.layoutName(), gps.hasSpeed ? gps.speedMps : -1.0, gps.hasSpeed ? gps.speedMps * 3.6 : -1.0,
                 gps.hasBearing ? gps.bearingDeg : -1.0, gps.hasAltitude ? gps.altitudeM : -1.0,
                 gps.hasSpeed ? "speed sent" : "no speed");
        }
        return gps;
    }

private:
    bool valid_;
    uint32_t lastLogMs_;
};
}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_EVO_GPS_HPP
