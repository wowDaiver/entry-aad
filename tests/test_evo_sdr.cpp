// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Sensor filter on the real DHU template, as entry-aad registers it with the core.

// Linked with platform/evo_sdr_sensors.cpp: channel 9 is still dropped by the core, the sensor channel is
// reduced to NIGHT_MODE (10) + DRIVING_STATUS (13) (plus LOCATION with the car's GNSS ring), and every other channel
// stays byte-identical. Also checks the vehicle file read by SdrIdentity, and the side and fuel told to the phone.
#include <stdio.h>
#include <stdlib.h>

#include <map>
#include <vector>

#include "car_brand.hpp"
#include "car_series.hpp"
#include "dhu_sdp.h"
#include "driver_side.hpp"
#include "evo_gps.hpp"
#include "evo_sdr_sensors.hpp"
#include "pb.hpp"
#include "sdr_builder.hpp"

#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            return 1;                                            \
        }                                                        \
    } while (0)

static bool varint(const std::vector<uint8_t>& b, size_t& i, uint64_t& v) {
    v = 0;
    for (int s = 0; i < b.size() && s < 64; s += 7) {
        uint8_t c = b[i++];
        v |= uint64_t(c & 0x7f) << s;
        if (!(c & 0x80)) {
            return true;
        }
    }
    return false;
}
struct F {
    int num, wt;
    uint64_t v;
    std::vector<uint8_t> body, raw;
};
static std::vector<F> fields(const std::vector<uint8_t>& b) {
    std::vector<F> out;
    size_t i = 0;
    while (i < b.size()) {
        size_t st = i;
        uint64_t k;
        if (!varint(b, i, k)) {
            break;
        }
        F f;
        f.num = int(k >> 3);
        f.wt = int(k & 7);
        f.v = 0;
        if (f.wt == 0) {
            varint(b, i, f.v);
        } else if (f.wt == 2) {
            uint64_t l;
            varint(b, i, l);
            f.body.assign(b.begin() + i, b.begin() + i + l);
            i += l;
        } else if (f.wt == 5) {
            i += 4;
        } else if (f.wt == 1) {
            i += 8;
        } else {
            break;
        }
        f.raw.assign(b.begin() + st, b.begin() + i);
        out.push_back(f);
    }
    return out;
}
// channel id -> raw bytes of its Service field; sensors of channel 1.
static std::map<uint64_t, std::vector<uint8_t>> channels(const std::vector<uint8_t>& sdr,
                                                         std::vector<uint64_t>& sensors) {
    std::map<uint64_t, std::vector<uint8_t>> m;
    sensors.clear();
    std::vector<F> top = fields(sdr);
    for (size_t i = 0; i < top.size(); ++i) {
        if (top[i].num != 1) {
            continue;
        }
        std::vector<F> svc = fields(top[i].body);
        uint64_t id = 999;
        for (size_t j = 0; j < svc.size(); ++j) {
            if (svc[j].num == 1 && svc[j].wt == 0) {
                id = svc[j].v;
            }
            if (svc[j].num == 2) {
                std::vector<F> ss = fields(svc[j].body);
                for (size_t k = 0; k < ss.size(); ++k) {
                    if (ss[k].num == 1) {
                        std::vector<F> s = fields(ss[k].body);
                        for (size_t q = 0; q < s.size(); ++q) {
                            if (s[q].num == 1) {
                                sensors.push_back(s[q].v);
                            }
                        }
                    }
                }
            }
        }
        m[id] = top[i].raw;
    }
    return m;
}

