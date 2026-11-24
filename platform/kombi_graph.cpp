// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Patches the captured route start for maneuver 0, adds its lane model, cuts out the update blocks: see
// kombi_graph.hpp.
#include "kombi_graph.hpp"

#include <cstring>

#include "kombi_graph_template.hpp"

namespace evo {
namespace kombi {

namespace {

// Offsets in a model's body.
const size_t kHandleAt = 0;
const size_t kModelIdAt = 4;
const size_t kModelTypeAt = 8;
const size_t kLengthAt = 9;      // u32: the body's length after kModelAt
const size_t kModelAt = 17;      // the model itself
const size_t kTypeEchoAt = 21;   // maneuver models repeat their type here
const size_t kBarAt = 22;        // ManeuverDistance: the red bar's percentage
const size_t kManeuverAt = 23;   // ManeuverDistance: the maneuver's route position (f64)
const size_t kCarAt = 22;        // destination info: the car's route position (f64)
const size_t kStateAt = 30;      // guidance state; also 0 in the destination clear
const size_t kDirectionAt = 33;  // ManeuverDescription
const size_t kLaneIdsAt = 54;    // ManeuverDescription: u32 byte length of its lane model ids, then the ids
const size_t kTextFrom = 18;     // where the search for a string's byte-order mark starts

enum ModelType : uint8_t {
    kTextModel = 3,
    kDescriptionModel = 15,
    kDestinationModel = 17,
    kLaneModel = 27,
    kDistanceModel = 29,
    kStateModel = 33,
};

// The methods this file looks at, by what they carry in the template.
const uint16_t kTextMethod = 0x0003;      // text resources
const uint16_t kRouteMethod = 0x0009;     // destination info, guidance state
const uint16_t kManeuverMethod = 0x000A;  // maneuver models
const uint16_t kGuidingMethod = 0x000B;   // guiding model
const uint16_t kLaneMethod = 0x0010;      // lane models (setLaneGuidingData)

// Through the first maneuver list, init and guidance-state 2. G01 146 accepts this complete
// block, but rejects the next road (0x27000001) in the captured multi-road route. AA exposes
// only maneuver 0 here: do not create the template's unrelated roads/texts or reference them.
const size_t kSingleManeuverCount = 14;

const uint32_t kOurLaneModel = 0x1B000000;  // lane model 0
const double kFallbackSpan = 120.0;         // when the template's car position isn't before maneuver 0
const double kSectionLead = 100.0;          // the section also starts this far before the car position reported

uint8_t modelType(const Bytes& b) {
    return b.size() > kModelTypeAt ? b[kModelTypeAt] : 0;
}

uint32_t modelIndex(const Bytes& b) {
    return b.size() >= kModelTypeAt ? readBE32(&b[kModelIdAt]) & 0xFFFFFF : 0;
}

bool isModel(const Request& r, uint16_t method, uint8_t type) {
    return r.method == method && modelType(r.body) == type;
}

void updateLength(Bytes& b) {
    writeBE32(&b[kLengthAt], static_cast<uint32_t>(b.size() - kModelAt));
}

double readDouble(const uint8_t* p) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = bits << 8 | p[i];
    }
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void writeDouble(uint8_t* p, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    for (int i = 7; i >= 0; --i, bits >>= 8) {
        p[i] = static_cast<uint8_t>(bits);
    }
}

void appendDouble(Bytes& b, double value) {
    b.resize(b.size() + 8);
    writeDouble(&b[b.size() - 8], value);
}

void appendUtf16(Bytes& b, uint32_t unit) {
    b.push_back(static_cast<uint8_t>(unit >> 8));
    b.push_back(static_cast<uint8_t>(unit));
}

// UTF-8 to UTF-16BE. A byte that doesn't start a whole character becomes U+FFFD; code points above U+FFFF a pair.
Bytes utf16(const std::string& s) {
    Bytes out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t lead = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t width = 1;
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            width = 2;
            cp = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            width = 3;
            cp = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            width = 4;
            cp = lead & 0x07;
        }
        if (width > s.size() - i) {
            cp = 0xFFFD;
            width = 1;
        }
        for (size_t k = 1; k < width; ++k) {
            const uint8_t next = static_cast<uint8_t>(s[i + k]);
            if ((next & 0xC0) != 0x80) {
                cp = 0xFFFD;
                width = 1;
                break;
            }
            cp = cp << 6 | (next & 0x3F);
        }
        i += width;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            appendUtf16(out, static_cast<uint16_t>(0xD800 | cp >> 10));
            appendUtf16(out, static_cast<uint16_t>(0xDC00 | (cp & 0x3FF)));
        } else {
            appendUtf16(out, cp);
        }
    }
    return out;
}

