// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Tests platform/kombi_most.cpp: the F-series cluster's guidance fields over MOST, with a fake vmostd connection.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kombi_display.hpp"
#include "kombi_most.hpp"
#include "kombi_test_util.hpp"

namespace {

using namespace evo;
using namespace evo::kombi;
using kombitest::Bytes;
using kombitest::Step;

// Twice the signed area: positive for the winding the stock arrows use (clockwise on the screen).
long winding(const std::vector<HudPoint>& p) {
    long area = 0;
    for (size_t k = 0; k < p.size(); ++k) {
        area +=
            static_cast<long>(p[k].x) * p[(k + 1) % p.size()].y - static_cast<long>(p[(k + 1) % p.size()].x) * p[k].y;
    }
    return area;
}

void extent(const std::vector<HudPoint>& p, int& minX, int& maxX, int& minY, int& maxY) {
    minX = minY = 1 << 20;
    maxX = maxY = -1;
    for (size_t k = 0; k < p.size(); ++k) {
        minX = p[k].x < minX ? p[k].x : minX;
        maxX = p[k].x > maxX ? p[k].x : maxX;
        minY = p[k].y < minY ? p[k].y : minY;
        maxY = p[k].y > maxY ? p[k].y : maxY;
    }
}

Maneuver maneuverOf(int type, const std::string& road, int64_t metres, const std::string& text, int unit) {
    const NavInbox in = kombitest::inbox(kombitest::state(Step(type, road)), kombitest::position(metres, text, unit));
    Maneuver m;
    CHECK(in.nextManeuver(m));
    return m;
}

// ---- the protocol's pieces ----

void registryAndSizes() {
    const uint8_t reg[] = {0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x82, 0x00, 0x01, 0x01, 0x52, 0x00, 0xFF};
    uint16_t a = 0;
    CHECK(registryAddress(reg, sizeof(reg), 0x82, 0x00, &a) && a == 0x0101);
    CHECK(registryAddress(reg, sizeof(reg), 0x02, 0x00, &a) && a == 0x0100);
    CHECK(!registryAddress(reg, sizeof(reg), 0x82, 0x02, &a));
    CHECK(!registryAddress(reg, 3, 0x02, 0x00, &a));  // no whole record

    const uint8_t size[] = {0x00, 0x05, 0x01, 0x00, 0xB0, 0x00, 0x7E};
    uint16_t field = 0;
    FieldSize s;
    CHECK(fieldSize(size, sizeof(size), &field, &s) && field == 5 && s.w == 176 && s.h == 126);
    CHECK(!fieldSize(size, 5, &field, &s));

    FieldSize sizes[kHudFields] = {{396, 68}, {272, 33}, {80, 28}, {44, 28}, {176, 126}, {176, 126}};
    std::string why;
    CHECK(layoutFits(sizes, &why));
    sizes[5].w = 0;
    sizes[4].w = 0;
    CHECK(!layoutFits(sizes, &why) && why.find("never set up") != std::string::npos);
    sizes[4].w = 176;
    sizes[5].w = 150;
    CHECK(!layoutFits(sizes, &why) && why.find("differ") != std::string::npos);
}

void calls() {
    const HudCall show = hudShow(1, true);
    CHECK(show.fkt == 0x204 && show.data == Bytes({0x00, 0x01, 0x01}));
    CHECK(hudSwap(6, 5).data == Bytes({0x00, 0x06, 0x00, 0x05}) && hudSwap(6, 5).fkt == 0x20E);
    CHECK(hudEmpty(5).fkt == 0x220 && hudEmpty(5).data == Bytes({0x00, 0x05}));
    CHECK(hudSize(3).fkt == 0xC04 && hudSize(3).data == Bytes({0x00, 0x03, 0x01}));
    // UTF-16BE after the field and a zero byte, then a zero character.
    CHECK(hudText(2, "Rue A").data ==
          Bytes({0x00, 0x02, 0x00, 0x00, 'R', 0x00, 'u', 0x00, 'e', 0x00, ' ', 0x00, 'A', 0x00, 0x00}));
    CHECK(hudText(2, "\xC3\xA9\xE2\x82\xAC").data == Bytes({0x00, 0x02, 0x00, 0x00, 0xE9, 0x20, 0xAC, 0x00, 0x00}));
    CHECK(hudText(2, "\xF0\x9F\x9A\x97").data == Bytes({0x00, 0x02, 0x00, 0x00, '?', 0x00, 0x00}));  // past U+FFFF
    CHECK(hudText(2, "a\xFF").data == Bytes({0x00, 0x02, 0x00, 0x00, 'a', 0x00, '?', 0x00, 0x00}));
    CHECK(hudText(4, "").data == Bytes({0x00, 0x04, 0x00, 0x00, 0x00}));
}

void pictures() {
    const int directions[] = {kStraightOn,
                              kHalfLeft,
                              kHalfRight,
                              kTightLeft,
                              kTightRight,
                              kLeft,
                              kRight,
                              kUTurnLeft,
                              kUTurnRight,
                              kDestination,
                              kRoundaboutClockwise,
                              kRoundaboutCounterClockwise,
                              kHighwayInLeft,
                              kHighwayOutRight,
                              kLaneChangeLeft,
                              kForkRight};
    const FieldSize fields[] = {{176, 126}, {120, 90}};
    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
        const uint16_t w = fields[f].w, h = fields[f].h;
        for (size_t i = 0; i < sizeof(directions) / sizeof(directions[0]); ++i) {
            const std::vector<HudShape> shapes = hudPicture(directions[i], w, h);
            CHECK(!shapes.empty() && shapes.size() <= 16);
            for (size_t s = 0; s < shapes.size(); ++s) {
                CHECK(convex(shapes[s].points));  // as the stock arrows: convex pieces only
                CHECK(winding(shapes[s].points) > 0);
                CHECK(shapes[s].points.size() >= 3 && shapes[s].points.size() <= 8);
                for (size_t k = 0; k < shapes[s].points.size(); ++k) {
                    const HudPoint& p = shapes[s].points[k];
                    // Inside the field and clear of the shade on its right.
                    CHECK(p.x >= w * 0.24 - 1 && p.x <= w * 0.92 + 1 && p.y >= 0 && p.y < h);
                }
            }
        }
    }
    CHECK(hudPicture(0, 176, 126).empty() && hudPicture(11, 176, 126).empty());
    // Left is the mirror of right about the middle of the clear part.
    const std::vector<HudShape> right = hudPicture(kRight, 176, 126), left = hudPicture(kLeft, 176, 126);
    CHECK(right.size() == left.size());
    const double middle = (176 * 0.24 + 176 * 0.92) / 2;
    for (size_t s = 0; s < right.size(); ++s) {
        int r0, r1, ry0, ry1, l0, l1, ly0, ly1;
        extent(right[s].points, r0, r1, ry0, ry1);
        extent(left[s].points, l0, l1, ly0, ly1);
        CHECK(r0 + l1 >= 2 * middle - 2 && r0 + l1 <= 2 * middle + 2 && r1 + l0 >= 2 * middle - 2 &&
              r1 + l0 <= 2 * middle + 2 && ry0 == ly0 && ry1 == ly1);
    }
    // The roundabout's ring is grey, its arrow white.
    const std::vector<HudShape> ring = hudPicture(kRoundaboutCounterClockwise, 176, 126);
    CHECK(ring.front().colour == kHudGrey && ring.back().colour == kHudWhite);
    std::vector<HudPoint> concave;
    const HudPoint pts[] = {{0, 0}, {10, 0}, {5, 2}, {10, 10}, {0, 10}};
    concave.assign(pts, pts + 5);
    CHECK(!convex(concave));

