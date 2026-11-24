// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Android Auto's next maneuver on the instrument cluster of F-series cars, over MOST.

// On these cars the cluster is a MOST device, not the Ethernet KOMBI of the G-series (kombi_client.hpp): its guidance
// fields belong to the function block HUD_Terminal (FBlock 0x82, instance 0), which the BMW navigation fills. As a
// tester described it on an F52 with the 6WA cluster (#79); not yet tried on a car by this project:
//   - the device's address is in the NetworkMaster's central registry (FBlock 0x02, function 0xA01, Get): records of
//     4 bytes, address (big-endian), FBlock, instance;
//   - every call is a StartResultAck (OpType 6), answered by a ResultAck (0xD) or an ErrorAck (0xF); parameters are
//     big-endian and start with the field's handle;
//   - 6WA fields: 1 the guidance frame, 2 the street, 3 the distance, 4 its unit, 5 and 6 the picture (two buffers,
//     176x126), sizes read with function 0xC04; the right edge of the picture is in the shade;
//   - a picture is filled polygons (0x21D) in a colour (0x21B), drawn into the hidden buffer, then 0x20E swaps the
//     buffers; texts are UTF-16BE (0x210); 0x204 shows or hides a field; 0x220 empties one.
// Nothing is created on the cluster: when the fields are missing or sized otherwise, nothing is drawn. Calls go out
// 20 ms apart at least, at most eight of them waiting for their answers; an answer is the oldest waiting call's of its
// function, of its field when the answer starts with one; after a call left unanswered, one at a time until an answer
// comes back. A refused call stops the drawing until the next Android Auto session; so do five calls in a row left
// unanswered for 10 s.
//
// Seen by another project on F-series cars (Sq00ky's, F20/F30 with the 6WA cluster): for minutes after a cold iDrive
// start the cluster answers every call, but 1.1 to 7.5 s late. So a call waits 10 s for its answer, and one that came
// later still belongs to it, not to a newer call; a copy of one of our calls is ours while the call waits and for 3 s
// after it was sent or answered; a size is taken whenever it comes, the size reads are judged after 10 s, and missing
// or unusable sizes are asked again 30 s later, three times in all. On a car with a head-up display, the iDrive setting
// moves the guidance between the cluster and the HUD, whose pictures differ (176x126 and 126x77 on an F30); the cluster
// then tells the navigation on FBlock 0x52 instance 0, function 0xCFF (with the factory navigation unlicensed, many
// times a second, unchanged). A new content of that message from the cluster's own address has both picture sizes read
// again, and the arrow is drawn anew at the new size.
//
// It reads the phone's navigation messages itself, as the Ethernet arrows do (kombi_nav.hpp), and shows the same next
// maneuver. Never where the Ethernet KOMBI is: not once it took an arrow, not while its address is in the neighbour
// table, and the MOST registry is asked only after the guidance wanted a picture for ethernetFirstS (the Ethernet
// KOMBI answers within a quarter of a second). Not while the setting (kombi-off) is off, nor while the BMW navigation
// guides (its D-Bus state, or another client drawing on the fields): the fields are left to it, uncleared. A marker
// file says a picture may be up; the next start clears it.
//
// Threads: onNavStatus() and onSessionEnded() on the session's, setBmwGuiding() and setEthernetSeen() on the Ethernet
// arrows', everything else on the controller reader's thread, whose vmostd connection this rides on (evo_knob.hpp).
#ifndef ENTRY_AAD_PLATFORM_KOMBI_MOST_HPP
#define ENTRY_AAD_PLATFORM_KOMBI_MOST_HPP

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <deque>
#include <string>
#include <vector>

#include "kombi_guide.hpp"
#include "kombi_phone.hpp"
#include "vmost_client.hpp"