// Replaces the UTF-16BE string at the first byte-order mark from kTextFrom. Its length byte, just before the mark,
// counts the mark, the characters and the 2-byte terminator. *after: where the bytes after the new string start. False
// when there is no string or the text doesn't fit the length byte.
bool replaceText(Bytes& b, const std::string& text, size_t* after) {
    for (size_t mark = kTextFrom; mark + 1 < b.size(); ++mark) {
        if (b[mark] != 0xFE || b[mark + 1] != 0xFF) {
            continue;
        }
        const size_t lengthAt = mark - 1, restAt = mark + b[lengthAt];
        if (restAt > b.size()) {
            return false;
        }
        const Bytes chars = utf16(text);
        const size_t length = 2 + chars.size() + 2;
        if (length > 0xFF) {
            return false;
        }
        Bytes rebuilt(b.begin(), b.begin() + static_cast<long>(lengthAt));
        rebuilt.push_back(static_cast<uint8_t>(length));
        rebuilt.push_back(0xFE);
        rebuilt.push_back(0xFF);
        rebuilt.insert(rebuilt.end(), chars.begin(), chars.end());
        rebuilt.push_back(0);
        rebuilt.push_back(0);
        rebuilt.insert(rebuilt.end(), b.begin() + static_cast<long>(restAt), b.end());
        b.swap(rebuilt);
        updateLength(b);
        if (after) {
            *after = mark + length;
        }
        return true;
    }
    return false;
}

// The lane model 0x1B, laid out as 126 factory requests are (each encodes again byte for byte with this):
//   handle | model id | 0x1B | length | tag 1 | model id | 0x1B | u32 n + n lane bytes | u32 m + m boundary bytes
//   | f64 start | f64 start | f64 length | 0
// One lane: 1 (valid) | kind | 1 | u32 1 + the arrow | u32 0 (extra bytes; the factory sends some only far from the
// junction) | u32 0. A boundary byte sits left of each lane, then a 0: 0 at the road's edge and between lanes with
// different arrows, 2 between lanes with the same arrow (as in factory junction sections; Android Auto has no lane
// lines). start is the route position where the section begins; it ends at the maneuver (start - length).
Bytes laneModel(uint32_t handle, const ClusterLanes& lanes, double start, double length) {
    Bytes laneBytes, boundaries, model;
    for (size_t i = 0; i < lanes.size(); ++i) {
        laneBytes.push_back(1);
        laneBytes.push_back(lanes[i].kind);
        laneBytes.push_back(1);
        appendBE32(laneBytes, 1);
        laneBytes.push_back(lanes[i].arrow);
        appendBE32(laneBytes, 0);
        appendBE32(laneBytes, 0);
        boundaries.push_back(i > 0 && lanes[i].arrow == lanes[i - 1].arrow ? 2 : 0);
    }
    boundaries.push_back(0);
    appendBE32(model, kOurLaneModel);
    model.push_back(kLaneModel);
    appendBE32(model, static_cast<uint32_t>(laneBytes.size()));
    model.insert(model.end(), laneBytes.begin(), laneBytes.end());
    appendBE32(model, static_cast<uint32_t>(boundaries.size()));
    model.insert(model.end(), boundaries.begin(), boundaries.end());
    appendDouble(model, start);
    appendDouble(model, start);
    appendDouble(model, length);
    model.push_back(0);
    Bytes body;
    appendBE32(body, handle);
    appendBE32(body, kOurLaneModel);
    body.push_back(kLaneModel);
    appendBE32(body, static_cast<uint32_t>(model.size()));
    appendBE32(body, 1);
    body.insert(body.end(), model.begin(), model.end());
    return body;
}

// A ManeuverDescription with its lane model ids replaced. False when it hasn't the expected layout.
bool setLaneModels(Bytes& b, const std::vector<uint32_t>& ids) {
    if (modelType(b) != kDescriptionModel || b.size() < kLaneIdsAt + 8 || b[kTypeEchoAt] != kDescriptionModel) {
        return false;
    }
    const uint32_t listed = readBE32(&b[kLaneIdsAt]);
    if (listed % 4 != 0 || b.size() < kLaneIdsAt + 4 + static_cast<size_t>(listed) + 4) {
        return false;
    }
    Bytes rebuilt(b.begin(), b.begin() + static_cast<long>(kLaneIdsAt));
    appendBE32(rebuilt, static_cast<uint32_t>(4 * ids.size()));
    for (size_t i = 0; i < ids.size(); ++i) {
        appendBE32(rebuilt, ids[i]);
    }
    rebuilt.insert(rebuilt.end(), b.begin() + static_cast<long>(kLaneIdsAt + 4 + listed), b.end());
    updateLength(rebuilt);
    b.swap(rebuilt);
    return true;
}

}  // namespace