    HudCalls out;
    pictureCalls(ring, 5, out);
    CHECK(out[0] == hudEmpty(5) && out[1].fkt == 0x219 && out[1].data == Bytes({0x00, 0x05, 0x00}));
    CHECK(out[2].fkt == 0x21B && out[2].data == Bytes({0x00, 0x05, kHudGrey}));
    CHECK(out[3].fkt == 0x21D && out[3].data[2] == 0 && out[3].data[3] == 0 && out[3].data[4] == 3);
    CHECK(out[3].data[5] == ring[0].points.size() && out[3].data.size() == 6 + 4 * ring[0].points.size());
    size_t colours = 0, polygons = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        colours += out[i].fkt == 0x21B;
        polygons += out[i].fkt == 0x21D;
    }
    CHECK(colours == 2 && polygons == ring.size());
    CHECK(out.back().data[2] == 0 && out.back().data[3] == ring.size() - 1);  // indices follow the shapes
}

void views() {
    const HudLayout l;
    const FieldSize picture = {176, 126};
    HudView v;
    v.direction = kRight;
    v.distance = "300";
    v.unit = "m";
    v.street = "Rue A";
    uint16_t visible = l.pictureB;
    HudCalls out;
    viewCalls(v, nullptr, l, picture, visible, out);
    // Picture into the hidden buffer A, swap, empty B, the text fields shown and filled, then the frame.
    CHECK(visible == l.pictureA && out[0] == hudEmpty(l.pictureA));
    size_t i = 0;
    while (out[i].fkt != 0x20E) {
        CHECK(out[i].data[0] == 0 && out[i].data[1] == l.pictureA);
        ++i;
    }
    CHECK(out[i] == hudSwap(l.pictureB, l.pictureA) && out[i + 1] == hudEmpty(l.pictureB));
    CHECK(out[i + 2] == hudShow(l.street, true) && out[i + 3] == hudShow(l.distance, true) &&
          out[i + 4] == hudShow(l.unit, true));
    CHECK(out[i + 5] == hudText(l.street, "Rue A") && out[i + 6] == hudText(l.distance, "300") &&
          out[i + 7] == hudText(l.unit, "m") && out[i + 8] == hudShow(l.frame, true) && out.size() == i + 9);

    // Same maneuver, nearer: the distance only.
    HudView nearer = v;
    nearer.distance = "250";
    out.clear();
    viewCalls(nearer, &v, l, picture, visible, out);
    CHECK(out.size() == 1 && out[0] == hudText(l.distance, "250") && visible == l.pictureA);
    // Next maneuver: into B this time, the street and the distance.
    HudView next = nearer;
    next.direction = kLeft;
    next.street = "Rue B";
    out.clear();
    viewCalls(next, &nearer, l, picture, visible, out);
    CHECK(visible == l.pictureB && out[0] == hudEmpty(l.pictureB));
    bool swapped = false;
    for (size_t k = 0; k < out.size(); ++k) {
        swapped |= out[k] == hudSwap(l.pictureA, l.pictureB);
    }
    CHECK(swapped && out.back() == hudText(l.street, "Rue B"));

    out.clear();
    clearCalls(l, out);
    CHECK(out.size() == 6 && out[0] == hudShow(l.frame, false) && out[1] == hudEmpty(l.pictureA) &&
          out[2] == hudEmpty(l.pictureB) && out[3] == hudText(l.street, "") && out[5] == hudText(l.unit, ""));
}

