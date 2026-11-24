// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The F-series cluster's guidance fields over MOST (see kombi_most.hpp).
#include "kombi_most.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "evo_log.hpp"
#include "kombi_display.hpp"
#include "service_contract.hpp"

namespace evo {
namespace kombi {

namespace {

const char* const kTag = "KombiMost";
const uint32_t kGapMs = 20;  // between two calls, at least
// A cold cluster answers every call 1.1 to 7.5 s late, for minutes after an iDrive start.
const uint32_t kAnswerMs = 10000;    // a call without an answer this long counts as missed
const uint32_t kSlowMs = 1500;       // an answer later than this is slow (logged)
const uint32_t kGivenUpMs = 30000;   // an answer to a call given up on is still recognised this long after the call
const size_t kWaitingMax = 8;        // calls waiting for their answers at once: one at a time takes minutes per arrow
const unsigned kSlowLogged = 3;      // slow or late answers logged (all are counted)
const uint32_t kRegistryMs = 1500;   // the registry's answer
const uint32_t kRetryMs = 30000;     // a registry or sizes that did not answer are asked again this much later
const uint32_t kSizeWaitMs = 10000;  // the size reads' answers, late ones included
const int kSizeTries = 3;            // size reads without usable answers before giving up for this run
const uint16_t kMinPictureW = 80, kMinPictureH = 60;  // smaller, no arrow fits
const unsigned kRemeasuresLogged = 20;
const uint32_t kForeignMs = 30000;  // another client drew on the fields: they are left to it this long
const uint32_t kEchoMs = 3000;      // our call's copy: while it waits, and this long after it went out or was answered
const int kMissedLimit = 5;         // calls in a row without an answer: the cluster is not there
const int kLookupTries = 3;         // registry requests without an answer before giving up for this run
const size_t kRecentCalls = 32;     // our calls kept to recognise their copies
const size_t kStreetChars = 30;     // the 6WA street field is 272 px wide
const size_t kDistanceChars = 4;    // and the distance field 80 px
const unsigned kManeuversLogged = 100;
const double kMetresPerMile = 1609.344;
const double kPi = 3.14159265358979323846;

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

HudCall fieldCall(uint16_t fkt, uint16_t field) {
    HudCall c;
    c.fkt = fkt;
    put16(c.data, field);
    return c;
}

HudCall fieldByteCall(uint16_t fkt, uint16_t field, uint8_t value) {
    HudCall c = fieldCall(fkt, field);
    c.data.push_back(value);
    return c;
}

bool drawingFunction(uint16_t fkt) {
    return (fkt >= 0x200 && fkt <= 0x22B) || (fkt >= 0xC03 && fkt <= 0xC09);
}

// ---- the picture, drawn in a 100 x 100 square (y down), right-hand versions; a left one is its mirror ----

struct P {
    double x, y;
};
using Poly = std::vector<P>;

const double kStroke = 8;     // half the width of an arrow's shaft
const double kHeadHalf = 21;  // half the base of its head

// A shaft from a to b: a rectangle.
Poly shaft(P a, P b, double half = kStroke) {
    const double dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    const double nx = -dy / len * half, ny = dx / len * half;
    Poly p;
    p.push_back(P{a.x + nx, a.y + ny});
    p.push_back(P{b.x + nx, b.y + ny});
    p.push_back(P{b.x - nx, b.y - ny});
    p.push_back(P{a.x - nx, a.y - ny});
    return p;
}

// Rounds the corner where two shafts meet.
Poly joint(P c, double radius = kStroke) {
    Poly p;
    for (int i = 0; i < 8; ++i) {
        const double a = (22.5 + 45.0 * i) * kPi / 180.0;
        p.push_back(P{c.x + radius * std::cos(a), c.y + radius * std::sin(a)});
    }
    return p;
}

// An arrow head: its base centred on base, its point at tip.
Poly head(P base, P tip, double half = kHeadHalf) {
    const double dx = tip.x - base.x, dy = tip.y - base.y, len = std::sqrt(dx * dx + dy * dy);
    const double nx = -dy / len * half, ny = dx / len * half;
    Poly p;
    p.push_back(tip);
    p.push_back(P{base.x - nx, base.y - ny});
    p.push_back(P{base.x + nx, base.y + ny});
    return p;
}

// A piece of the roundabout's ring, between two angles (degrees).
Poly ringPiece(P c, double inner, double outer, double from, double to) {
    const double a = from * kPi / 180.0, b = to * kPi / 180.0;
    Poly p;
    p.push_back(P{c.x + outer * std::cos(a), c.y + outer * std::sin(a)});
    p.push_back(P{c.x + outer * std::cos(b), c.y + outer * std::sin(b)});
    p.push_back(P{c.x + inner * std::cos(b), c.y + inner * std::sin(b)});
    p.push_back(P{c.x + inner * std::cos(a), c.y + inner * std::sin(a)});
    return p;
}

struct Piece {
    uint8_t colour;
    Poly poly;
};
using Glyph = std::vector<Piece>;

void add(Glyph& g, const Poly& p, uint8_t colour = kHudWhite) {
    g.push_back(Piece{colour, p});
}

Glyph straight() {
    Glyph g;
    add(g, shaft(P{50, 94}, P{50, 38}));
    add(g, head(P{50, 38}, P{50, 6}));
    return g;
}

Glyph turn() {  // 90 degrees to the right
    Glyph g;
    add(g, shaft(P{34, 94}, P{34, 50}));
    add(g, joint(P{34, 50}));
    add(g, shaft(P{34, 50}, P{64, 50}));
    add(g, head(P{64, 50}, P{94, 50}));
    return g;
}

Glyph halfTurn() {  // 45 degrees to the right: keep, fork, ramps, lane change
    Glyph g;
    add(g, shaft(P{38, 94}, P{38, 60}));
    add(g, joint(P{38, 60}));
    add(g, shaft(P{38, 60}, P{60, 38}));
    add(g, head(P{60, 38}, P{82, 16}));
    return g;
}

Glyph tightTurn() {  // 135 degrees to the right
    Glyph g;
    add(g, shaft(P{32, 94}, P{32, 30}));
    add(g, joint(P{32, 30}));
    add(g, shaft(P{32, 30}, P{56, 54}));
    add(g, head(P{56, 54}, P{78, 76}));
    return g;
}

Glyph uTurn() {  // up on the left, back down on the right
    Glyph g;
    add(g, shaft(P{32, 94}, P{32, 34}));
    add(g, joint(P{32, 34}));
    add(g, shaft(P{32, 34}, P{66, 34}));
    add(g, joint(P{66, 34}));
    add(g, shaft(P{66, 34}, P{66, 56}));
    add(g, head(P{66, 56}, P{66, 86}, 19));
    return g;
}

Glyph destination() {  // a map pin
    Glyph g;
    add(g, joint(P{50, 34}, 24));
    Poly tail;
    tail.push_back(P{31, 46});
    tail.push_back(P{69, 46});
    tail.push_back(P{50, 92});
    add(g, tail);
    add(g, joint(P{50, 34}, 9), 0);  // black: the pin's hole
    return g;
}

Glyph roundabout() {  // grey ring, entry from below, exit ahead
    Glyph g;
    const P centre = {50, 46};
    for (int i = 0; i < 8; ++i) {
        add(g, ringPiece(centre, 14, 23, 45.0 * i, 45.0 * (i + 1)), kHudGrey);
    }
    add(g, shaft(P{50, 94}, P{50, 66}, 7));
    add(g, head(P{50, 25}, P{50, 3}, 17));
    return g;
}

// The glyph for a direction and whether it is mirrored.
bool glyphFor(int direction, Glyph& g, bool& left) {
    left = false;
    switch (direction) {
        case kStraightOn:
            g = straight();
            return true;
        case kHalfLeft:
        case kHighwayInLeft:
        case kHighwayOutLeft:
        case kLaneChangeLeft:
        case kForkLeft:
            left = true;
            g = halfTurn();
            return true;
        case kHalfRight:
        case kHighwayInRight:
        case kHighwayOutRight:
        case kLaneChangeRight:
        case kForkRight:
            g = halfTurn();
            return true;
        case kTightLeft:
            left = true;
            g = tightTurn();
            return true;
        case kTightRight:
            g = tightTurn();
            return true;
        case kLeft:
            left = true;
            g = turn();
            return true;
        case kRight:
            g = turn();
            return true;
        case kUTurnLeft:
            left = true;
            g = uTurn();
            return true;
        case kUTurnRight:
            g = uTurn();
            return true;
        case kDestination:
            g = destination();
            return true;
        case kRoundaboutClockwise:
        case kRoundaboutCounterClockwise:
            g = roundabout();
            return true;
        default:
            return false;
    }
}

double clampTo(double v, double hi) {
    return v < 0 ? 0 : v > hi ? hi : v;
}

}  // namespace

HudLayout::HudLayout() : frame(1), street(2), distance(3), unit(4), pictureA(5), pictureB(6) {}

bool layoutFits(const FieldSize sizes[kHudFields], std::string* why) {
    const FieldSize &frame = sizes[0], &street = sizes[1], &distance = sizes[2], &unit = sizes[3];
    const FieldSize &a = sizes[4], &b = sizes[5];
    const char* problem = nullptr;
    if (a.w == 0 || a.h == 0 || frame.w == 0 || frame.h == 0) {
        problem = "a field is 0x0: they were never set up (the BMW navigation does it)";
    } else if (a.w != b.w || a.h != b.h) {
        problem = "the two picture buffers differ";
    } else if (a.w < kMinPictureW || a.h < kMinPictureH) {
        problem = "picture too small for an arrow";
    } else if (street.w < 60 || street.h < 12 || distance.w < 24 || distance.h < 12 || unit.w < 12 || unit.h < 12) {
        problem = "a text field too small";
    }
    if (problem && why) {
        *why = problem;
    }
    return !problem;
}

bool registryAddress(const uint8_t* data, size_t len, uint8_t fblock, uint8_t inst, uint16_t* address) {
    for (size_t i = 0; data && i + 4 <= len; i += 4) {
        if (data[i + 2] == fblock && data[i + 3] == inst) {
            *address = get16(data + i);
            return true;
        }
    }
    return false;
}

bool fieldSize(const uint8_t* data, size_t len, uint16_t* field, FieldSize* size) {
    if (!data || len < 6) {
        return false;
    }
    *field = get16(data);
    size->w = get16(data + len - 4);
    size->h = get16(data + len - 2);
    return true;
}

HudCall hudShow(uint16_t field, bool on) {
    return fieldByteCall(kHudShow, field, on ? 1 : 0);
}

HudCall hudSwap(uint16_t hide, uint16_t show) {
    HudCall c = fieldCall(kHudSwap, hide);
    put16(c.data, show);
    return c;
}

HudCall hudText(uint16_t field, const std::string& utf8) {
    HudCall c = fieldByteCall(kHudText, field, 0);
    for (size_t i = 0; i < utf8.size();) {
        const uint8_t b = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = '?';
        size_t n = 1;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            n = 2;
        } else if ((b & 0xF0) == 0xE0) {
            n = 3;
        } else if ((b & 0xF8) == 0xF0) {
            n = 4;
        }
        if (n > 1) {
            bool ok = i + n <= utf8.size();
            uint32_t v = b & (0x7F >> n);
            for (size_t k = 1; ok && k < n; ++k) {
                const uint8_t t = static_cast<uint8_t>(utf8[i + k]);
                ok = (t & 0xC0) == 0x80;
                v = (v << 6) | (t & 0x3F);
            }
            cp = ok && v >= 0x80 && v <= 0xFFFF && (v < 0xD800 || v > 0xDFFF) ? v : '?';
            if (!ok) {
                n = 1;
            }
        }
        put16(c.data, static_cast<uint16_t>(cp));
        i += n;
    }
    put16(c.data, 0);
    return c;
}

HudCall hudEmpty(uint16_t field) {
    return fieldCall(kHudEmpty, field);
}

HudCall hudSize(uint16_t field) {
    return fieldByteCall(kHudSize, field, 1);
}

std::vector<HudShape> hudPicture(int direction, uint16_t w, uint16_t h) {
    std::vector<HudShape> shapes;
    Glyph g;
    bool left;
    if (!glyphFor(direction, g, left) || w < 2 || h < 2) {
        return shapes;
    }
    // The part of the field left clear of the shade on the right, as seen on the 6WA (176 x 126: x 42-162, y 6-124).
    const double x0 = w * 0.24, x1 = w * 0.92, y0 = h * 0.05, y1 = h * 0.98;
    const double side = std::min(x1 - x0, y1 - y0);
    const double ox = x0 + (x1 - x0 - side) / 2, oy = y0 + (y1 - y0 - side) / 2;
    for (size_t i = 0; i < g.size(); ++i) {
        HudShape s;
        s.colour = g[i].colour;
        for (size_t k = 0; k < g[i].poly.size(); ++k) {
            const double x = left ? 100.0 - g[i].poly[k].x : g[i].poly[k].x;
            const double y = g[i].poly[k].y;
            HudPoint p;
            p.x = static_cast<int16_t>(std::lround(clampTo(ox + x * side / 100.0, w - 1)));
            p.y = static_cast<int16_t>(std::lround(clampTo(oy + y * side / 100.0, h - 1)));
            s.points.push_back(p);
        }
        // One winding for every polygon, clockwise on the screen, as the stock arrows are listed (a mirror reverses).
        long area = 0;
        for (size_t k = 0; k < s.points.size(); ++k) {
            const HudPoint &a = s.points[k], &b = s.points[(k + 1) % s.points.size()];
            area += static_cast<long>(a.x) * b.y - static_cast<long>(b.x) * a.y;
        }
        if (area < 0) {
            std::reverse(s.points.begin(), s.points.end());
        }
        shapes.push_back(s);
    }
    return shapes;
}

bool convex(const std::vector<HudPoint>& points) {
    const size_t n = points.size();
    if (n < 3) {
        return false;
    }
    int sign = 0;
    for (size_t i = 0; i < n; ++i) {
        const HudPoint &a = points[i], &b = points[(i + 1) % n], &c = points[(i + 2) % n];
        const long cross = static_cast<long>(b.x - a.x) * (c.y - b.y) - static_cast<long>(b.y - a.y) * (c.x - b.x);
        const int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
        if (s && sign && s != sign) {
            return false;
        }
        sign = s ? s : sign;
    }
    return sign != 0;
}

void pictureCalls(const std::vector<HudShape>& shapes, uint16_t buffer, HudCalls& out) {
    out.push_back(hudEmpty(buffer));
    out.push_back(fieldByteCall(kHudLine, buffer, 0));
    int colour = -1;
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].colour != colour) {
            colour = shapes[i].colour;
            out.push_back(fieldByteCall(kHudColour, buffer, shapes[i].colour));
        }
        HudCall c = fieldCall(kHudPolygon, buffer);
        put16(c.data, static_cast<uint16_t>(i));
        c.data.push_back(3);  // filled
        c.data.push_back(static_cast<uint8_t>(shapes[i].points.size()));
        for (size_t k = 0; k < shapes[i].points.size(); ++k) {
            put16(c.data, static_cast<uint16_t>(shapes[i].points[k].x));
            put16(c.data, static_cast<uint16_t>(shapes[i].points[k].y));
        }
        out.push_back(c);
    }
}

