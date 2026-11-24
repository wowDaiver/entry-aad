// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Tests platform/kombi_graph.cpp and kombi_someip.cpp: the cluster's guidance graph and its SOME/IP requests.

// The SHA-256 values come from the Python test client that drew routes and lane rows on a cluster and HUD with the
// same template and inputs: equal hashes mean the same bytes as on the car.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kombi_graph.hpp"
#include "kombi_graph_template.hpp"
#include "kombi_test_util.hpp"

namespace {

using namespace evo::kombi;

// SHA-256 (FIPS 180-4), enough for a few kilobytes.
class Sha256 {
public:
    Sha256() : bytes_(0), used_(0) {
        static const uint32_t kStart[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(state_, kStart, sizeof(state_));
    }
    void add(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            block_[used_++] = p[i];
            if (used_ == 64) {
                compress();
                used_ = 0;
            }
        }
        bytes_ += n;
    }
    std::string hex() {
        const uint64_t bits = bytes_ * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (used_ != 56) {
            add(&zero, 1);
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            const uint8_t b = static_cast<uint8_t>(bits >> shift);
            add(&b, 1);
        }
        std::string out;
        for (int i = 0; i < 8; ++i) {
            char word[9];
            snprintf(word, sizeof(word), "%08x", state_[i]);
            out += word;
        }
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }
    void compress() {
        static const uint32_t kRound[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 64; ++i) {
            if (i < 16) {
                w[i] = readBE32(&block_[4 * i]);
            } else {
                const uint32_t a = w[i - 15], b = w[i - 2];
                w[i] = w[i - 16] + (rotr(a, 7) ^ rotr(a, 18) ^ (a >> 3)) + w[i - 7] +
                       (rotr(b, 17) ^ rotr(b, 19) ^ (b >> 10));
            }
        }
        uint32_t v[8];
        memcpy(v, state_, sizeof(v));
        for (int i = 0; i < 64; ++i) {
            const uint32_t choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const uint32_t t1 = v[7] + (rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25)) + choose + kRound[i] + w[i];
            const uint32_t t2 = (rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22)) + majority;
            memmove(v + 1, v, 7 * sizeof(v[0]));
            v[4] += t1;
            v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) {
            state_[i] += v[i];
        }
    }

    uint32_t state_[8];
    uint8_t block_[64];
    uint64_t bytes_;
    size_t used_;
};

// The hash the Python client printed: per request, the method (2 bytes) then the body.
std::string hashOf(const std::vector<Request>& requests) {
    Sha256 h;
    for (size_t i = 0; i < requests.size(); ++i) {
        const uint8_t method[2] = {static_cast<uint8_t>(requests[i].method >> 8),
                                   static_cast<uint8_t>(requests[i].method)};
        h.add(method, 2);
        if (!requests[i].body.empty()) {
            h.add(&requests[i].body[0], requests[i].body.size());
        }
    }
    return h.hex();
}

uint8_t typeOf(const Request& r) {
    return r.body.size() > 8 ? r.body[8] : 0;
}

uint32_t indexOf(const Request& r) {
    return r.body.size() >= 8 ? readBE32(&r.body[4]) & 0xFFFFFF : 0;
}

bool is(const Request& r, uint16_t method, uint8_t type, uint32_t index) {
    return r.method == method && typeOf(r) == type && indexOf(r) == index;
}

using kombitest::textIn;

double routePositionIn(const Request& r) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = bits << 8 | r.body[22 + i];
    }
    double at;
    memcpy(&at, &bits, sizeof(at));
    return at;
}