// ---- what is shown ----

void presenter() {
    GuideSettings s;  // approach 1 mi, hysteresis 1.15, grace 8 s, first distance 3 s, distance every 0.5 s
    HudPresenter p(s);
    HudView v;
    const Maneuver far = maneuverOf(8, "Rue A", 3000, "3.0", kUnitKilometresTenths);
    CHECK(!p.step(&far, 0, v));  // beyond the approach
    const Maneuver near = maneuverOf(8, "Rue A", 800, "800", kUnitMetres);
    CHECK(p.step(&near, 1, v) && v.direction == kRight && v.distance == "800" && v.unit == "m" && v.street == "Rue A");
    // Hysteresis: the same turn stays up to 1.15 mi, then goes.
    const Maneuver back = maneuverOf(8, "Rue A", 1750, "1.8", kUnitKilometresTenths);
    CHECK(p.step(&back, 2, v) && v.distance == "1.8" && v.unit == "km");
    const Maneuver gone = maneuverOf(8, "Rue A", 1900, "1.9", kUnitKilometresTenths);
    CHECK(!p.step(&gone, 3, v));
    // The distance changes at most every 0.5 s on the same maneuver.
    CHECK(p.step(&near, 10, v) && v.distance == "800");
    const Maneuver closer = maneuverOf(8, "Rue A", 700, "700", kUnitMetres);
    CHECK(p.step(&closer, 10.2, v) && v.distance == "800");
    CHECK(p.step(&closer, 10.6, v) && v.distance == "700");
    // Guidance gone: the view stays 8 s.
    CHECK(p.step(nullptr, 11, v) && v.distance == "700");
    CHECK(p.step(nullptr, 18.9, v));
    CHECK(!p.step(nullptr, 19.1, v));
    // A first maneuver without a distance waits 3 s, then shows "-".
    p.reset();
    Maneuver none = near;
    none.distanceText.clear();
    none.hasMetres = false;
    CHECK(!p.step(&none, 30, v) && !p.step(&none, 32.9, v));
    CHECK(p.step(&none, 33.1, v) && v.distance == "-" && v.unit.empty());
    // Miles stay miles; a roundabout's exit goes before the street.
    p.reset();
    const Maneuver miles = maneuverOf(34, "Main St", 700, "0.4", kUnitMilesTenths);
    Maneuver exitTwo = miles;
    exitTwo.cue = "Take the 2nd exit";
    CHECK(p.step(&exitTwo, 40, v) && v.unit == "mi" && v.distance == "0.4" && v.street == "Exit 2 - Main St" &&
          v.direction == kRoundaboutCounterClockwise);
    // A long street is cut to 30 characters.
    p.reset();
    const Maneuver longName = maneuverOf(7, std::string(40, 'x'), 300, "300", kUnitMetres);
    CHECK(p.step(&longName, 50, v) && v.street.size() == 30);
}