void viewCalls(const HudView& view, const HudView* shown, const HudLayout& layout, FieldSize picture, uint16_t& visible,
               HudCalls& out) {
    if (!shown || shown->direction != view.direction) {
        const uint16_t hidden = visible == layout.pictureA ? layout.pictureB : layout.pictureA;
        pictureCalls(hudPicture(view.direction, picture.w, picture.h), hidden, out);
        out.push_back(hudSwap(visible, hidden));
        out.push_back(hudEmpty(visible));
        visible = hidden;
    }
    if (!shown) {
        out.push_back(hudShow(layout.street, true));
        out.push_back(hudShow(layout.distance, true));
        out.push_back(hudShow(layout.unit, true));
    }
    if (!shown || shown->street != view.street) {
        out.push_back(hudText(layout.street, view.street));
    }
    if (!shown || shown->distance != view.distance) {
        out.push_back(hudText(layout.distance, view.distance));
    }
    if (!shown || shown->unit != view.unit) {
        out.push_back(hudText(layout.unit, view.unit));
    }
    if (!shown) {
        out.push_back(hudShow(layout.frame, true));
    }
}

void clearCalls(const HudLayout& layout, HudCalls& out) {
    out.push_back(hudShow(layout.frame, false));
    out.push_back(hudEmpty(layout.pictureA));
    out.push_back(hudEmpty(layout.pictureB));
    out.push_back(hudText(layout.street, ""));
    out.push_back(hudText(layout.distance, ""));
    out.push_back(hudText(layout.unit, ""));
}

