// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The cluster's route-guidance graph (BMW.IC.Guidance), built from the captured factory route start. No I/O.

// Every request is a byte copy of the head unit's own route start (kombi_graph_template.hpp) in which only fields
// checked on the car change, so the cluster always gets a consistent graph. Only maneuver 0, the next one, changes:
//   - instance handle              body[0:4] of every request (0xC8 on every boot seen)
//   - ManeuverDescription (0x0F)   the direction, body[33]
//   - ManeuverDistance (0x1D)      the UTF-16BE text at its byte-order mark, the unit byte after its terminator, and
//   the
//                                  red distance bar's percentage, body[22] (255 hides it)
//   - TextResource (0x03)          the road name, UTF-16BE at its byte-order mark
// Body of a model: [0:4] handle, [4:8] model id (type << 24 | index), [8] type, [9:13] length of what follows [17],
// [13:17] tag, [17:] the model.
//
// Lanes: maneuver 0 gets one junction lane model, 0x1B000000 (method 0x0010), right before each of its 0x0F nodes,
// which then list it; the template's own lane models are left out (93 requests instead of 97). The cluster
// (HmiAggrNaviLaneGuiding::SetRicData) shows the row while the car's route position, from the last destination info
// (0x11), lies in the maneuver's lane section, and checks again on each one: a position update follows the graph.
#ifndef ENTRY_AAD_PLATFORM_KOMBI_GRAPH_HPP
#define ENTRY_AAD_PLATFORM_KOMBI_GRAPH_HPP

#include <stdint.h>

#include <string>
#include <vector>

#include "kombi_display.hpp"
#include "kombi_someip.hpp"

namespace evo {
namespace kombi {

// Maneuver 0 as the cluster shows it.
struct ClusterManeuver {
    int direction;         // Direction
    std::string road;      // "" keeps the template's
    std::string distance;  // "" keeps the template's
    int unit;              // ClusterUnit; negative (kNoClusterUnit) keeps the template's byte
    ClusterLanes lanes;    // empty: no lane row
    ClusterManeuver(int dir, const std::string& r, const std::string& d, int u, const ClusterLanes& l = ClusterLanes())
        : direction(dir), road(r), distance(d), unit(u), lanes(l) {}
};

// The stretch of route where the cluster shows maneuver 0's lane row: it ends at the maneuver, at route position
// maneuverAt (metres to the destination), and the car is reported at most `span` metres before it.
struct LaneSection {
    double maneuverAt;
    double span;
};

class RouteGraph {
public:
    static const int kTemplateBar = -1;

    // handle: the guidance instance. bar: the red bar's percentage in maneuver 0's distance; negative (kTemplateBar)
    // keeps the template's.
    explicit RouteGraph(uint32_t handle = 0xC8, int bar = 255, bool singleManeuver = false);

    // The whole route start, in the factory's order. With lanes, *section gets the lane row's section. False: the
    // template doesn't have the layout this code expects (nothing is to be sent).
    bool routeStart(const ClusterManeuver& m, std::vector<Request>& out, LaneSection* section = nullptr) const;
    // A new maneuver 0 on a route already shown: the factory's own update block (text, road, thresholds, guiding model,
    // distance, [lane model,] description, list, init, guidance state 2), byte for byte the one routeStart() holds.
    // No route-start state 1, so the cluster keeps its route. withGuiding false leaves the 0x07 guiding model out.
    bool maneuverUpdate(const ClusterManeuver& m, bool withGuiding, std::vector<Request>& out,
                        LaneSection* section = nullptr) const;
    // Maneuver 0's distance alone: the template's last maneuver-0 ManeuverDistance.
    bool distanceUpdate(const std::string& text, int unit, std::vector<Request>& out) const;
    // One destination info (0x11) with the car's route position.
    bool positionUpdate(double routePosition, std::vector<Request>& out) const;
    // The factory cancel: guidance state 7, 5, 0, an empty maneuver list, init, the destination cleared.
    void cancel(std::vector<Request>& out) const;
    // When the cancel is refused: its two requests whose byte 30 is 0 (guidance state 0, destination cleared).
    void cancelGuidance(std::vector<Request>& out) const;

private:
    Request copied(size_t templateIndex) const;
    bool patch(Request& r, const ClusterManeuver& m) const;
    bool addLanes(std::vector<Request>& requests, const ClusterLanes& lanes, LaneSection* section) const;

    uint32_t handle_;
    int bar_;
    bool singleManeuver_;
    int carRequest_;  // the template's destination info holding the car's route position; -1 when there is none
    double carAt_;
};

// The route position that keeps the lane row up: maneuver 0's position plus the metres to it, kept inside the section;
// the section's far end when the distance is unknown.
double lanePosition(const LaneSection& section, bool knownDistance, double metres);

}  // namespace kombi
}  // namespace evo

#endif