void sameAsOnTheCar() {
    const RouteGraph hidden(0xC8, 255), factoryBar(0xC8, RouteGraph::kTemplateBar);
    const struct {
        const RouteGraph* graph;
        ClusterManeuver m;
        const char* sha;
    } starts[] = {
        {&hidden, ClusterManeuver(6, "TEST ST", "0.3", kClusterMiles),
         "f7995023fd50bc078537ed0cbe27d31a1f59185c3d89e3f81b83a734a7edb6c9"},
        {&factoryBar, ClusterManeuver(7, "Main St", "600", kClusterFeet),
         "ad422abdcd99af78568d72d1556d28522347ab1d3b1a8d03c41729b2b3316492"},
        // "Strasse" with a sharp s, then a car: a surrogate pair
        {&hidden,
         ClusterManeuver(10,
                         "Stra\xc3\x9f"
                         "e \xf0\x9f\x9a\x97",
                         "-", kNoClusterUnit),
         "134d27d9c6ed90beae7d48d90625dbbfdae62478407188cde43ad86e41824091"},
        {&hidden, ClusterManeuver(1, " ", "0.1", kClusterMiles),
         "1d4ecf5a30cd22fa8294890f8362f614d8629a88654e55106ce8c884b6e7ab03"},
        {&hidden, ClusterManeuver(6, std::string(40, 'X'), "0.5", kClusterMiles),
         "f436f9e12d13c94ad2c12e04437665924aa07593e0e60ea3fe1dabc125d65815"},
    };
    std::vector<Request> out;
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        CHECK(starts[i].graph->routeStart(starts[i].m, out) && out.size() == 97);
        CHECK(hashOf(out) == starts[i].sha);
    }
    CHECK(hidden.distanceUpdate("450", kClusterFeet, out) && out.size() == 1);
    CHECK(hashOf(out) == "86964e5eebd4b0f4abecacf722be406653fbec2138a64d756ba1d3a34535b2cc");
    CHECK(factoryBar.distanceUpdate("12.5", kClusterMiles, out));
    CHECK(hashOf(out) == "ed4a713611ed02e68edbf853dcd96e8ab302a0a2894af779c6ae619334c1bba7");
    hidden.cancel(out);
    CHECK(out.size() == 6 && hashOf(out) == "179ca21bd1493713bba5d338d90c296530e072abca958015848a939a8dbcaeda");
}

void templateKeptElsewhere() {
    // Nothing to change: the template itself, the factory's direction 7 and texts.
    std::vector<Request> out;
    CHECK(RouteGraph(0xC8, RouteGraph::kTemplateBar).routeStart(ClusterManeuver(7, "", "", kNoClusterUnit), out));
    CHECK(out.size() == tmpl::phase_b_count);
    for (size_t i = 0; i < out.size(); ++i) {
        const tmpl::Frame& t = tmpl::phase_b[i];
        CHECK(out[i].method == t.method && out[i].body == Bytes(t.body, t.body + t.len));
    }
    // Our handle in every request, consistent lengths, only maneuver 0 changed.
    CHECK(RouteGraph(0xC9, 255).routeStart(ClusterManeuver(6, "ELM ST", "0.4", kClusterMiles), out));
    int directions = 0, distances = 0, roads = 0, laterDistances = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const Bytes& b = out[i].body;
        CHECK(isAllowedMethod(out[i].method) && readBE32(&b[0]) == 0xC9);
        if (b.size() >= 17 && out[i].method != 0x0002) {
            CHECK(readBE32(&b[9]) == b.size() - 17);
        }
        if (is(out[i], 0x000A, 15, 0)) {
            CHECK(b[33] == kLeft);
            ++directions;
        } else if (is(out[i], 0x000A, 29, 0)) {
            int unit = -1;
            CHECK(textIn(b, &unit) == "0.4" && unit == kClusterMiles && b[22] == 255);
            ++distances;
        } else if (is(out[i], 0x0003, 3, 0)) {
            CHECK(textIn(b) == "ELM ST");
            ++roads;
        } else if (is(out[i], 0x000A, 29, 1)) {
            CHECK(textIn(b) == "400");
            ++laterDistances;
        }
    }
    CHECK(directions == 2 && distances == 2 && roads == 2 && laterDistances == 2);
    // What can't be written is refused rather than cut.
    RouteGraph g;
    CHECK(!g.routeStart(ClusterManeuver(1, std::string(126, 'R'), "1", kClusterKilometres), out) && out.empty());
    CHECK(g.routeStart(ClusterManeuver(1, std::string(125, 'R'), "1", kClusterKilometres), out));
    CHECK(!g.distanceUpdate(std::string(130, '9'), kClusterMetres, out));
}