// ---- what to show ----

HudPresenter::HudPresenter(const GuideSettings& settings)
    : settings_(settings),
      showing_(false),
      gapOn_(false),
      noDistanceOn_(false),
      gapAt_(0),
      noDistanceAt_(0),
      distanceAt_(0) {}

void HudPresenter::reset() {
    showing_ = false;
    gapOn_ = false;
    noDistanceOn_ = false;
}

bool HudPresenter::step(const Maneuver* next, double now, HudView& view) {
    if (!next) {
        if (!showing_) {
            return false;
        }
        if (!gapOn_) {
            gapOn_ = true;
            gapAt_ = now;
        }
        if (now - gapAt_ >= settings_.graceS) {
            reset();
            return false;
        }
        view = view_;
        return true;
    }
    gapOn_ = false;
    Maneuver m = *next;
    double metres;
    if (settings_.approachMiles > 0 && metresToManeuver(m, metres)) {
        const bool same = showing_ && m.sameTurn(turn_);
        if (metres > settings_.approachMiles * kMetresPerMile * (same ? settings_.hysteresis : 1.0)) {
            reset();
            return false;
        }
    }
    const bool sameTurn = showing_ && m.sameTurn(turn_);
    if (m.distanceText.empty() && sameTurn && !turn_.distanceText.empty()) {
        m.distanceText = turn_.distanceText;  // keep the last distance
        m.distanceUnit = turn_.distanceUnit;
        m.hasMetres = turn_.hasMetres;
        m.metres = turn_.metres;
    }
    HudView v;
    v.direction = clusterDirection(m.type);
    if (m.distanceText.empty()) {
        if (!noDistanceOn_) {
            noDistanceOn_ = true;
            noDistanceAt_ = now;
        }
        if (!showing_ && now - noDistanceAt_ < settings_.firstDistanceS) {
            return false;
        }
        v.distance = "-";
    } else {
        noDistanceOn_ = false;
        std::string text;
        int unit = m.distanceUnit;
        if (clusterUnit(m.distanceUnit) == kNoClusterUnit && m.hasMetres) {
            metric(static_cast<double>(m.metres), text, unit);  // a unit the phone did not give: metric
            m.distanceText = text;
            m.distanceUnit = unit;
        }
        shortDistance(m, kDistanceChars, text, unit);
        v.distance = text;
        if (text != "-" && clusterUnit(unit) != kNoClusterUnit) {
            v.unit = clusterUnitName(clusterUnit(unit));
        }
    }
    const int exit = roundaboutExit(m);
    v.street = m.road;
    if (exit > 0) {
        char label[24];
        snprintf(label, sizeof(label), "Exit %d", exit);
        v.street = m.road.empty() ? std::string(label) : std::string(label) + " - " + m.road;
    }
    v.street = utf8Prefix(v.street, kStreetChars);
    // The distance changes at most every distanceEveryS on the same maneuver.
    if (showing_ && v.direction == view_.direction && v.street == view_.street && v.distance != view_.distance &&
        now - distanceAt_ < settings_.distanceEveryS) {
        v.distance = view_.distance;
        v.unit = view_.unit;
    }
    if (!showing_ || v.distance != view_.distance) {
        distanceAt_ = now;
    }
    view_ = v;
    turn_ = m;
    showing_ = true;
    view = v;
    return true;
}