// The first field `num` of a message: its bytes, or its value.
static bool sub(const std::vector<uint8_t>& b, int num, std::vector<uint8_t>& out) {
    const std::vector<F> all = fields(b);
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].num == num && all[i].wt == 2) {
            out = all[i].body;
            return true;
        }
    }
    return false;
}
// Every varint field `num` of a message, in order.
static std::vector<uint64_t> values(const std::vector<uint8_t>& b, int num) {
    std::vector<uint64_t> out;
    const std::vector<F> all = fields(b);
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].num == num && all[i].wt == 0) {
            out.push_back(all[i].v);
        }
    }
    return out;
}
static uint64_t value(const std::vector<uint8_t>& b, int num) {
    const std::vector<F> all = fields(b);
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].num == num && all[i].wt == 0) {
            return all[i].v;
        }
    }
    return ~0ull;
}
// The service of one channel in a description.
static bool service(const std::vector<uint8_t>& sdr, uint64_t channel, std::vector<uint8_t>& out) {
    const std::vector<F> top = fields(sdr);
    for (size_t i = 0; i < top.size(); ++i) {
        if (top[i].num == 1 && top[i].wt == 2 && value(top[i].body, 1) == channel) {
            out = top[i].body;
            return true;
        }
    }
    return false;
}
// What a description tells the phone about the picture: canvas number, the two margins, the insets, the
// touchscreen's size.
struct Shown {
    uint64_t resolution, marginW, marginH, top, bottom, left, right, touchW, touchH, density;
};
static bool shown(const std::vector<uint8_t>& sdr, Shown& s) {
    std::vector<uint8_t> video, sink, config, ui, insets, input, source, touch;
    if (!service(sdr, 6, video) || !sub(video, 3, sink) || !sub(sink, 4, config) || !sub(config, 11, ui) ||
        !sub(ui, 1, insets) || !service(sdr, 7, input) || !sub(input, 4, source) || !sub(source, 2, touch)) {
        return false;
    }
    s.resolution = value(config, 1);
    s.marginW = value(config, 3);
    s.marginH = value(config, 4);
    s.density = value(config, 5);
    s.top = value(insets, 1);
    s.bottom = value(insets, 2);
    s.left = value(insets, 3);
    s.right = value(insets, 4);
    s.touchW = value(touch, 1);
    s.touchH = value(touch, 2);
    return true;
}