void textAsUtf16() {
    std::vector<Request> out;
    CHECK(RouteGraph().routeStart(ClusterManeuver(1, "A\xc3\x9f\xf0\x9f\x9a\x97\xff", "1", kClusterMetres), out));
    // The length byte, the byte-order mark, A, sharp s, a car as a surrogate pair, U+FFFD for the stray byte, the end.
    const uint8_t want[] = {14, 0xFE, 0xFF, 0, 'A', 0x00, 0xDF, 0xD8, 0x3D, 0xDE, 0x97, 0xFF, 0xFD, 0, 0};
    int roads = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (is(out[i], 0x0003, 3, 0)) {
            const Bytes& b = out[i].body;
            CHECK(b.size() >= 27 + sizeof(want) && !memcmp(&b[27], want, sizeof(want)));
            ++roads;
        }
    }
    CHECK(roads == 2);
}

void cancels() {
    std::vector<Request> all, fallback;
    const RouteGraph g;
    g.cancel(all);
    g.cancelGuidance(fallback);
    CHECK(all.size() == tmpl::cancel_count && all[0].body[30] == 7 && all[1].body[30] == 5 && all[2].body[30] == 0);
    CHECK(fallback.size() == 2 && fallback[0].body == all[2].body && fallback[1].body == all[5].body);
}

void neutralTemplate() {
    // The captured street names were replaced with neutral ones of the same length.
    const char* const neutral[] = {"Main Street", "Second Road", "Third Drive", "Fourth Ave"};
    int named = 0;
    for (size_t i = 0; i < tmpl::phase_b_count; ++i) {
        if (tmpl::phase_b[i].method != 0x0003) {
            continue;
        }
        const std::string name = textIn(Bytes(tmpl::phase_b[i].body, tmpl::phase_b[i].body + tmpl::phase_b[i].len));
        if (name == "<none>") {
            continue;
        }
        bool known = false;
        for (size_t k = 0; k < 4; ++k) {
            known = known || name == neutral[k];
        }
        CHECK(known);
        ++named;
    }
    CHECK(named == 9 && tmpl::phase_b_count == 97 && tmpl::cancel_count == 6);
}

void maneuverInPlace() {
    const RouteGraph g;
    const ClusterManeuver m(kRight, "MAIN ST", "0.3", kClusterMiles);
    std::vector<Request> full, block, noGuiding;
    CHECK(g.routeStart(m, full) && g.maneuverUpdate(m, true, block) && block.size() == 9);
    for (size_t i = 0; i < block.size(); ++i) {
        CHECK(block[i].method == full[24 + i].method && block[i].body == full[24 + i].body);
    }
    CHECK(block[5].body[33] == kRight && textIn(block[4].body) == "0.3" && textIn(block[0].body) == "MAIN ST");
    CHECK(block.back().method == 0x0009 && block.back().body[30] == 2);  // guidance state 2, never route start 1
    CHECK(g.maneuverUpdate(m, false, noGuiding) && noGuiding.size() == 8);
    for (size_t i = 0; i < noGuiding.size(); ++i) {
        CHECK(noGuiding[i].method != 0x000B);
    }
}