// ---- the Ethernet KOMBI's neighbour entry ----

bool arpHasNeighbour(const std::string& table, const std::string& ip) {
    size_t pos = table.find('\n');  // past the header line
    while (pos != std::string::npos && pos + 1 < table.size()) {
        const size_t end = table.find('\n', pos + 1);
        const std::string line = table.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
        pos = end;
        char addr[64], type[16], flags[16], mac[32];
        if (sscanf(line.c_str(), "%63s %15s %15s %31s", addr, type, flags, mac) != 4 || ip != addr) {
            continue;
        }
        const unsigned long f = strtoul(flags, nullptr, 16);
        if ((f & 0x2) && strcmp(mac, "00:00:00:00:00:00") != 0) {
            return true;
        }
    }
    return false;
}

bool ethernetNeighbour(const std::string& ip) {
    FILE* f = fopen("/proc/net/arp", "r");
    if (!f) {
        return false;
    }
    std::string table;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && table.size() < 65536) {
        table.append(buf, n);
    }
    fclose(f);
    return arpHasNeighbour(table, ip);
}

// ---- the calls, on the controller reader's thread ----

MostHudConfig::MostHudConfig()
    : markerFile(evo::contract::kKombiMostShown),
      offFile(evo::contract::kKombiOff),
      clusterAddress("160.48.199.96"),
      ethernetFirstS(5.0) {}

MostHud::MostHud(const MostHudConfig& config)
    : config_(config),
      messages_(0),
      generation_(0),
      bmw_(false),
      ethernet_(false),
      clock_(0),
      started_(false),
      startedAt_(0),
      inputsAt_(0),
      settingAt_(0),
      arpAt_(0),
      messageAt_(0),
      wantSince_(0),
      seenMessages_(0),
      seenGeneration_(0),
      settingOn_(false),
      arpSeen_(false),
      ethernetLogged_(false),
      allowed_(false),
      bmwNow_(false),
      has_(false),
      wantOn_(false),
      presenter_(config.guide),
      phase_(kIdle),
      shadowed_(false),
      address_(0xFFFF),
      lookupTries_(0),
      lookupAt_(0),
      retryAt_(0),
      registryIn_(false),
      sizesWaiting_(false),
      sizesAskedAt_(0),
      sizeTries_(0),
      remeasureDue_(false),
      remeasuring_(false),
      haveMovedTo_(false),
      remeasureAt_(0),
      remeasures_(0),
      lateSizes_(0),
      batch_(kNoBatch),
      batchFailed_(false),
      serial_(0),
      refused_(false),
      refusals_(0),
      slowAnswers_(0),
      lateAnswers_(0),
      lastSendAt_(0),
      missed_(0),
      foreignUntil_(0),
      foreignLogged_(false),
      shown_(false),
      unsure_(false),
      marked_(false),
      staleMarker_(false),
      firstAcked_(false),
      visible_(config.layout.pictureB),  // the stock navigation leaves the second buffer visible
      drawn_(0) {
    pthread_mutex_init(&mu_, nullptr);
    memset(sizes_, 0, sizeof(sizes_));
    memset(sizeIn_, 0, sizeof(sizeIn_));
    memset(sizeAskedAt_, 0, sizeof(sizeAskedAt_));
    memset(&drawnPicture_, 0, sizeof(drawnPicture_));
    memset(&pendingPicture_, 0, sizeof(pendingPicture_));
    staleMarker_ = access(config_.markerFile.c_str(), F_OK) == 0;
    marked_ = staleMarker_;
}

MostHud::~MostHud() {
    pthread_mutex_destroy(&mu_);
}

void MostHud::onNavStatus(uint16_t id, const uint8_t* body, size_t size, void* hud) {
    MostHud* h = static_cast<MostHud*>(hud);
    if (!h) {
        return;
    }
    pthread_mutex_lock(&h->mu_);
    if (h->inbox_.keep(id, body ? body : reinterpret_cast<const uint8_t*>(""), body ? size : 0)) {
        ++h->messages_;
    }
    pthread_mutex_unlock(&h->mu_);
}

void MostHud::onSessionEnded() {
    pthread_mutex_lock(&mu_);
    inbox_.clear();
    ++generation_;
    pthread_mutex_unlock(&mu_);
}