namespace evo {
namespace kombi {

const uint8_t kHudFblock = 0x82, kHudInstance = 0x00;
const uint8_t kRegistryFblock = 0x02, kRegistryInstance = 0x00;
const uint16_t kRegistryAddress = 0x0100;  // the NetworkMaster
const uint16_t kRegistryFkt = 0xA01;       // CentralRegistry
const uint8_t kGuidanceFblock = 0x52, kGuidanceInstance = 0x00;
const uint16_t kGuidanceMovedFkt = 0xCFF;  // the cluster tells the navigation the guidance moved (cluster or HUD)
enum HudFunction : uint16_t {
    kHudShow = 0x204,     // [field][0|1]
    kHudSwap = 0x20E,     // [field to hide][field to show]
    kHudText = 0x210,     // [field][00][UTF-16BE][00 00]
    kHudLine = 0x219,     // [field][width]
    kHudColour = 0x21B,   // [field][colour]
    kHudPolygon = 0x21D,  // [field][index][03 filled][count][x y]...
    kHudEmpty = 0x220,    // [field]
    kHudSize = 0xC04,     // [field][01] -> [field][..][width][height]
};
// Answers to a StartResultAck (kMostOpStartResultAck, vmost_client.hpp).
const uint32_t kMostOpError = 0x9, kMostOpResultAck = 0xD, kMostOpErrorAck = 0xF;

// The fields' handles, as on the 6WA cluster.
struct HudLayout {
    uint16_t frame, street, distance, unit, pictureA, pictureB;
    HudLayout();
};
const size_t kHudFields = 6;  // handles 1 to 6
struct FieldSize {
    uint16_t w, h;
};
// The six sizes read (handles 1 to 6) fit what is drawn here: a frame, three text fields, two pictures of one size
// large enough for an arrow. False: why says what did not fit.
bool layoutFits(const FieldSize sizes[kHudFields], std::string* why);

// The address of a FBlock instance in the central registry's answer.
bool registryAddress(const uint8_t* data, size_t len, uint8_t fblock, uint8_t inst, uint16_t* address);
// A size answer: the field, its width and height (the last four bytes).
bool fieldSize(const uint8_t* data, size_t len, uint16_t* field, FieldSize* size);

// One call to the cluster: a function and its parameters.
struct HudCall {
    uint16_t fkt;
    std::vector<uint8_t> data;
    bool operator==(const HudCall& o) const { return fkt == o.fkt && data == o.data; }
};
using HudCalls = std::vector<HudCall>;
HudCall hudShow(uint16_t field, bool on);
HudCall hudSwap(uint16_t hide, uint16_t show);
HudCall hudText(uint16_t field, const std::string& utf8);  // code points past U+FFFF become '?'
HudCall hudEmpty(uint16_t field);
HudCall hudSize(uint16_t field);

// The picture: convex filled polygons, as the stock arrows are drawn. Colours of the 6WA palette.
const uint8_t kHudWhite = 4, kHudGrey = 3;
struct HudPoint {
    int16_t x, y;
};
struct HudShape {
    uint8_t colour;
    std::vector<HudPoint> points;
};
// The arrow for a cluster direction (kombi_display.hpp Direction) in a w x h picture field, within the part left
// clear of the shade. Empty for a direction without a picture.
std::vector<HudShape> hudPicture(int direction, uint16_t w, uint16_t h);
bool convex(const std::vector<HudPoint>& points);
// The calls that draw the shapes into a buffer, from empty.
void pictureCalls(const std::vector<HudShape>& shapes, uint16_t buffer, HudCalls& out);

// What the fields show.
struct HudView {
    int direction;  // 0: no picture
    std::string distance, unit, street;
    HudView() : direction(0) {}
    bool operator==(const HudView& o) const {
        return direction == o.direction && distance == o.distance && unit == o.unit && street == o.street;
    }
    bool operator!=(const HudView& o) const { return !(*this == o); }
};
// The calls from what is shown (null: nothing) to view. A new picture goes into the hidden buffer, then the buffers
// swap and visible changes; otherwise only the texts that changed.
void viewCalls(const HudView& view, const HudView* shown, const HudLayout& layout, FieldSize picture, uint16_t& visible,
               HudCalls& out);
void clearCalls(const HudLayout& layout, HudCalls& out);

// The phone's next maneuver as the fields show it, with the Ethernet guide's settings: only within approachMiles
// (hysteresis once shown), a first maneuver waits up to firstDistanceS for its distance, the view stays graceS after
// the guidance stops, the distance text changes at most every distanceEveryS. Single-threaded.
class HudPresenter {
public:
    explicit HudPresenter(const GuideSettings& settings);
    // True and the view to show; false: nothing.
    bool step(const Maneuver* next, double now, HudView& view);
    void reset();

private:
    const GuideSettings settings_;
    bool showing_;
    HudView view_;
    Maneuver turn_;  // the maneuver shown
    bool gapOn_, noDistanceOn_;
    double gapAt_, noDistanceAt_, distanceAt_;
};

// True when /proc/net/arp (its text) holds a complete entry for ip: the Ethernet KOMBI answers on this car.
bool arpHasNeighbour(const std::string& table, const std::string& ip);
bool ethernetNeighbour(const std::string& ip);

struct MostHudConfig {
    std::string markerFile;      // present: a picture may be up
    std::string offFile;         // present: the arrows are off (the iDrive app's menu), for both clusters
    std::string clusterAddress;  // the Ethernet KOMBI, looked for in the neighbour table
    double ethernetFirstS;       // the guidance wants a picture this long before the MOST registry is asked
    GuideSettings guide;
    HudLayout layout;
    MostHudConfig();
};

class MostHud : public VmostPeer {
public:
    enum Phase { kIdle, kLookup, kSizes, kReady, kAbsent, kStopped };