RouteGraph::RouteGraph(uint32_t handle, int bar, bool singleManeuver)
    : handle_(handle), bar_(bar), singleManeuver_(singleManeuver), carRequest_(-1), carAt_(0) {
    // The car's position in the template: its last destination info with a route position (4132 m).
    const size_t count = singleManeuver_ ? kSingleManeuverCount : tmpl::phase_b_count;
    for (size_t i = 0; i < count; ++i) {
        const tmpl::Frame& t = tmpl::phase_b[i];
        if (t.method != kRouteMethod || t.len < kCarAt + 8 || t.body[kModelTypeAt] != kDestinationModel) {
            continue;
        }
        const double at = readDouble(t.body + kCarAt);
        if (at > 0) {
            carRequest_ = static_cast<int>(i);
            carAt_ = at;
        }
    }
}

Request RouteGraph::copied(size_t templateIndex) const {
    const tmpl::Frame& t = tmpl::phase_b[templateIndex];
    Request r;
    r.method = t.method;
    r.body.assign(t.body, t.body + t.len);
    writeBE32(&r.body[kHandleAt], handle_);
    return r;
}

// Maneuver 0's own requests get the maneuver; every request gets our handle. False: unexpected layout.
bool RouteGraph::patch(Request& r, const ClusterManeuver& m) const {
    Bytes& b = r.body;
    writeBE32(&b[kHandleAt], handle_);
    if (modelIndex(b) != 0) {
        return true;
    }
    if (isModel(r, kManeuverMethod, kDescriptionModel)) {
        if (b.size() <= kDirectionAt || b[kTypeEchoAt] != kDescriptionModel) {
            return false;
        }
        b[kDirectionAt] = static_cast<uint8_t>(m.direction);
    } else if (isModel(r, kManeuverMethod, kDistanceModel)) {
        if (bar_ >= 0) {
            // The template's bar is 0 or 100.
            if (b.size() <= kBarAt || b[kTypeEchoAt] != kDistanceModel ||
                (b[kBarAt] != 0 && b[kBarAt] != 100 && b[kBarAt] != bar_)) {
                return false;
            }
            b[kBarAt] = static_cast<uint8_t>(bar_);
        }
        if (!m.distance.empty()) {
            size_t unitAt = 0;
            if (!replaceText(b, m.distance, &unitAt)) {
                return false;
            }
            if (m.unit >= 0) {
                // The template was captured in miles and feet.
                if (unitAt >= b.size() || (b[unitAt] != kClusterMiles && b[unitAt] != kClusterFeet)) {
                    return false;
                }
                b[unitAt] = static_cast<uint8_t>(m.unit);
            }
        }
    } else if (isModel(r, kTextMethod, kTextModel) && !m.road.empty()) {
        return replaceText(b, m.road, nullptr);
    }
    return true;
}

bool RouteGraph::routeStart(const ClusterManeuver& m, std::vector<Request>& out, LaneSection* section) const {
    out.clear();
    const size_t count = singleManeuver_ ? kSingleManeuverCount : tmpl::phase_b_count;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(copied(i));
        if (!patch(out.back(), m)) {
            out.clear();
            return false;
        }
    }
    return m.lanes.empty() || addLanes(out, m.lanes, section);
}