// The setting, the Ethernet KOMBI, the BMW navigation and the phone's next maneuver, every 250 ms.
void MostHud::readInputs(uint32_t now) {
    if (started_ && now - inputsAt_ < 250) {
        return;
    }
    inputsAt_ = now;
    if (!started_) {
        started_ = true;
        startedAt_ = now;
        settingAt_ = now - 1000;
        arpAt_ = now - 10000;
        messageAt_ = now;
    }
    pthread_mutex_lock(&mu_);
    const NavInbox inbox = inbox_;
    const unsigned messages = messages_, generation = generation_;
    pthread_mutex_unlock(&mu_);
    if (generation != seenGeneration_) {
        seenGeneration_ = generation;
        presenter_.reset();
        if (phase_ == kStopped) {
            phase_ = kReady;
            missed_ = 0;
            // The last session's calls no longer count against this one.
            givenUp_.insert(givenUp_.end(), waiting_.begin(), waiting_.end());
            waiting_.clear();
            ELOG(kTag, "new Android Auto session: the cluster may be drawn on again");
        }
    }
    if (messages != seenMessages_) {
        seenMessages_ = messages;
        messageAt_ = now;
    }
    if (now - settingAt_ >= 1000) {
        settingAt_ = now;
        settingOn_ = access(config_.offFile.c_str(), F_OK) != 0;
    }
    const bool ethernet = ethernet_;
    if (ethernet && !ethernetLogged_) {
        ethernetLogged_ = true;
        ELOG(kTag, "the Ethernet KOMBI took an arrow: the MOST cluster is left alone");
    }
    if (!ethernet && now - arpAt_ >= 10000) {
        arpAt_ = now;
        const bool seen = ethernetNeighbour(config_.clusterAddress);
        if (seen != arpSeen_) {
            ELOG(kTag, "KOMBI %s %s the neighbour table: the MOST cluster is %s", config_.clusterAddress.c_str(),
                 seen ? "in" : "no longer in", seen ? "left alone" : "allowed again");
        }
        arpSeen_ = seen;
    }
    allowed_ = settingOn_ && !ethernet && !arpSeen_;
    bmwNow_ = bmw_;
    Maneuver next;
    const Maneuver* shown = nullptr;
    if (inbox.guiding() && inbox.nextManeuver(next) && now - messageAt_ <= config_.guide.staleS * 1000) {
        shown = &next;
    }
    if (allowed_ && !bmwNow_) {
        has_ = presenter_.step(shown, now / 1000.0, view_);
    } else {
        presenter_.reset();
        has_ = false;
    }
    // The MOST registry only after the guidance wanted a picture for a while: the Ethernet KOMBI's chance first.
    const bool wanting = (has_ || staleMarker_) && allowed_ && !bmwNow_;
    if (wanting && !wantOn_) {
        wantSince_ = now;
    }
    wantOn_ = wanting;
}

bool MostHud::wants(const VmostMsgView& m) const {
    return m.fblock == kHudFblock || (m.fblock == kRegistryFblock && m.fkt == kRegistryFkt) ||
           (m.fblock == kGuidanceFblock && m.fkt == kGuidanceMovedFkt);
}

namespace {

bool sameCall(const HudCall& c, const VmostMsgView& m) {
    return c.fkt == m.fkt && c.data.size() == m.len && (m.len == 0 || memcmp(c.data.data(), m.data, m.len) == 0);
}

}  // namespace

bool MostHud::ours(const VmostMsgView& m, uint32_t now) const {
    for (size_t i = 0; i < waiting_.size(); ++i) {
        if (sameCall(waiting_[i].call, m)) {
            return true;
        }
    }
    for (size_t i = 0; i < recent_.size(); ++i) {
        const Sent& s = recent_[i];
        if (sameCall(s.call, m) && (now - s.at <= kEchoMs || (s.answered && now - s.answeredAt <= kEchoMs))) {
            return true;
        }
    }
    return false;
}

// The oldest call of the answer's function, of its field when the answer starts with one of theirs.
bool MostHud::takeAnswer(std::deque<Waiting>& list, const VmostMsgView& m, Waiting& out) {
    size_t pick = list.size();
    for (size_t i = 0; i < list.size(); ++i) {
        const HudCall& c = list[i].call;
        if (c.fkt != m.fkt) {
            continue;
        }
        if (m.len >= 2 && c.data.size() >= 2 && memcmp(c.data.data(), m.data, 2) == 0) {
            pick = i;
            break;
        }
        if (pick == list.size()) {
            pick = i;
        }
    }
    if (pick == list.size()) {
        return false;
    }
    out = list[pick];
    list.erase(list.begin() + static_cast<long>(pick));
    return true;
}

// An answer to one of our drawing calls, waiting or given up on; any other (another client's, a size read's) is left.
void MostHud::onAnswer(const VmostMsgView& m, uint32_t now) {
    Waiting w;
    bool late = false;
    if (!takeAnswer(waiting_, m, w)) {
        if (!takeAnswer(givenUp_, m, w)) {
            return;
        }
        late = true;
    }
    for (size_t i = 0; i < recent_.size(); ++i) {
        if (recent_[i].serial == w.serial) {
            recent_[i].answered = true;
            recent_[i].answeredAt = now;
        }
    }
    const unsigned after = static_cast<unsigned>(now - w.at);
    if (late) {
        if (++lateAnswers_ <= kSlowLogged) {
            ELOG(kTag, "the cluster answered function 0x%03x after %u ms, once given up on (%u such answers)", m.fkt,
                 after, lateAnswers_);
        }
    } else if (after > kSlowMs && ++slowAnswers_ <= kSlowLogged) {
        ELOG(kTag,
             "the cluster answered function 0x%03x after %u ms: slow, as a cold cluster is for minutes after an "
             "iDrive start (%u slow answers)",
             m.fkt, after, slowAnswers_);
    }
    if (m.op == kMostOpErrorAck || m.op == kMostOpError) {
        refused_ = true;
        ++refusals_;
        ELOG(kTag, "the cluster refused function 0x%03x (operation 0x%x, error %02x %02x)", m.fkt, m.op,
             m.len > 0 ? m.data[0] : 0, m.len > 1 ? m.data[1] : 0);
    } else {
        missed_ = 0;  // it is there
    }
}

// Waiting calls without an answer for 10 s are missed; the answers still to come are recognised for a while.
void MostHud::expire(uint32_t now) {
    while (!waiting_.empty() && now - waiting_.front().at >= kAnswerMs) {
        const Waiting w = waiting_.front();
        waiting_.pop_front();
        givenUp_.push_back(w);
        if (++missed_ == 1) {
            ELOG(kTag, "no answer to function 0x%03x within %u s", w.call.fkt, kAnswerMs / 1000);
        }
    }
    while (!givenUp_.empty() && (now - givenUp_.front().at >= kGivenUpMs || givenUp_.size() > kRecentCalls)) {
        givenUp_.pop_front();
    }
}