// Lanes: the same four junctions as the drive the car showed them on, against the Python client.
void lanes() {
    const struct {
        int direction;
        const char* road;
        const char* lanes;  // kind:arrow per lane
        const char* sha;
    } junctions[] = {
        {6, "TEST ST", "1:6 2:1 2:1 2:1", "83148d1e9d09e178e99b074af6fbcecd06e6784361eb12de523e8225156ba38d"},
        {7, "MAIN ST", "2:6 2:1 2:1 1:7", "4611b5d77a91891f1cab8759fdfff0421f47d264bfefb5e1afc85c3ab0603045"},
        {7, "OAK AVE", "2:6 2:1 1:7", "23891cc1ef8230e1533641ad76fe730d20cece6beed2da1a2741826615372a2b"},
        {2, "ELM WAY", "1:2 1:2 2:1 2:1", "99f104c94377a84c51ffd9b292f003b874fa14c9f13729c4ef1305c149e68801"},
    };
    const struct {
        bool known;
        double metres, at;
        const char* sha;
    } positions[] = {
        {true, 395.0, 4132.0, "3982173efa6656487fe982ce2f3b7dae0cdcb2d1bc1112adbe00bac8e5a24e38"},
        {false, 0.0, 4132.0, "3982173efa6656487fe982ce2f3b7dae0cdcb2d1bc1112adbe00bac8e5a24e38"},
        {true, 10.0, 3962.0, "53221d352e214920da4314fd5d0211af5490e9705e2d64d14d30275a243d8c50"},
        {true, 500.0, 4132.0, "3982173efa6656487fe982ce2f3b7dae0cdcb2d1bc1112adbe00bac8e5a24e38"},
        {true, -5.0, 3952.0, "519972f83c9327d4f7682d468a7e069fb0a47d3c48bbb37a4748f682ebe01a26"},
    };
    const RouteGraph g;
    for (size_t j = 0; j < sizeof(junctions) / sizeof(junctions[0]); ++j) {
        ClusterLanes row;
        int kind, arrow, used;
        for (const char* p = junctions[j].lanes; sscanf(p, " %d:%d%n", &kind, &arrow, &used) == 2; p += used) {
            row.push_back(ClusterLane(static_cast<uint8_t>(kind), static_cast<uint8_t>(arrow)));
        }
        std::vector<Request> out, update;
        LaneSection section = {0, 0};
        CHECK(g.routeStart(ClusterManeuver(junctions[j].direction, junctions[j].road, "0.2", kClusterMiles, row), out,
                           &section));
        CHECK(out.size() == 93 && hashOf(out) == junctions[j].sha);
        CHECK(section.maneuverAt == 3952.0 && section.span == 180.0);
        for (size_t k = 0; k < sizeof(positions) / sizeof(positions[0]); ++k) {
            const double at = lanePosition(section, positions[k].known, positions[k].metres);
            CHECK(at == positions[k].at && g.positionUpdate(at, update) && update.size() == 1);
            CHECK(update[0].method == 0x0009 && routePositionIn(update[0]) == at && hashOf(update) == positions[k].sha);
        }
        // One lane model, right before maneuver 0's descriptions, which list it; the others list none.
        int models = 0;
        for (size_t i = 0; i < out.size(); ++i) {
            CHECK(isAllowedMethod(out[i].method));
            if (out[i].method == 0x0010) {
                ++models;
                CHECK(readBE32(&out[i].body[4]) == 0x1B000000 && i + 1 < out.size() && is(out[i + 1], 0x000A, 15, 0));
                CHECK(readBE32(&out[i].body[9]) == out[i].body.size() - 17);
            }
            if (is(out[i], 0x000A, 15, indexOf(out[i]))) {
                const uint32_t listed = readBE32(&out[i].body[54]);
                CHECK(indexOf(out[i]) == 0 ? listed == 4 && readBE32(&out[i].body[58]) == 0x1B000000 : listed == 0);
            }
        }
        CHECK(models == 2);
        CHECK(g.maneuverUpdate(ClusterManeuver(7, "R", "0.1", kClusterMiles, row), true, update) &&
              update.size() == 10);
    }
    // No lanes: the graph without a lane row, byte for byte.
    std::vector<Request> plain, empty;
    CHECK(g.routeStart(ClusterManeuver(6, "TEST ST", "0.3", kClusterMiles), plain));
    CHECK(g.routeStart(ClusterManeuver(6, "TEST ST", "0.3", kClusterMiles, ClusterLanes()), empty));
    CHECK(plain.size() == 97 && hashOf(plain) == hashOf(empty));
}

// Scope: no road shields or route-number icons, so never an image resource (model type 1, methods 0x0002/0x0003).
void noImages() {
    std::vector<Request> out, cancel;
    const RouteGraph g;
    CHECK(g.routeStart(ClusterManeuver(6, "TEST ST", "0.3", kClusterMiles, ClusterLanes(1, ClusterLane(1, 6))), out));
    g.cancel(cancel);
    out.insert(out.end(), cancel.begin(), cancel.end());
    for (size_t i = 0; i < out.size(); ++i) {
        CHECK(typeOf(out[i]) != 1);
        if (out[i].method == 0x0002 || out[i].method == 0x0003) {
            CHECK(readBE32(&out[i].body[4]) >> 24 != 0x01);
        }
    }
}