bool RouteGraph::addLanes(std::vector<Request>& requests, const ClusterLanes& lanes, LaneSection* section) const {
    const std::vector<uint32_t> ours(1, kOurLaneModel), none;
    std::vector<Request> out;
    out.reserve(requests.size() + 4);
    bool placed = false;
    double maneuverAt = 0;
    for (size_t i = 0; i < requests.size(); ++i) {
        Request& r = requests[i];
        if (r.method == kLaneMethod) {
            continue;  // the template's own lane models
        }
        const bool maneuverZero = modelIndex(r.body) == 0;
        if (isModel(r, kManeuverMethod, kDistanceModel) && maneuverZero && r.body.size() >= kManeuverAt + 8) {
            maneuverAt = readDouble(&r.body[kManeuverAt]);
            placed = true;
        }
        if (isModel(r, kManeuverMethod, kDescriptionModel)) {
            if (maneuverZero) {
                if (!placed) {
                    return false;
                }
                // The section ends at the maneuver and starts kSectionLead before the car position reported.
                LaneSection s;
                s.maneuverAt = maneuverAt;
                s.span = carAt_ > maneuverAt ? carAt_ - maneuverAt : kFallbackSpan;
                Request model;
                model.method = kLaneMethod;
                model.body = laneModel(handle_, lanes, maneuverAt + s.span + kSectionLead, s.span + kSectionLead);
                out.push_back(model);
                if (!setLaneModels(r.body, ours)) {
                    return false;
                }
                if (section) {
                    *section = s;
                }
            } else if (r.body.size() >= kLaneIdsAt + 4 && readBE32(&r.body[kLaneIdsAt]) != 0) {
                if (!setLaneModels(r.body, none)) {
                    return false;
                }
            }
        }
        out.push_back(r);
    }
    requests.swap(out);
    return true;
}

bool RouteGraph::maneuverUpdate(const ClusterManeuver& m, bool withGuiding, std::vector<Request>& out,
                                LaneSection* section) const {
    out.clear();
    std::vector<Request> all;
    if (!routeStart(m, all, section)) {
        return false;
    }
    // The block opens with the last maneuver-0 text resource and closes with the next guidance state.
    size_t from = all.size();
    for (size_t i = 0; i < all.size(); ++i) {
        if (isModel(all[i], kTextMethod, kTextModel) && modelIndex(all[i].body) == 0) {
            from = i;
        }
    }
    bool described = false, distanced = false;
    for (size_t i = from; i < all.size(); ++i) {
        const Request& r = all[i];
        if (r.method == kGuidingMethod && !withGuiding) {
            continue;
        }
        out.push_back(r);
        if (r.method == kManeuverMethod && modelIndex(r.body) == 0) {
            described = described || modelType(r.body) == kDescriptionModel;
            distanced = distanced || modelType(r.body) == kDistanceModel;
        }
        if (isModel(r, kRouteMethod, kStateModel)) {
            break;
        }
    }
    if (!described || !distanced || out.empty() || out.back().method != kRouteMethod) {
        out.clear();
        return false;
    }
    return true;
}

bool RouteGraph::distanceUpdate(const std::string& text, int unit, std::vector<Request>& out) const {
    out.clear();
    const size_t count = singleManeuver_ ? kSingleManeuverCount : tmpl::phase_b_count;
    for (size_t i = count; i-- > 0;) {
        if (tmpl::phase_b[i].method != kManeuverMethod) {
            continue;
        }
        Request r = copied(i);
        if (modelType(r.body) != kDistanceModel || modelIndex(r.body) != 0) {
            continue;
        }
        if (!patch(r, ClusterManeuver(0, std::string(), text, unit))) {
            return false;
        }
        out.push_back(r);
        return true;
    }
    return false;
}

bool RouteGraph::positionUpdate(double routePosition, std::vector<Request>& out) const {
    out.clear();
    if (carRequest_ < 0) {
        return false;
    }
    out.push_back(copied(static_cast<size_t>(carRequest_)));
    writeDouble(&out.back().body[kCarAt], routePosition);
    return true;
}

void RouteGraph::cancel(std::vector<Request>& out) const {
    out.clear();
    for (size_t i = 0; i < tmpl::cancel_count; ++i) {
        Request r;
        r.method = tmpl::cancel[i].method;
        r.body.assign(tmpl::cancel[i].body, tmpl::cancel[i].body + tmpl::cancel[i].len);
        writeBE32(&r.body[kHandleAt], handle_);
        out.push_back(r);
    }
}

void RouteGraph::cancelGuidance(std::vector<Request>& out) const {
    std::vector<Request> all;
    cancel(all);
    out.clear();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].method == kRouteMethod && all[i].body.size() > kStateAt && all[i].body[kStateAt] == 0) {
            out.push_back(all[i]);
        }
    }
}

double lanePosition(const LaneSection& section, bool knownDistance, double metres) {
    if (!knownDistance) {
        return section.maneuverAt + section.span;
    }
    double ahead = metres < 0.0 ? 0.0 : metres;
    if (ahead > section.span) {
        ahead = section.span;
    }
    return section.maneuverAt + ahead;
}

}  // namespace kombi
}  // namespace evo