void MostHud::onMessage(const VmostMsgView& m) {
    const uint32_t now = clock_;
    if (m.fblock == kRegistryFblock) {
        if (phase_ == kLookup && m.fkt == kRegistryFkt && m.op == kMostOpStatus) {
            registry_.assign(m.data, m.data + m.len);
            registryIn_ = true;
        }
        return;
    }
    if (m.fblock == kGuidanceFblock) {
        onGuidanceMoved(m);
        return;
    }
    if (m.inst != kHudInstance) {
        return;
    }
    if (m.fkt == kHudSize && (m.op == kMostOpResultAck || m.op == kMostOpStatus)) {
        // A size is taken whenever it comes: a cold cluster answers seconds late.
        uint16_t field;
        FieldSize size;
        if (fieldSize(m.data, m.len, &field, &size) && field >= 1 && field <= kHudFields) {
            sizes_[field - 1] = size;
            sizeIn_[field - 1] = true;
            if (now - sizeAskedAt_[field - 1] > kSlowMs) {
                ++lateSizes_;
            }
        }
        return;
    }
    if (m.op <= 8) {  // a request: a copy of ours, or another client drawing (the BMW navigation)
        if (drawingFunction(m.fkt) && !ours(m, now)) {
            if (!foreignLogged_) {
                ELOG(kTag,
                     "another client draws on the cluster (address 0x%04x, function 0x%03x): Android Auto's picture "
                     "gives way",
                     m.addr, m.fkt);
                foreignLogged_ = true;
            }
            foreignUntil_ = now + kForeignMs;
        }
        return;
    }
    if (m.op == kMostOpResultAck || m.op == kMostOpStatus || m.op == kMostOpErrorAck || m.op == kMostOpError) {
        onAnswer(m, now);
    }
}

// After a call left unanswered, one at a time until an answer comes back.
size_t MostHud::room() const {
    const size_t most = missed_ > 0 ? 1 : kWaitingMax;
    return waiting_.size() < most ? most - waiting_.size() : 0;
}

int MostHud::dueInMs(uint32_t nowMs) const {
    if (refused_ || (batch_ != kNoBatch && queue_.empty() && waiting_.empty())) {
        return 0;
    }
    int32_t due = 100;
    if (!queue_.empty() && room() > 0) {
        due = std::min(due, static_cast<int32_t>(lastSendAt_ + kGapMs - nowMs));
    }
    if (!waiting_.empty()) {
        due = std::min(due, static_cast<int32_t>(waiting_.front().at + kAnswerMs - nowMs));
    }
    return due > 0 ? due : 0;
}

void MostHud::tick(uint32_t now, VmostPort& port) {
    clock_ = now;
    readInputs(now);
    expire(now);
    // Size reads are not waited for: a size is taken whenever it comes, and checkSizes() judges them.
    if (refused_) {
        refused_ = false;
        batchFailed_ = true;
        queue_.clear();
        unsure_ = true;  // part of a picture may be up, or not cleared
        stop("the cluster refused a call");
    } else if (missed_ >= kMissedLimit) {
        batchFailed_ = true;
        queue_.clear();
        stop("five calls in a row without an answer");
    }
    if (!queue_.empty()) {
        if (room() > 0 && static_cast<int32_t>(now - lastSendAt_) >= static_cast<int32_t>(kGapMs)) {
            sendNext(now, port);
        }
        return;
    }
    if (!waiting_.empty()) {
        return;  // the batch's last answers
    }
    if (batch_ != kNoBatch) {
        endBatch();
        if (phase_ == kAbsent) {
            unshadow(port);
        }
    }
    switch (phase_) {
        case kIdle:
            if (wantOn_ && now - wantSince_ >= static_cast<uint32_t>(config_.ethernetFirstS * 1000) &&
                static_cast<int32_t>(now - retryAt_) >= 0) {
                startLookup(now, port);
            }
            break;
        case kLookup:
            if (registryIn_) {
                onRegistry(now);
                if (phase_ == kAbsent) {
                    unshadow(port);
                }
            } else if (now - lookupAt_ >= kRegistryMs) {
                if (++lookupTries_ >= kLookupTries) {
                    ELOG(kTag, "the MOST registry never answered: no Android Auto picture on the cluster this run");
                    phase_ = kAbsent;
                    unshadow(port);
                } else {
                    ELOG(kTag, "the MOST registry did not answer: asked again in %u s", kRetryMs / 1000);
                    phase_ = kIdle;
                    retryAt_ = now + kRetryMs;
                }
            }
            break;
        case kSizes:
            checkSizes(now);
            if (phase_ == kAbsent) {
                unshadow(port);
            }
            break;
        case kReady:
            decide(now);
            break;
        case kAbsent:
        case kStopped:
            break;
    }
}

void MostHud::startLookup(uint32_t now, VmostPort& port) {
    if (!shadowed_) {
        const int a = port.shadow(kRegistryFblock, kRegistryInstance, true);
        const int b = port.shadow(kHudFblock, kHudInstance, true);
        const int c = port.shadow(kGuidanceFblock, kGuidanceInstance, true);
        ELOG(kTag,
             "copies of the MOST registry, the cluster's guidance fields (82/00) and the navigation (52/00): %s (%d), "
             "%s (%d), %s (%d)",
             a == 0 ? "OK" : "refused", a, b == 0 ? "OK" : "refused", b, c == 0 ? "OK" : "refused", c);
        shadowed_ = true;
    }
    static const uint8_t none[4] = {0, 0, 0, 0};
    VmostMsgOut m;
    memset(&m, 0, sizeof(m));
    m.addr = kRegistryAddress;
    m.fblock = kRegistryFblock;
    m.inst = kRegistryInstance;
    m.fkt = kRegistryFkt;
    m.op = kMostOpGet;
    m.data = none;
    const int rc = port.send(m);
    ELOG(kTag, "looking for the cluster's guidance fields (HUD_Terminal 82/00) in the MOST registry: request %s (%d)",
         rc == 0 ? "sent" : "refused", rc);
    registryIn_ = false;
    registry_.clear();
    lookupAt_ = now;
    phase_ = kLookup;
}