void neighbours() {
    const std::string table =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "160.48.199.96    0x1         0x2         02:00:00:00:00:96     *        eth0\n"
        "160.48.199.97    0x1         0x0         00:00:00:00:00:00     *        eth0\n"
        "172.16.222.50    0x1         0x2         aa:bb:cc:dd:ee:ff     *        wlan0\n";
    CHECK(arpHasNeighbour(table, "160.48.199.96"));
    CHECK(!arpHasNeighbour(table, "160.48.199.97"));  // incomplete
    CHECK(!arpHasNeighbour(table, "160.48.199.9"));
    CHECK(!arpHasNeighbour(table.substr(0, table.find('\n') + 1), "160.48.199.96"));
}

// ---- the calls, with a fake connection ----

struct Message {
    VmostMsgOut out;
    Bytes data;
};

class FakePort : public VmostPort {
public:
    FakePort() : refuseSends(false), now(0) {}
    int send(VmostMsgOut& m) override {
        if (refuseSends) {
            return -1;
        }
        Message copy;
        copy.out = m;
        copy.data.assign(m.data, m.data + m.len);
        sent.push_back(copy);
        at.push_back(now);
        return 0;
    }
    int shadow(uint8_t fblock, uint8_t inst, bool on) override {
        shadows.push_back((on ? 0x10000 : 0) | (fblock << 8) | inst);
        return 0;
    }
    void pump(int) override {}
    std::vector<Message> sent;
    std::vector<uint32_t> at;  // when each was sent
    std::vector<int> shadows;
    bool refuseSends;
    uint32_t now;
};

VmostMsgView answer(uint8_t fblock, uint16_t fkt, uint32_t op, const Bytes& data, uint16_t addr = 0x0101) {
    VmostMsgView v;
    v.addr = addr;
    v.fblock = fblock;
    v.inst = 0;
    v.fkt = fkt;
    v.op = op;
    v.len = static_cast<uint16_t>(data.size());
    v.data = data.empty() ? nullptr : data.data();
    return v;
}

// An answer the car sends later.
struct Later {
    uint32_t at;
    uint16_t fkt;
    Bytes data;
};

// The cluster of the F52: registry with 82/00 at 0x0101, the 6WA fields; every call acknowledged unless refuseFkt.
struct Car {
    bool hud;
    bool silent;     // the registry does not answer
    bool hudSilent;  // the cluster does not answer
    uint16_t refuseFkt;
    uint16_t pictureW, pictureH;
    uint32_t sizeDelayMs;    // a cold cluster answers the size reads this late
    uint32_t answerDelayMs;  // and the drawing calls this late, each answer with its call's parameters
    bool drawSilent;         // the drawing calls are never answered
    std::vector<Later> later;
    Car()
        : hud(true),
          silent(false),
          hudSilent(false),
          refuseFkt(0),
          pictureW(176),
          pictureH(126),
          sizeDelayMs(0),
          answerDelayMs(0),
          drawSilent(false) {}
};