    explicit MostHud(const MostHudConfig& config = MostHudConfig());
    ~MostHud();

    // The session's nav-status callback, as Arrows::onNavStatus: keeps the message and returns.
    static void onNavStatus(uint16_t id, const uint8_t* body, size_t size, void* hud);
    void onSessionEnded();
    // From the Ethernet arrows: the BMW navigation guides; the Ethernet KOMBI took an arrow (for good).
    void setBmwGuiding(bool guiding) { bmw_ = guiding; }
    void setEthernetSeen() { ethernet_ = true; }

    // VmostPeer
    bool wants(const VmostMsgView& m) const override;
    void onMessage(const VmostMsgView& m) override;
    int dueInMs(uint32_t nowMs) const override;
    void tick(uint32_t nowMs, VmostPort& port) override;
    void finish(VmostPort& port) override;

    Phase phase() const { return phase_; }
    bool showing() const { return shown_; }

private:
    enum Batch { kNoBatch, kSizesBatch, kRemeasureBatch, kDrawBatch, kClearBatch };
    struct Waiting {  // a drawing call sent, its answer not in
        HudCall call;
        uint32_t at;
        unsigned serial;
    };
    struct Sent {
        HudCall call;
        uint32_t at;
        unsigned serial;
        bool answered;
        uint32_t answeredAt;
    };

    void readInputs(uint32_t now);
    void startLookup(uint32_t now, VmostPort& port);
    void onRegistry(uint32_t now);
    void askSizes(bool missingOnly);
    void checkSizes(uint32_t now);
    void onSizes(uint32_t now);
    bool sizesComplete() const;
    void onGuidanceMoved(const VmostMsgView& m);
    void decide(uint32_t now);
    void queue(Batch batch, const HudCalls& calls);
    void sendNext(uint32_t now, VmostPort& port);
    void endBatch();
    void stop(const char* why);
    void mark(bool up);
    void unshadow(VmostPort& port);
    bool ours(const VmostMsgView& m, uint32_t now) const;
    void onAnswer(const VmostMsgView& m, uint32_t now);
    void expire(uint32_t now);
    size_t room() const;
    static bool takeAnswer(std::deque<Waiting>& list, const VmostMsgView& m, Waiting& out);

    const MostHudConfig config_;

    // From the session's thread, under mu_
    pthread_mutex_t mu_;
    NavInbox inbox_;
    unsigned messages_, generation_;
    // From the Ethernet arrows
    std::atomic<bool> bmw_, ethernet_;

    // The controller reader's thread
    uint32_t clock_;  // the last tick's time, for the messages that arrive between ticks
    bool started_;
    uint32_t startedAt_, inputsAt_, settingAt_, arpAt_, messageAt_, wantSince_;
    unsigned seenMessages_, seenGeneration_;
    bool settingOn_, arpSeen_, ethernetLogged_, allowed_, bmwNow_, has_, wantOn_;
    HudView view_;
    HudPresenter presenter_;
    Phase phase_;
    bool shadowed_;
    uint16_t address_;
    int lookupTries_;
    uint32_t lookupAt_, retryAt_;
    bool registryIn_;
    std::vector<uint8_t> registry_;
    FieldSize sizes_[kHudFields];
    bool sizeIn_[kHudFields];
    uint32_t sizeAskedAt_[kHudFields];
    bool sizesWaiting_;  // the size reads went out: answers until sizesAskedAt_ + 10 s
    uint32_t sizesAskedAt_;
    int sizeTries_;
    bool remeasureDue_, remeasuring_, haveMovedTo_;
    uint32_t remeasureAt_;
    std::vector<uint8_t> movedTo_;  // the last place the cluster named for the guidance
    unsigned remeasures_, lateSizes_;
    FieldSize drawnPicture_, pendingPicture_;  // the picture size the arrow on the cluster was drawn for
    std::deque<HudCall> queue_;
    Batch batch_;
    bool batchFailed_;
    HudView pending_;
    std::deque<Waiting> waiting_;  // oldest first
    std::deque<Waiting> givenUp_;  // no answer within 10 s: one that still comes is theirs
    unsigned serial_;
    bool refused_;  // a drawing call was refused since the last tick
    unsigned refusals_, slowAnswers_, lateAnswers_;
    uint32_t lastSendAt_;
    int missed_;
    std::deque<Sent> recent_;  // our last calls: their copies on the shadow are not another client
    uint32_t foreignUntil_;
    bool foreignLogged_;
    bool shown_, unsure_, marked_, staleMarker_, firstAcked_;
    HudView shownView_;
    uint16_t visible_;
    unsigned drawn_;
};

}  // namespace kombi
}  // namespace evo

#endif