void MostHud::onRegistry(uint32_t now) {
    (void)now;
    const unsigned devices = static_cast<unsigned>(registry_.size() / 4);
    uint16_t address;
    if (!registryAddress(registry_.data(), registry_.size(), kHudFblock, kHudInstance, &address)) {
        ELOG(kTag, "MOST registry: %u entries, no HUD_Terminal (82/00): no MOST cluster to draw on", devices);
        phase_ = kAbsent;
        if (staleMarker_) {
            mark(false);
            staleMarker_ = false;
        }
        return;
    }
    address_ = address;
    uint16_t hud;
    const bool separateHud = registryAddress(registry_.data(), registry_.size(), kHudFblock, 0x02, &hud);
    ELOG(kTag, "MOST registry: %u entries; HUD_Terminal 82/00 at 0x%04x%s: reading its fields' sizes", devices,
         address_, separateHud ? " (a separate head-up display 82/02 too, left alone)" : "");
    sizeTries_ = 0;
    askSizes(false);
    phase_ = kSizes;
}

// The size reads, of every field or of those not answered yet.
void MostHud::askSizes(bool missingOnly) {
    HudCalls calls;
    for (uint16_t field = 1; field <= kHudFields; ++field) {
        if (!missingOnly || !sizeIn_[field - 1]) {
            sizeIn_[field - 1] = false;
            calls.push_back(hudSize(field));
        }
    }
    sizesWaiting_ = false;
    queue(kSizesBatch, calls);
}

bool MostHud::sizesComplete() const {
    for (size_t i = 0; i < kHudFields; ++i) {
        if (!sizeIn_[i]) {
            return false;
        }
    }
    return true;
}

// After the reads: every size in, or 3 s gone, judges them; a judged failure asks again 30 s later.
void MostHud::checkSizes(uint32_t now) {
    if (batch_ == kSizesBatch) {
        return;  // still asking
    }
    if (sizesWaiting_ && (sizesComplete() || now - sizesAskedAt_ >= kSizeWaitMs)) {
        sizesWaiting_ = false;
        onSizes(now);
    } else if (!sizesWaiting_ && static_cast<int32_t>(now - retryAt_) >= 0) {
        askSizes(!sizesComplete());  // unusable sizes: all again; missing ones: those only
    }
}

void MostHud::onSizes(uint32_t now) {
    const FieldSize* s = sizes_;
    ELOG(kTag,
         "fields: frame %ux%u, street %ux%u, distance %ux%u, unit %ux%u, pictures %ux%u and %ux%u (%u of %u read, "
         "%u late)",
         s[0].w, s[0].h, s[1].w, s[1].h, s[2].w, s[2].h, s[3].w, s[3].h, s[4].w, s[4].h, s[5].w, s[5].h,
         static_cast<unsigned>(sizeIn_[0] + sizeIn_[1] + sizeIn_[2] + sizeIn_[3] + sizeIn_[4] + sizeIn_[5]),
         static_cast<unsigned>(kHudFields), lateSizes_);
    std::string why;
    if (!sizesComplete()) {
        why = "a size was not given";
    } else if (!layoutFits(sizes_, &why)) {
        why = why.empty() ? "unknown" : why;
    }
    if (why.empty()) {
        ELOG(kTag, "guidance fields ready: Android Auto's next maneuver goes to the cluster when the phone guides");
        phase_ = kReady;
        return;
    }
    if (++sizeTries_ < kSizeTries) {
        ELOG(kTag, "these fields are not the ones drawn here yet (%s): asked again in %u s", why.c_str(),
             kRetryMs / 1000);
        retryAt_ = now + kRetryMs;
        return;
    }
    ELOG(kTag, "these fields are not the ones drawn here (%s, %d tries): no Android Auto picture on the cluster",
         why.c_str(), sizeTries_);
    phase_ = kAbsent;
}

// The cluster says the guidance moved between itself and the HUD: both picture sizes are read again, once per new place
// it names (it may repeat the same one many times a second). Only its own message: the navigation answers on the same
// function.
void MostHud::onGuidanceMoved(const VmostMsgView& m) {
    if (m.fkt != kGuidanceMovedFkt || m.inst != kGuidanceInstance || m.addr != address_ ||
        (phase_ != kReady && phase_ != kStopped)) {
        return;
    }
    if (haveMovedTo_ && movedTo_.size() == m.len && (m.len == 0 || memcmp(movedTo_.data(), m.data, m.len) == 0)) {
        return;
    }
    movedTo_.assign(m.data, m.data + m.len);
    haveMovedTo_ = true;
    remeasureDue_ = true;
}

void MostHud::decide(uint32_t now) {
    const bool has = has_, allowed = allowed_, bmw = bmwNow_;
    const HudView& view = view_;
    const bool foreign = static_cast<int32_t>(foreignUntil_ - now) > 0;
    if (!foreign) {
        foreignLogged_ = false;
    }
    if (bmw || foreign) {
        if (shown_ || unsure_ || staleMarker_) {
            ELOG(kTag, "the BMW navigation uses the cluster: Android Auto's picture left to it, not cleared");
            shown_ = false;
            unsure_ = false;
            staleMarker_ = false;
            mark(false);
        }
        return;
    }
    HudCalls calls;
    const uint16_t a = config_.layout.pictureA, b = config_.layout.pictureB;
    if (remeasureDue_) {
        remeasureDue_ = false;
        if (++remeasures_ <= kRemeasuresLogged) {
            ELOG(kTag, "the guidance moved between the cluster and the head-up display: picture size read again");
        }
        sizeIn_[a - 1] = false;
        sizeIn_[b - 1] = false;
        remeasuring_ = true;
        remeasureAt_ = now;
        calls.push_back(hudSize(a));
        calls.push_back(hudSize(b));
        queue(kRemeasureBatch, calls);
        return;
    }
    if (remeasuring_) {
        // Drawn at the new size once both are in; at the old one if they never come.
        if (!(sizeIn_[a - 1] && sizeIn_[b - 1]) && now - remeasureAt_ < kSizeWaitMs) {
            return;
        }
        remeasuring_ = false;
    }
    if (unsure_ || staleMarker_) {
        ELOG(kTag, "clearing a picture possibly left on the cluster (%s)",
             staleMarker_ ? "by a previous run" : "a call was refused");
        clearCalls(config_.layout, calls);
        queue(kClearBatch, calls);
        return;
    }
    const FieldSize picture = sizes_[a - 1];
    const bool fits = picture.w >= kMinPictureW && picture.h >= kMinPictureH;
    if (!has || !allowed || !fits) {
        if (shown_) {
            ELOG(kTag, "picture cleared (%s)",
                 !allowed ? "arrows off, or an Ethernet KOMBI"
                 : !has   ? "no maneuver to show"
                          : "picture field too small for an arrow");
            clearCalls(config_.layout, calls);
            queue(kClearBatch, calls);
        }
        return;
    }
    const bool resized = shown_ && (picture.w != drawnPicture_.w || picture.h != drawnPicture_.h);
    if (shown_ && view == shownView_ && !resized) {
        return;
    }
    HudView shown = shownView_;
    if (resized) {
        ELOG(kTag, "picture field now %ux%u (was %ux%u): arrow drawn again", picture.w, picture.h, drawnPicture_.w,
             drawnPicture_.h);
        shown.direction = -1;  // a new picture, the texts as they are
    }
    uint16_t visible = visible_;
    viewCalls(view, shown_ ? &shown : nullptr, config_.layout, picture, visible, calls);
    if (calls.empty()) {
        shownView_ = view;
        return;
    }
    if ((!shown_ || view.direction != shownView_.direction) && drawn_ < kManeuversLogged) {
        ELOG(kTag, "maneuver: direction %d, %s %s, street \"%s\"", view.direction, view.distance.c_str(),
             view.unit.c_str(), view.street.c_str());
    }
    mark(true);
    visible_ = visible;
    pending_ = view;
    pendingPicture_ = picture;
    queue(kDrawBatch, calls);
}