// Ticks the hud for `ms`, answering each new call as the car would.
void run(MostHud& hud, FakePort& port, Car& car, uint32_t& now, uint32_t ms) {
    size_t answered = port.sent.size();
    for (uint32_t end = now + ms; now < end; now += 5) {
        port.now = now;
        for (size_t i = 0; i < car.later.size();) {
            if (now >= car.later[i].at) {
                const Later l = car.later[i];
                car.later.erase(car.later.begin() + static_cast<long>(i));
                hud.onMessage(answer(kHudFblock, l.fkt, kMostOpResultAck, l.data));
            } else {
                ++i;
            }
        }
        hud.tick(now, port);
        while (answered < port.sent.size()) {
            const Message& m = port.sent[answered++];
            Bytes keep;
            if (m.out.fblock == kRegistryFblock ? car.silent : car.hudSilent) {
                continue;
            }
            if (m.out.fblock == kRegistryFblock) {
                const uint8_t entries[] = {0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x82, 0x00};
                keep.assign(entries, entries + (car.hud ? 8 : 4));
                hud.onMessage(answer(kRegistryFblock, kRegistryFkt, kMostOpStatus, keep, 0x0100));
            } else if (m.out.fkt == car.refuseFkt) {
                keep = Bytes({0x41, 0x02});
                hud.onMessage(answer(kHudFblock, m.out.fkt, kMostOpErrorAck, keep));
            } else if (m.out.fkt == kHudSize) {
                static const uint16_t w[] = {396, 272, 80, 44, 176, 176}, h[] = {68, 33, 28, 28, 126, 126};
                const unsigned field = m.data[1];
                const uint16_t width = field >= 5 ? car.pictureW : w[field - 1];
                const uint16_t height = field >= 5 ? car.pictureH : h[field - 1];
                keep = Bytes({0x00, static_cast<uint8_t>(field), 0x01, static_cast<uint8_t>(width >> 8),
                              static_cast<uint8_t>(width & 0xFF), static_cast<uint8_t>(height >> 8),
                              static_cast<uint8_t>(height & 0xFF)});
                if (car.sizeDelayMs) {
                    Later l = {now + car.sizeDelayMs, kHudSize, keep};
                    car.later.push_back(l);
                } else {
                    hud.onMessage(answer(kHudFblock, kHudSize, kMostOpResultAck, keep));
                }
            } else if (car.drawSilent) {
                continue;
            } else if (car.answerDelayMs) {
                Later l = {now + car.answerDelayMs, m.out.fkt, m.data};
                car.later.push_back(l);
            } else {
                hud.onMessage(answer(kHudFblock, m.out.fkt, kMostOpResultAck, Bytes()));
            }
        }
    }
}

size_t countFkt(const FakePort& port, uint16_t fkt, size_t from = 0) {
    size_t n = 0;
    for (size_t i = from; i < port.sent.size(); ++i) {
        n += port.sent[i].out.fblock == kHudFblock && port.sent[i].out.fkt == fkt;
    }
    return n;
}

struct Place {
    char dir[64];
    std::string marker, off;
    Place() {
        snprintf(dir, sizeof(dir), "/tmp/kombi-most-XXXXXX");
        CHECK(mkdtemp(dir));
        marker = std::string(dir) + "/kombi-most.shown";
        off = std::string(dir) + "/kombi-off";
    }
    ~Place() {
        unlink(marker.c_str());
        unlink(off.c_str());
        rmdir(dir);
    }
    MostHudConfig config(double ethernetFirstS = 0) const {
        MostHudConfig c;
        c.markerFile = marker;
        c.offFile = off;
        c.clusterAddress = "160.48.199.96";  // never a neighbour of the PC
        c.ethernetFirstS = ethernetFirstS;
        c.guide.graceS = 0;
        c.guide.firstDistanceS = 0;
        c.guide.distanceEveryS = 0;
        return c;
    }
    bool marked() const { return access(marker.c_str(), F_OK) == 0; }
    void touch(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "w");
        CHECK(f);
        fclose(f);
    }
};

// The navigation messages of one moment, as the session hands them over.
void tell(MostHud& hud, int type, const std::string& road, int64_t metres, const char* text, int unit) {
    const Bytes status = kombitest::status(1), state = kombitest::state(Step(type, road)),
                position = kombitest::position(metres, text, unit);
    MostHud::onNavStatus(kNavStatus, &status[0], status.size(), &hud);
    MostHud::onNavStatus(kNavState, &state[0], state.size(), &hud);
    MostHud::onNavStatus(kNavPosition, &position[0], position.size(), &hud);
}

void guidanceOff(MostHud& hud) {
    const Bytes status = kombitest::status(2);
    MostHud::onNavStatus(kNavStatus, &status[0], status.size(), &hud);
}