void someipFraming() {
    const Bytes request = encodeRequest(0x000A, Bytes(5, 0xAB), 0x1234);
    const uint8_t header[] = {0xB0, 0x06, 0x00, 0x0A, 0, 0, 0, 13, 0x7E, 0x01, 0x12, 0x34, 1, 1, 0, 0};
    CHECK(request.size() == 21 && !memcmp(&request[0], header, sizeof(header)) && request[16] == 0xAB);
    uint8_t reply[16];
    memcpy(reply, header, sizeof(reply));
    reply[14] = 0x80;
    CHECK(acknowledges(decodeHeader(reply), 0x000A, 0x1234));
    CHECK(!acknowledges(decodeHeader(reply), 0x000A, 0x1235) && !acknowledges(decodeHeader(reply), 0x0009, 0x1234));
    reply[15] = 1;
    CHECK(!acknowledges(decodeHeader(reply), 0x000A, 0x1234));  // an error code
    reply[15] = 0;
    reply[8] = 0x7E;
    reply[9] = 0x02;
    CHECK(!acknowledges(decodeHeader(reply), 0x000A, 0x1234));  // another client's answer
    const uint8_t cookie[16] = {0xFF, 0xFF, 0x80, 0x00, 0, 0, 0, 8, 0xDE, 0xAD, 0xBE, 0xEF, 1, 1, 2, 0};
    CHECK(isMagicCookie(decodeHeader(cookie)) && !isMagicCookie(decodeHeader(reply)));
    CHECK(decodeHeader(cookie).length == 8);
    const uint16_t allowed[] = {0x0002, 0x0003, 0x0004, 0x0009, 0x000A, 0x000B, 0x000C, 0x000E, 0x0010};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        CHECK(isAllowedMethod(allowed[i]));
    }
    CHECK(!isAllowedMethod(0x0001) && !isAllowedMethod(0x0005) && !isAllowedMethod(0x000D) && !isAllowedMethod(0x0011));
}

void singleManeuverGraph() {
    // Regression for G01 146: session 18 of the old graph created road 1 / text 2
    // and received 0x26. Keep a closed graph for the AA maneuver, not template roads.
    RouteGraph g(0xC8, 255, true);
    std::vector<Request> start, update, distance;
    CHECK(g.routeStart(ClusterManeuver(6, "FIRST", "250", kClusterMetres), start));
    CHECK(start.size() == 14);
    int texts = 0, roads = 0, descriptions = 0;
    Bytes initialDistance;
    for (size_t i = 0; i < start.size(); ++i) {
        const Request& r = start[i];
        CHECK(indexOf(r) == 0);  // every model and text resource belongs to maneuver 0
        if (r.method == 0x0003) {
            CHECK(textIn(r.body) == "FIRST");
            ++texts;
        } else if (r.method == 0x000E) {
            CHECK(readBE32(&r.body[4]) == 0x27000000);
            CHECK(readBE32(&r.body[32]) == 0x03000000);
            ++roads;
        } else if (is(r, 0x000A, 15, 0)) {
            CHECK(r.body[33] == kLeft);
            ++descriptions;
        } else if (is(r, 0x000A, 29, 0)) {
            initialDistance = r.body;
        }
    }
    CHECK(texts == 1 && roads == 1 && descriptions == 1 && !initialDistance.empty());
    CHECK(start.back().method == 0x0009 && start.back().body[30] == 2);
    CHECK(g.distanceUpdate("200", kClusterMetres, distance) && distance.size() == 1);
    CHECK(!memcmp(&initialDistance[23], &distance[0].body[23], 8));
    CHECK(textIn(distance[0].body) == "200");
    CHECK(g.maneuverUpdate(ClusterManeuver(7, "NEXT", "100", kClusterMetres), true, update));
    CHECK(update.size() == 8);
    for (size_t i = 0; i < update.size(); ++i) {
        CHECK(indexOf(update[i]) == 0);
        if (update[i].method == 0x0003) {
            CHECK(textIn(update[i].body) == "NEXT");
        }
        if (is(update[i], 0x000A, 15, 0)) {
            CHECK(update[i].body[33] == kRight);
        }
    }
    ClusterLanes lanes;
    lanes.push_back(ClusterLane(1, 1));
    LaneSection section;
    CHECK(g.routeStart(ClusterManeuver(6, "LANES", "50", kClusterMetres, lanes), start, &section));
    CHECK(start.size() == 15 && section.span > 0);
    for (size_t i = 0; i < start.size(); ++i) {
        CHECK(indexOf(start[i]) == 0);
    }
}

}  // namespace

int main() {
    sameAsOnTheCar();
    templateKeptElsewhere();
    textAsUtf16();
    cancels();
    neutralTemplate();
    maneuverInPlace();
    lanes();
    noImages();
    someipFraming();
    singleManeuverGraph();
    puts("PASS kombi graph: route start, update block, distance, lanes and positions as on the car, SOME/IP framing");
    return 0;
}