void MostHud::queue(Batch batch, const HudCalls& calls) {
    batch_ = batch;
    batchFailed_ = false;
    queue_.insert(queue_.end(), calls.begin(), calls.end());
}

void MostHud::sendNext(uint32_t now, VmostPort& port) {
    const HudCall call = queue_.front();
    queue_.pop_front();
    VmostMsgOut m;
    memset(&m, 0, sizeof(m));
    m.addr = address_;
    m.fblock = kHudFblock;
    m.inst = kHudInstance;
    m.fkt = call.fkt;
    m.op = kMostOpStartResultAck;
    m.len = static_cast<uint16_t>(call.data.size());
    m.data = call.data.data();
    const unsigned serial = ++serial_;
    recent_.push_back(Sent{call, now, serial, false, 0});
    while (recent_.size() > kRecentCalls) {
        recent_.pop_front();
    }
    lastSendAt_ = now;
    const int rc = port.send(m);
    if (rc == 0) {
        if (call.fkt == kHudSize) {
            // Read, not waited for: its answer is taken whenever it comes.
            const uint16_t field = call.data.size() >= 2 ? get16(call.data.data()) : 0;
            if (field >= 1 && field <= kHudFields) {
                sizeAskedAt_[field - 1] = now;
            }
        } else {
            waiting_.push_back(Waiting{call, now, serial});
        }
    } else {
        ELOG(kTag, "function 0x%03x not handed to vmostd (%d)", call.fkt, rc);
        if (++missed_ >= kMissedLimit) {
            batchFailed_ = true;
            queue_.clear();
            stop("vmostd refused five calls in a row");
        }
    }
}

void MostHud::endBatch() {
    const Batch batch = batch_;
    const bool failed = batchFailed_;
    batch_ = kNoBatch;
    batchFailed_ = false;
    switch (batch) {
        case kSizesBatch:
            sizesWaiting_ = true;  // checkSizes() takes it from here
            sizesAskedAt_ = clock_;
            break;
        case kRemeasureBatch:
            break;  // the answers come in whenever (onMessage)
        case kDrawBatch:
            if (failed) {
                unsure_ = true;
                break;
            }
            shown_ = true;
            shownView_ = pending_;
            drawnPicture_ = pendingPicture_;
            ++drawn_;
            if (!firstAcked_) {
                firstAcked_ = true;
                ELOG(kTag,
                     "first picture taken by the cluster; nothing on it? its coding may not show the navigation's "
                     "guidance");
            }
            break;
        case kClearBatch:
            if (!failed) {
                shown_ = false;
                unsure_ = false;
                staleMarker_ = false;
                mark(false);
            }
            break;
        case kNoBatch:
            break;
    }
}

void MostHud::stop(const char* why) {
    if (phase_ != kStopped && phase_ != kAbsent) {
        ELOG(kTag, "%s: no Android Auto picture on the cluster until the next Android Auto session", why);
        phase_ = kStopped;
    }
}

void MostHud::mark(bool up) {
    if (up == marked_) {
        return;
    }
    if (up) {
        const int fd = open(config_.markerFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            close(fd);
        }
    } else {
        unlink(config_.markerFile.c_str());
    }
    marked_ = up;
}

void MostHud::unshadow(VmostPort& port) {
    if (shadowed_) {
        port.shadow(kGuidanceFblock, kGuidanceInstance, false);
        port.shadow(kHudFblock, kHudInstance, false);
        port.shadow(kRegistryFblock, kRegistryInstance, false);
        shadowed_ = false;
    }
}

void MostHud::finish(VmostPort& port) {
    const bool bmw = bmw_;
    const bool mayBeUp = shown_ || unsure_ || batch_ == kDrawBatch;
    queue_.clear();
    givenUp_.insert(givenUp_.end(), waiting_.begin(), waiting_.end());  // not waited for any more
    waiting_.clear();
    if ((phase_ == kReady || phase_ == kStopped) && mayBeUp && !bmw) {
        HudCalls calls;
        clearCalls(config_.layout, calls);
        const unsigned refusalsBefore = refusals_;
        unsigned failed = 0;
        for (size_t i = 0; i < calls.size(); ++i) {
            const HudCall& call = calls[i];
            VmostMsgOut m;
            memset(&m, 0, sizeof(m));
            m.addr = address_;
            m.fblock = kHudFblock;
            m.inst = kHudInstance;
            m.fkt = call.fkt;
            m.op = kMostOpStartResultAck;
            m.len = static_cast<uint16_t>(call.data.size());
            m.data = call.data.data();
            if (port.send(m) != 0) {
                ++failed;
            } else {
                waiting_.push_back(Waiting{call, clock_, ++serial_});  // a refusal in the pause below is seen
            }
            port.pump(static_cast<int>(kGapMs));
        }
        waiting_.clear();
        const bool refusedOrFailed = failed > 0 || refusals_ != refusalsBefore;
        ELOG(kTag, "service stopping: picture cleared%s",
             refusedOrFailed ? " (not every call taken: the marker stays)" : "");
        if (!refusedOrFailed) {
            mark(false);
        }
    }
    unshadow(port);
}

}  // namespace kombi
}  // namespace evo