void drawsAndClears() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 1000;
    run(hud, port, car, now, 600);
    CHECK(port.sent.empty() && port.shadows.empty());  // nothing on the bus before a maneuver
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.phase() == MostHud::kReady && hud.showing() && place.marked());
    // The registry request, then six sizes, then the frame; every call to the cluster's address, StartResultAck.
    CHECK(port.sent[0].out.addr == kRegistryAddress && port.sent[0].out.fkt == kRegistryFkt &&
          port.sent[0].out.op == kMostOpGet);
    CHECK(countFkt(port, kHudSize) == 6 && countFkt(port, kHudSwap) == 1 && countFkt(port, kHudText) == 3);
    for (size_t i = 1; i < port.sent.size(); ++i) {
        CHECK(port.sent[i].out.addr == 0x0101 && port.sent[i].out.op == kMostOpStartResultAck &&
              port.sent[i].out.fblock == kHudFblock && port.sent[i].out.inst == 0);
    }
    CHECK(port.shadows.size() == 3);
    for (size_t i = 2; i < port.sent.size(); ++i) {
        CHECK(port.at[i] - port.at[i - 1] >= 20);  // 20 ms apart at least
    }
    // Nearer: one text.
    size_t before = port.sent.size();
    tell(hud, 8, "Rue A", 250, "250", kUnitMetres);
    run(hud, port, car, now, 500);
    CHECK(port.sent.size() == before + 1 && port.sent.back().data == hudText(3, "250").data);
    // Guidance ends: cleared, marker gone.
    guidanceOff(hud);
    run(hud, port, car, now, 600);
    CHECK(!hud.showing() && !place.marked() && countFkt(port, kHudShow, before) == 1);
    // The menu's setting: off clears, nothing more is drawn.
    tell(hud, 8, "Rue A", 250, "250", kUnitMetres);
    run(hud, port, car, now, 600);
    CHECK(hud.showing());
    place.touch(place.off);
    run(hud, port, car, now, 1500);
    CHECK(!hud.showing() && !place.marked());
    unlink(place.off.c_str());
    run(hud, port, car, now, 1500);
    CHECK(hud.showing());
    // A session that ends clears too.
    hud.onSessionEnded();
    run(hud, port, car, now, 600);
    CHECK(!hud.showing());
    // Stopping the service with nothing up sends nothing.
    before = port.sent.size();
    hud.finish(port);
    CHECK(port.sent.size() == before && port.shadows.size() == 6);
}

void pacing() {
    {
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        car.silent = true;  // the registry never answers: asked again 30 s later, three times, then given up
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 6000);
        CHECK(hud.phase() == MostHud::kIdle && port.sent.size() == 1 && hud.dueInMs(now) == 100);
        run(hud, port, car, now, 70000);
        CHECK(hud.phase() == MostHud::kAbsent && port.sent.size() == 3);
    }
    {
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        car.hudSilent = true;  // the cluster never answers: the reads judged after 10 s, asked three times 30 s apart
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 5000);
        CHECK(hud.phase() == MostHud::kSizes && port.sent.size() == 1 + 6 && port.at[2] - port.at[1] >= 20);
        run(hud, port, car, now, 80000);
        CHECK(hud.phase() == MostHud::kSizes && port.sent.size() == 1 + 18);
        run(hud, port, car, now, 15000);
        CHECK(hud.phase() == MostHud::kAbsent && port.sent.size() == 1 + 18);
    }
    {
        Place place;
        MostHud hud(place.config(5.0));  // the Ethernet KOMBI's chance first
        FakePort port;
        Car car;
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 4800);
        CHECK(port.sent.empty());
        run(hud, port, car, now, 2000);
        CHECK(hud.showing());
    }
}

void notAllowedOrBmw() {
    Place place;
    FakePort port;
    Car car;
    uint32_t now = 0;
    {
        MostHud hud(place.config());
        hud.setEthernetSeen();  // a G-series KOMBI took an arrow
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 1000);
        CHECK(port.sent.empty() && port.shadows.empty());
    }
    {
        MostHud hud(place.config());
        hud.setBmwGuiding(true);  // the BMW navigation guides
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 1000);
        place.touch(place.off);  // and the arrows off
        hud.setBmwGuiding(false);
        run(hud, port, car, now, 1500);
        CHECK(port.sent.empty() && port.shadows.empty());
        unlink(place.off.c_str());
    }
    {
        Car g;
        g.hud = false;  // a car without the MOST cluster
        MostHud hud(place.config());
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, g, now, 1000);
        CHECK(hud.phase() == MostHud::kAbsent && port.sent.size() == 1 && countFkt(port, kHudSize) == 0);
        CHECK(port.shadows.size() == 6 && !(port.shadows[3] & 0x10000) && !(port.shadows[4] & 0x10000) &&
              !(port.shadows[5] & 0x10000));  // the copies dropped again
    }
    {
        Car blank;
        blank.pictureW = 0;  // fields never set up: nothing drawn
        port.sent.clear();
        MostHud hud(place.config());
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, blank, now, 2000);
        CHECK(hud.phase() == MostHud::kSizes && countFkt(port, kHudSize) == 6);  // asked again later
        run(hud, port, blank, now, 70000);
        CHECK(hud.phase() == MostHud::kAbsent && countFkt(port, kHudSize) == 18 && countFkt(port, kHudPolygon) == 0);
    }
}