int main() {
    char gpsFile[96];
    snprintf(gpsFile, sizeof(gpsFile), "/tmp/entry-sdr-gps-test-%d", int(getpid()));
    evo::setCarGpsSourceForTest(gpsFile);
    unlink(gpsFile);
    std::vector<uint8_t> orig(kDhuSdpProtobuf, kDhuSdpProtobuf + kDhuSdpProtobufLen), sdr = orig;
    std::vector<uint64_t> s0, s1;
    std::map<uint64_t, std::vector<uint8_t>> c0 = channels(orig, s0);
    CHECK(s0.size() == 13 && c0.count(9) && c0.count(1));  // DHU: 13 sensor entries incl. LOCATION
    CHECK(aa::removeMediaChannel(sdr) == 1 && evo::trimDiscoverySensors(sdr) == 11);
    std::map<uint64_t, std::vector<uint8_t>> c1 = channels(sdr, s1);
    CHECK(!c1.count(9) && c1.size() == c0.size() - 1);
    CHECK(s1.size() == 2 && s1[0] == 10 && s1[1] == 13);  // NIGHT_MODE, DRIVING_STATUS only
    for (std::map<uint64_t, std::vector<uint8_t>>::iterator it = c1.begin(); it != c1.end(); ++it) {
        if (it->first != 1) {
            CHECK(it->second == c0[it->first]);  // other channels byte-identical
        }
    }
    CHECK(sdr.size() < orig.size());

    FILE* f = fopen(gpsFile, "wb");
    CHECK(f);
    CHECK(ftruncate(fileno(f), 0x4258) == 0);
    fclose(f);
    std::vector<uint8_t> withGps = orig;
    aa::removeMediaChannel(withGps);
    evo::trimDiscoverySensors(withGps);
    std::vector<uint64_t> carSensors;
    channels(withGps, carSensors);
    CHECK(carSensors.size() == 3 && carSensors[0] == 1);
    CHECK(evo::keptSensorTypes(true).size() == 3 && evo::keptSensorTypes(false).size() == 2);
    f = fopen(gpsFile, "wb");
    CHECK(f);
    fclose(f);  // unsupported layouts keep phone GPS
    CHECK(!evo::carGpsPresent());
    unlink(gpsFile);
    std::vector<uint8_t> junk(5, 0xff);  // unparsable input is left alone
    CHECK(evo::filterSdrSensors(junk, evo::keptSensorTypes(false)) == 0 && junk.size() == 5);
    // Vehicle file (written at installation for another car): model, year, side and fuel, invalid lines ignored.
    char vehicle[96];
    snprintf(vehicle, sizeof(vehicle), "/tmp/entry-sdr-vehicle-%d", int(getpid()));
    f = fopen(vehicle, "w");
    CHECK(f);
    fputs(
        "model=320d xDrive\r\nyear=2021\nside=right\r\nfuel=diesel\nmodel=bad;model\nyear=3021\nother=1\n"
        "side=centre\nside=Left\nfuel=electric\nfuel=petrol \n",
        f);
    fclose(f);
    aa::SdrIdentity id;
    CHECK(id.carModel == "iDrive" && id.carYear == "2019");
    CHECK(id.driverPosition == aa::DriverPosition::kLeft && id.fuel == aa::FuelType::kUnleaded);
    CHECK(id.loadVehicleFile(vehicle) && id.carModel == "320d xDrive" && id.carYear == "2021");
    CHECK(id.driverPosition == aa::DriverPosition::kRight && id.fuel == aa::FuelType::kDiesel);
    f = fopen(vehicle, "w");  // written again for a left-hand drive petrol car: the last valid line wins
    CHECK(f);
    fputs("side=left\nfuel=petrol\n", f);
    fclose(f);
    CHECK(id.loadVehicleFile(vehicle) && id.driverPosition == aa::DriverPosition::kLeft &&
          id.fuel == aa::FuelType::kUnleaded && id.carModel == "320d xDrive");
    unlink(vehicle);
    aa::SdrIdentity none;
    CHECK(!none.loadVehicleFile(vehicle) && none.carModel == "iDrive");
    CHECK(none.driverPosition == aa::DriverPosition::kLeft && none.fuel == aa::FuelType::kUnleaded);
    // What the phone is told: the template says left-hand drive (field 6 = 0), unleaded plus electric (fuel types 1
    // and 10) and a Tesla Supercharger connector (8). A car gets its own side and exactly one fuel, no connector.
    std::vector<uint8_t> sensorService, source;
    CHECK(value(orig, 6) == 0 && service(orig, 1, sensorService) && sub(sensorService, 2, source));
    CHECK(values(source, 3) == std::vector<uint64_t>({1, 10}) && values(source, 4) == std::vector<uint64_t>({8}));
    const std::vector<uint8_t> leftPetrol =
        aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr);
    CHECK(values(leftPetrol, 6) == std::vector<uint64_t>({0}));
    CHECK(service(leftPetrol, 1, sensorService) && sub(sensorService, 2, source));
    CHECK(values(source, 3) == std::vector<uint64_t>({1}) && values(source, 4).empty());
    CHECK(value(source, 2) == 256);  // the rest of the sensor source stays
    f = fopen(vehicle, "w");
    CHECK(f);
    fputs("side=right\nfuel=diesel\n", f);
    fclose(f);
    const std::vector<uint8_t> rightDiesel =
        aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, vehicle, nullptr);
    unlink(vehicle);
    CHECK(values(rightDiesel, 6) == std::vector<uint64_t>({1}));
    CHECK(service(rightDiesel, 1, sensorService) && sub(sensorService, 2, source));
    CHECK(values(source, 3) == std::vector<uint64_t>({4}) && values(source, 4).empty());
    // Only the side's value and the fuel's value differ: same length, two bytes apart.
    CHECK(rightDiesel.size() == leftPetrol.size());
    size_t differing = 0;
    for (size_t i = 0; i < leftPetrol.size(); ++i) {
        differing += leftPetrol[i] != rightDiesel[i] ? 1 : 0;
    }
    CHECK(differing == 2);
    // The side coded in the car (driver_side.hpp): told to the phone, unless the vehicle file says "side=".
    const std::vector<uint8_t> codedRight = aa::buildCarServiceDiscovery(
        kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, aa::DisplayPlan(), nullptr, aa::DriverPosition::kRight);
    CHECK(values(codedRight, 6) == std::vector<uint64_t>({1}) && codedRight.size() == leftPetrol.size());
    f = fopen(vehicle, "w");
    CHECK(f);
    fputs("model=118i\nside=left\n", f);
    fclose(f);
    const std::vector<uint8_t> fileLeft = aa::buildCarServiceDiscovery(
        kDhuSdpProtobuf, kDhuSdpProtobufLen, vehicle, nullptr, aa::DisplayPlan(), nullptr, aa::DriverPosition::kRight);
    f = fopen(vehicle, "w");
    CHECK(f);
    fputs("model=118i\n", f);
    fclose(f);
    const std::vector<uint8_t> fileWithoutSide = aa::buildCarServiceDiscovery(
        kDhuSdpProtobuf, kDhuSdpProtobufLen, vehicle, nullptr, aa::DisplayPlan(), nullptr, aa::DriverPosition::kRight);
    unlink(vehicle);
    CHECK(values(fileLeft, 6) == std::vector<uint64_t>({0}) &&
          values(fileWithoutSide, 6) == std::vector<uint64_t>({1}));
    bool right = false;
    CHECK(aa::parseDriverSide("right\n", right) && right && !strcmp(aa::driverSideName(right), "right"));
    CHECK(aa::parseDriverSide("left", right) && !right && !strcmp(aa::driverSideName(right), "left"));
    CHECK(!aa::parseDriverSide("rights", right) && !aa::parseDriverSide("Right", right) &&
          !aa::parseDriverSide("", right) && !aa::parseDriverSide(nullptr, right) && !right);
    // A description without the field still gets the side; one without a sensor channel is left as it was.
    std::vector<uint8_t> bare;
    aa::pb::appendString(bare, 2, "BMW");
    aa::SdrIdentity rightHand;
    rightHand.driverPosition = aa::DriverPosition::kRight;
    const std::vector<uint8_t> sided = aa::buildServiceDiscovery(bare.data(), bare.size(), rightHand);
    CHECK(values(sided, 6) == std::vector<uint64_t>({1}));
    std::vector<uint8_t> noSensors = sided;
    CHECK(!aa::declareFuel(noSensors, aa::FuelType::kDiesel) && noSensors == sided);
    CHECK(aa::validCarModel("X3 M40i") && !aa::validCarModel("") && !aa::validCarModel(std::string(25, 'a')) &&
          !aa::validCarModel("a'b") && aa::validCarYear("1999") && !aa::validCarYear("999") &&
          !aa::validCarYear("3000"));
    // The software version announced: the program's own when it gives one, "0.0.0" otherwise; nothing else moves.
    const std::vector<uint8_t> unversioned =
        aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr);
    const std::vector<uint8_t> versioned =
        aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, "12.34.56-rc.7");
    const std::string plain(unversioned.begin(), unversioned.end()), named(versioned.begin(), versioned.end());
    CHECK(plain.find("0.0.0") != std::string::npos && plain.find("12.34.56-rc.7") == std::string::npos);
    CHECK(named.find("12.34.56-rc.7") != std::string::npos && named.find("0.0.0") == std::string::npos);
    // Twice: at the top level and in the head unit info, 8 characters longer each time.
    CHECK(named.find("12.34.56-rc.7") != named.rfind("12.34.56-rc.7") && versioned.size() >= unversioned.size() + 16);
    // The picture asked of the phone follows the panel (core/display_plan.hpp).
    aa::DisplayPlan plan;
    CHECK(plan.reference() && plan.canvasWidth == 1280 && plan.canvasHeight == 720 && plan.resolution == 2);
    aa::DisplayPlan same;
    CHECK(aa::planDisplay(1280, 480, same) && same.reference() && same.canvasHeight == 720 && same.resolution == 2 &&
          same.bottomInset() == 240 && same.rightInset() == 0);
    aa::DisplayPlan small, exact, wide, tall, full;
    CHECK(aa::planDisplay(800, 480, small) && small.resolution == 1 && small.canvasWidth == 800 &&
          small.bottomInset() == 0 && small.rightInset() == 0 && !small.reference());
    CHECK(aa::planDisplay(1280, 720, exact) && exact.resolution == 2 && exact.bottomInset() == 0);
    CHECK(aa::planDisplay(1440, 540, wide) && wide.resolution == 3 && wide.canvasWidth == 1920 &&
          wide.bottomInset() == 540 && wide.rightInset() == 480);
    CHECK(aa::planDisplay(800, 600, tall) && tall.resolution == 2 && tall.rightInset() == 480 &&
          tall.bottomInset() == 120);  // one pixel too tall for the small canvas
    CHECK(aa::planDisplay(1920, 1080, full) && full.resolution == 3 && full.bottomInset() == 0);
    // Sizes already run in a car pass the check without a warning; any other size gets one.
    CHECK(plan.triedInCar() && same.triedInCar() && small.triedInCar());
    CHECK(!exact.triedInCar() && !wide.triedInCar() && !tall.triedInCar() && !full.triedInCar());
    aa::DisplayPlan refused;
    CHECK(!aa::planDisplay(1921, 720, refused) && !aa::planDisplay(1280, 1081, refused) &&
          !aa::planDisplay(0, 480, refused) && !aa::planDisplay(800, 0, refused) &&
          refused.reference());  // a refused panel leaves the plan as it was
    // The reference car: the very bytes sent before the panel was read, whether the plan is given or not.
    CHECK(aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, same) == unversioned);
    Shown s;
    CHECK(shown(unversioned, s) && s.resolution == 2 && s.marginW == 1280 && s.marginH == 480 && s.density == 140 &&
          s.top == 0 && s.bottom == 240 && s.left == 0 && s.right == 0 && s.touchW == 1280 && s.touchH == 720);
    // An 800x480 panel: the small canvas shown whole, no margin, no inset, a touchscreen of its size.
    const std::vector<uint8_t> forSmall =
        aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, small);
    CHECK(shown(forSmall, s) && s.resolution == 1 && s.marginW == 0 && s.marginH == 0 && s.density == 140 &&
          s.top == 0 && s.bottom == 0 && s.left == 0 && s.right == 0 && s.touchW == 800 && s.touchH == 480);
    // A 1440x540 panel: the large canvas, its UI kept in the top-left 1440x540.
    const std::vector<uint8_t> forWide =
        aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, wide);
    CHECK(shown(forWide, s) && s.resolution == 3 && s.marginW == 0 && s.marginH == 0 && s.bottom == 540 &&
          s.right == 480 && s.top == 0 && s.left == 0 && s.touchW == 1920 && s.touchH == 1080);
    // The brand file the iDrive app writes: "MINI" or "BMW", nothing else.
    aa::CarBrand brand = aa::CarBrand::kBmw;
    CHECK(aa::parseCarBrand("MINI\n", brand) && brand == aa::CarBrand::kMini && !strcmp(aa::brandName(brand), "MINI"));
    CHECK(aa::parseCarBrand("BMW", brand) && brand == aa::CarBrand::kBmw && !strcmp(aa::brandName(brand), "BMW"));
    CHECK(!aa::parseCarBrand("MINIS", brand) && !aa::parseCarBrand("mini", brand) && !aa::parseCarBrand("", brand) &&
          !aa::parseCarBrand(nullptr, brand) && brand == aa::CarBrand::kBmw);
    CHECK(aa::surroundSide(aa::CarBrand::kBmw, 1280) == 0 && aa::surroundSide(aa::CarBrand::kMini, 1280) == 40 &&
          aa::surroundSide(aa::CarBrand::kMini, 800) == 25);
    // A MINI: its round surround hides 40 columns on each side of the 1280x480 panel (25 of an 800x480 one). The
    // phone keeps its UI away from them; the canvas, the touchscreen and the bottom inset stay.
    aa::DisplayPlan mini = same, smallMini = small;
    mini.hiddenSide = aa::surroundSide(aa::CarBrand::kMini, mini.width);
    smallMini.hiddenSide = aa::surroundSide(aa::CarBrand::kMini, smallMini.width);
    const std::vector<uint8_t> forMini =
        aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, mini, "MINI");
    CHECK(shown(forMini, s) && s.resolution == 2 && s.bottom == 240 && s.left == 40 && s.right == 40 &&
          s.touchW == 1280 && s.touchH == 720);
    CHECK(shown(aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, smallMini), s) &&
          s.left == 25 && s.right == 25 && s.bottom == 0 && s.touchW == 800);
    // The phone's own display, in whose pixels touches are sent: the canvas less the margins.
    CHECK(same.phoneWidth() == 1280 && same.phoneHeight() == 480 && same.leftInset() == 0);
    CHECK(mini.phoneWidth() == 1200 && mini.phoneHeight() == 480 && mini.leftInset() == 40);
    CHECK(smallMini.phoneWidth() == 750 && smallMini.phoneHeight() == 480 && small.phoneWidth() == 800);
    CHECK(wide.phoneWidth() == 1440 && wide.phoneHeight() == 540);
    // The maker: "MINI" for the car and the head unit, at the top level and in the head unit info; "BMW" otherwise.
    const std::string miniText(forMini.begin(), forMini.end());
    CHECK(plain.find("MINI") == std::string::npos && miniText.find("BMW") == std::string::npos);
    size_t makes = 0;
    for (size_t at = miniText.find("MINI"); at != std::string::npos; at = miniText.find("MINI", at + 1)) {
        ++makes;
    }
    CHECK(makes == 4);  // car make and head unit make, twice each
    CHECK(aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, same, "") == unversioned);
    // Nothing but the picture and the touchscreen differs: every other channel is the reference car's.
    std::vector<uint64_t> sensorsA, sensorsB;
    std::map<uint64_t, std::vector<uint8_t>> reference = channels(unversioned, sensorsA),
                                             other = channels(forSmall, sensorsB);
    CHECK(reference.size() == other.size() && sensorsA == sensorsB);
    for (std::map<uint64_t, std::vector<uint8_t>>::iterator it = reference.begin(); it != reference.end(); ++it) {
        CHECK(other.count(it->first) && (it->first == 6 || it->first == 7 || other[it->first] == it->second));
    }
    // The series coded in the car (car_series.hpp): the model told to the phone, unless the vehicle file says
    // "model=". The table: distinct ids and names, every name valid in the series file and as a model.
    char series[6] = {0};
    for (size_t i = 0; i < aa::kSeriesVariantCount; ++i) {
        CHECK(aa::kSeriesVariants[i].id != aa::kVariantSeriesUnknown);
        CHECK(aa::parseCarSeries(aa::kSeriesVariants[i].series, series) &&
              !strcmp(series, aa::kSeriesVariants[i].series) && aa::validCarModel(series));
        for (size_t k = i + 1; k < aa::kSeriesVariantCount; ++k) {
            CHECK(aa::kSeriesVariants[i].id != aa::kSeriesVariants[k].id &&
                  strcmp(aa::kSeriesVariants[i].series, aa::kSeriesVariants[k].series) != 0);
        }
    }
    const char* g20 = nullptr;
    const char* f48 = nullptr;
    const char* f56 = nullptr;
    for (size_t i = 0; i < aa::kSeriesVariantCount; ++i) {
        g20 = aa::kSeriesVariants[i].id == 0xDC3 ? aa::kSeriesVariants[i].series : g20;
        f48 = aa::kSeriesVariants[i].id == 0x55A ? aa::kSeriesVariants[i].series : f48;
        f56 = aa::kSeriesVariants[i].id == 0x55E ? aa::kSeriesVariants[i].series : f56;
    }
    CHECK(aa::kSeriesVariantCount == 90 && g20 && !strcmp(g20, "G20") && f48 && !strcmp(f48, "F48") && f56 &&
          !strcmp(f56, "F56"));
    CHECK(aa::parseCarSeries("G20\n", series) && !strcmp(series, "G20") && aa::parseCarSeries("RR31\r\n", series));
    CHECK(!aa::parseCarSeries("g20", series) && !aa::parseCarSeries("G", series) &&
          !aa::parseCarSeries("G20 xDrive", series) && !aa::parseCarSeries("", series) &&
          !aa::parseCarSeries(nullptr, series) && !strcmp(series, "RR31"));
    const std::vector<uint8_t> codedG20 = aa::buildCarServiceDiscovery(
        kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, same, nullptr, aa::DriverPosition::kLeft, "G20");
    const std::string g20Text(codedG20.begin(), codedG20.end());
    CHECK(g20Text.find("G20") != std::string::npos && g20Text.find("iDrive") == std::string::npos);
    const std::string unversionedText(unversioned.begin(), unversioned.end());
    size_t idrives = 0, g20s = 0;
    for (size_t at = unversionedText.find("iDrive"); at != std::string::npos;
         at = unversionedText.find("iDrive", at + 1)) {
        ++idrives;
    }
    for (size_t at = g20Text.find("G20"); at != std::string::npos; at = g20Text.find("G20", at + 1)) {
        ++g20s;
    }
    CHECK(idrives > 0 && g20s == idrives && codedG20.size() == unversioned.size() - 3 * idrives);
    f = fopen(vehicle, "w");
    CHECK(f);
    fputs("model=320d\n", f);
    fclose(f);
    const std::vector<uint8_t> fileModel = aa::buildCarServiceDiscovery(
        kDhuSdpProtobuf, kDhuSdpProtobufLen, vehicle, nullptr, same, nullptr, aa::DriverPosition::kLeft, "G20");
    unlink(vehicle);
    const std::string fileText(fileModel.begin(), fileModel.end());
    CHECK(fileText.find("320d") != std::string::npos && fileText.find("G20") == std::string::npos);
    CHECK(aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, same, nullptr,
                                       aa::DriverPosition::kLeft, "G2;0") == unversioned);
    CHECK(aa::buildCarServiceDiscovery(kDhuSdpProtobuf, kDhuSdpProtobufLen, nullptr, nullptr, same, nullptr,
                                       aa::DriverPosition::kLeft, nullptr) == unversioned);
    puts(
        "PASS capteurs AA : modele DHU reduit a NIGHT_MODE + DRIVING_STATUS (le telephone garde son GPS), canal 9 "
        "toujours retire par le coeur, autres canaux identiques, fichier voiture (modele, annee, cote, carburant), "
        "un seul carburant sans prise de recharge, version annoncee");
    return 0;
}