// Right after an iDrive restart the cluster answers the size reads late: they are taken, and the picture goes up.
void coldCluster() {
    {
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        car.sizeDelayMs = 1800;
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 6000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing() && countFkt(port, kHudSize) == 6);
    }
    {
        Place place;
        MostHud hud(place.config());
        FakePort port;
        Car car;
        car.hudSilent = true;  // nothing the first time, then the cluster wakes up: the missing sizes asked again
        uint32_t now = 0;
        tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
        run(hud, port, car, now, 5000);
        CHECK(hud.phase() == MostHud::kSizes && !hud.showing());
        car.hudSilent = false;
        run(hud, port, car, now, 40000);
        CHECK(hud.phase() == MostHud::kReady && hud.showing() && countFkt(port, kHudSize) == 12);
    }
}

// For minutes after a cold iDrive start the cluster answers every call seconds late (1.1 to 7.5 s on an F30): the arrow
// still goes up, eight calls waiting at most, nothing stops, and a copy of a call still waiting is not another client.
void lateAnswers() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    car.sizeDelayMs = 5300;
    car.answerDelayMs = 5300;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 3000);
    CHECK(hud.phase() == MostHud::kSizes && !hud.showing());
    run(hud, port, car, now, 30000);
    CHECK(hud.phase() == MostHud::kReady && hud.showing() && place.marked());
    CHECK(countFkt(port, kHudSize) == 6 && countFkt(port, kHudSwap) == 1 && countFkt(port, kHudText) == 3);
    const size_t first = 1 + 6;  // the first drawing call
    CHECK(port.sent.size() > first + 8 && port.at[first + 8] - port.at[first] >= 5300);
    // A new distance; the cluster's copy of that call comes while it waits, well after it was sent.
    tell(hud, 8, "Rue A", 250, "250", kUnitMetres);
    size_t before = port.sent.size();
    run(hud, port, car, now, 3500);
    CHECK(port.sent.size() == before + 1);
    const Message waiting = port.sent.back();
    hud.onMessage(answer(kHudFblock, waiting.out.fkt, kMostOpStartResultAck, waiting.data, 0x0100));
    tell(hud, 8, "Rue A", 200, "200", kUnitMetres);
    before = port.sent.size();
    run(hud, port, car, now, 8000);
    CHECK(port.sent.size() == before + 1 && port.sent.back().data == hudText(3, "200").data && hud.showing());
}

// Sizes answered, drawing calls never: eight go out, and only after 10 s without an answer does the drawing stop.
void silentDrawing() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    car.drawSilent = true;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 5000);
    CHECK(hud.phase() == MostHud::kReady && port.sent.size() == 1 + 6 + 8);
    run(hud, port, car, now, 7000);
    CHECK(hud.phase() == MostHud::kStopped && !hud.showing() && place.marked() && port.sent.size() == 1 + 6 + 8);
}

// The iDrive setting moves the guidance to the head-up display: the cluster says so, the picture sizes are read again
// and the arrow is drawn anew inside the HUD's smaller picture. The same place again, or the navigation's answer on the
// same function, changes nothing.
void headUpDisplay() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.showing() && countFkt(port, kHudSize) == 6);
    car.pictureW = 126;
    car.pictureH = 77;
    const Bytes hudPlace({0x02});
    hud.onMessage(answer(kGuidanceFblock, kGuidanceMovedFkt, 0x0, hudPlace, 0x0101));
    size_t before = port.sent.size();
    run(hud, port, car, now, 1000);
    CHECK(countFkt(port, kHudSize, before) == 2 && countFkt(port, kHudSwap, before) == 1);
    CHECK(countFkt(port, kHudText, before) == 0);  // the texts stay
    for (size_t i = before; i < port.sent.size(); ++i) {
        const Bytes& d = port.sent[i].data;
        if (port.sent[i].out.fkt != kHudPolygon) {
            continue;
        }
        for (size_t k = 6; k + 3 < d.size(); k += 4) {
            CHECK(((d[k] << 8) | d[k + 1]) < 126 && ((d[k + 2] << 8) | d[k + 3]) < 77);
        }
    }
    before = port.sent.size();
    hud.onMessage(answer(kGuidanceFblock, kGuidanceMovedFkt, 0x0, hudPlace, 0x0101));       // the same place again
    hud.onMessage(answer(kGuidanceFblock, kGuidanceMovedFkt, 0xC, Bytes({0x01}), 0x0100));  // the navigation
    run(hud, port, car, now, 1000);
    CHECK(port.sent.size() == before);
    // Back to the cluster.
    car.pictureW = 176;
    car.pictureH = 126;
    hud.onMessage(answer(kGuidanceFblock, kGuidanceMovedFkt, 0x0, Bytes({0x01}), 0x0101));
    run(hud, port, car, now, 1000);
    CHECK(countFkt(port, kHudSize, before) == 2 && countFkt(port, kHudSwap, before) == 1 && hud.showing());
}

void refusedThenNextSession() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    car.refuseFkt = kHudSwap;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.phase() == MostHud::kStopped && !hud.showing() && place.marked());
    CHECK(countFkt(port, kHudText) == 0);  // nothing after the refused call
    const size_t before = port.sent.size();
    run(hud, port, car, now, 1000);
    CHECK(port.sent.size() == before);
    // Next session: the possibly half-drawn picture is cleared first, then drawn again.
    car.refuseFkt = 0;
    hud.onSessionEnded();
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.phase() == MostHud::kReady && hud.showing() && countFkt(port, kHudShow, before) >= 2);
    CHECK(port.sent[before].data == hudShow(1, false).data);
}

void otherClientAndEchoes() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.showing());
    // Copies of our own calls on the shadow are not another client.
    const Message last = port.sent.back();
    hud.onMessage(answer(kHudFblock, last.out.fkt, kMostOpStartResultAck, last.data, 0x0100));
    tell(hud, 8, "Rue A", 250, "250", kUnitMetres);
    size_t before = port.sent.size();
    run(hud, port, car, now, 500);
    CHECK(port.sent.size() == before + 1);
    // The BMW navigation draws: the picture is left to it, uncleared, the marker removed.
    const Bytes polygon({0x00, 0x05, 0x00, 0x00, 0x03});
    hud.onMessage(answer(kHudFblock, kHudPolygon, kMostOpStartResultAck, polygon, 0x0100));
    tell(hud, 8, "Rue A", 200, "200", kUnitMetres);
    before = port.sent.size();
    run(hud, port, car, now, 1000);
    CHECK(port.sent.size() == before && !hud.showing() && !place.marked());
    // Thirty seconds later it may draw again.
    run(hud, port, car, now, 30000);
    CHECK(hud.showing());
}

void staleMarker() {
    Place place;
    place.touch(place.marker);
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    run(hud, port, car, now, 2000);  // no guidance, but a picture left by a killed run
    CHECK(hud.phase() == MostHud::kReady && !place.marked() && countFkt(port, kHudShow) == 1);
    CHECK(port.sent.back().data == hudText(4, "").data);
}

void finishClears() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 2000);
    CHECK(hud.showing() && place.marked());
    const size_t before = port.sent.size();
    hud.finish(port);
    CHECK(port.sent.size() == before + 6 && port.sent[before].data == hudShow(1, false).data && !place.marked());
}

void refusedSends() {
    Place place;
    MostHud hud(place.config());
    FakePort port;
    Car car;
    uint32_t now = 0;
    tell(hud, 8, "Rue A", 300, "300", kUnitMetres);
    run(hud, port, car, now, 300);  // registry, sizes, part of the frame
    CHECK(hud.phase() == MostHud::kReady && countFkt(port, kHudPolygon) > 0);
    port.refuseSends = true;  // vmostd refuses everything from here: five refusals, then stop
    run(hud, port, car, now, 2000);
    CHECK(hud.phase() == MostHud::kStopped && !hud.showing() && place.marked());
}

}  // namespace

int main() {
    registryAndSizes();
    calls();
    pictures();
    views();
    presenter();
    neighbours();
    drawsAndClears();
    pacing();
    notAllowedOrBmw();
    coldCluster();
    lateAnswers();
    silentDrawing();
    headUpDisplay();
    refusedThenNextSession();
    otherClientAndEchoes();
    staleMarker();
    finishClears();
    refusedSends();
    printf(
        "PASS kombi most: registry, sizes, calls, pictures, views, presenter, neighbours, drawing and clearing, "
        "pacing, Ethernet and BMW navigation first, cold cluster, late answers, silent cluster, head-up display, "
        "refusals, other client, stale "
        "marker, exit\n");
    return 0;
}
