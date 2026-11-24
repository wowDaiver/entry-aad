// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Exercise the actual supervisor recovery decisions with simulated Alps/ILM boundaries.
// The real /proc reader and D-Bus encodings have separate process/integration tests.
#define main supervisor_entry_unused
#include "../tools/aa_supervisor.cpp"
#undef main
#include <assert.h>

#include "../platform/spp_advertisement.hpp"

namespace {
// simulated: the slot under test (testSlot), the one Android Auto takes; spare: the other allowed slot (0 or 5).
evo::SppSlotSnapshot simulated, spare;
unsigned testSlot = 0, started = 0, spareConfiguration = 0;
bool spareReadable = false;
unsigned configuration = 0, uuidWrites = 0, configWrites = 0, disconnects = 0;
unsigned transientReads = 0, readerOpens = 0, readerCloses = 0;
bool failUuid = false, failConfig = false, videoGone = true, liveLink = false, failRead = false;
bool flipKnown = true;
// The phone of a lost session, as on the car: while our Android Auto UUID is on the slot, it reconnects kPhoneBackMs
// after a disconnection, phoneReturns more times. lateWrites: results of the next UUID writes, which reach the stack
// only once the phone is back (-1: our link saw it first and sent nothing; 7: the stack refused, slot connected).
// splitWrites: next writes answered 4 with only the transport copy changed, as when the stack cannot close its server.
const uint32_t kPhoneBackMs = 70;
unsigned phoneReturns = 0, splitWrites = 0;
bool phoneComing = false;
uint32_t droppedAt = 0;
std::vector<int> lateWrites;
void phoneStep() {
    if (!phoneComing || evo::monoMs() - droppedAt < kPhoneBackMs) {
        return;
    }
    phoneComing = false;
    if (simulated.uuidPayload != aaPayload()) {
        return;  // no Android Auto record left to come back to
    }
    simulated.state = 2;
    simulated.transportState = 1;
    simulated.connected = true;
    liveLink = true;  // its connection opens our link as well
}
}  // namespace
namespace evo {
int updateFromUsb(bool, const char*) {  // tests/test_update_install.cpp
    return 0;
}
unsigned hotspotRequests = 0;
bool requestWlanAccessPoint(unsigned, WlanRequestResult*) {
    ++hotspotRequests;
    return true;
}
SppSnapshotReader::SppSnapshotReader() : fd_(-1), pid_(123), base_(0), identity_("456") {}
SppSnapshotReader::~SppSnapshotReader() {}
bool SppSnapshotReader::open() {
    ++readerOpens;
    return true;
}
void SppSnapshotReader::close() {
    ++readerCloses;
}
bool SppSnapshotReader::read(unsigned instance, SppSlotSnapshot& out, bool allowSplit) {
    assert(instance == 0 || instance == 5);
    if (instance != testSlot) {
        out = spare;
        return spareReadable;
    }
    phoneStep();
    out = simulated;
    if (transientReads) {
        --transientReads;
        return false;
    }
    return !failRead && (allowSplit || out.uuidPayload == out.transportUuidPayload);
}
AlpsSppLink::AlpsSppLink() : p_(0) {}
AlpsSppLink::~AlpsSppLink() {}
bool AlpsSppLink::start(uint8_t instance) {
    assert(instance == 0 || instance == 5);
    started = instance;
    return true;
}
void AlpsSppLink::setMonitorOnly(bool) {}
void AlpsSppLink::closeLink() {
    liveLink = false;
}
bool AlpsSppLink::linkUp() {
    return liveLink;
}
int AlpsSppLink::registerUuid(const char* uuid, int timeout) {
    uint8_t parsed[16];
    assert(parseUuid128(uuid, parsed));
    return registerUuidPayload(buildSetUuidPayload(parsed, parsed), timeout);
}
// As the car's stack does: a read of slots 2 to 5 answers slot 0's value (slotConfigurationReadable).
bool AlpsSppLink::readConfiguration(uint8_t instance, uint16_t id, std::vector<uint8_t>& out, int) {
    assert((instance == 0 || instance == 5) && id == 0x6203);
    const unsigned answered = slotConfigurationReadable(instance) ? instance : 0;
    out.assign(1, answered == testSlot ? configuration : spareConfiguration);
    return true;
}
// Every write goes to the slot Android Auto took, never to the other one.
bool AlpsSppLink::setDataInterface(uint8_t value, int) {
    assert(started == testSlot);
    ++configWrites;
    if (failConfig) {
        return false;
    }
    configuration = value;
    return true;
}
int AlpsSppLink::registerUuidPayload(const std::vector<uint8_t>& payload, int) {
    assert(started == testSlot);
    if (!lateWrites.empty()) {
        const int result = lateWrites.front();
        lateWrites.erase(lateWrites.begin());
        while (phoneComing && evo::monoMs() - droppedAt < kPhoneBackMs) {
            usleep(1000);
        }
        phoneStep();
        assert(simulated.state == 2 && (result == -1 || result == 7));
        if (result == 7) {
            ++uuidWrites;  // sent, and refused by the stack
        }
        return result;
    }
    if (liveLink) {
        return -1;  // as the real link: no UUID while a link is up, nothing sent
    }
    ++uuidWrites;
    if (failUuid) {
        return -1;
    }
    if (simulated.state == 2) {
        return 7;  // as the stack: a connected slot keeps its UUID
    }
    if (splitWrites) {
        --splitWrites;
        simulated.transportUuidPayload = payload;
        return 4;
    }
    simulated.uuidPayload = simulated.transportUuidPayload = payload;
    return 0;
}
bool AlpsSppLink::requestSppDisconnect() {
    assert(started == testSlot);
    ++disconnects;
    simulated.state = 1;
    simulated.transportState = 0;
    simulated.connected = false;
    if (phoneReturns) {
        --phoneReturns;
        phoneComing = true;
        droppedAt = evo::monoMs();
    }
    return true;
}
EvoIlm::EvoIlm() {}
bool EvoIlm::validateTarget(uint32_t, uint32_t, uint32_t, uint32_t, bool) {
    return true;
}
bool EvoIlm::screenSize(uint32_t& w, uint32_t& h) {
    w = 1280;
    h = 480;
    return true;
}
bool EvoIlm::renderOrderTopFirst(bool& topFirst) {
    topFirst = true;
    return flipKnown;
}
bool EvoIlm::surfaceAbsent(uint32_t id) {
    assert(id == 0x7eaa0001);
    return videoGone;
}
bool EvoIlm::detachSurface(uint32_t id, uint32_t target) {
    assert(id == 0x7eaa0001 && target == 0x80000000);
    return true;
}
bool apAvailable = true;
bool loadAccessPoint(aa::ApInfo&, std::string&) {
    return apAvailable;
}
}  // namespace evo
// The whole save / restore cycle on slot 0, on a car where slot 5 is unreadable (not there).
static void recoveryCases(const std::vector<uint8_t>& original) {
    uuidWrites = configWrites = disconnects = configuration = 0;
    const unsigned slot = 0;
    testSlot = slot;
    spare = evo::SppSlotSnapshot();
    spareReadable = false;
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    char dir[] = "/tmp/entry-aa-recovery-test-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    Baseline b;
    b.pid = 123;
    b.identity = "456";
    b.instance = slot;
    b.interface = 0;
    b.uuid = original;
    evo::SppSnapshotReader reader;
    simulated = evo::SppSlotSnapshot();
    simulated.state = 1;
    simulated.uuidPayload = simulated.transportUuidPayload = b.uuid;
    Baseline checked;
    assert(preflight(reader, checked) && checked.uuid == b.uuid && checked.interface == 0 && checked.instance == slot);
    // Every preflight attempt rescans Alps (close then open), so a retried preflight can pass.
    {
        unsigned o = readerOpens, c = readerCloses;
        assert(preflight(reader, checked));
        assert(readerOpens == o + 1 && readerCloses == c + 1);
    }
    assert(uuidWrites == 0 && configWrites == 0 && disconnects == 0);
    // Hotspot off -> one request to wland (only with --service / g_wlanAuto), not repeated within 60 s.
    evo::apAvailable = false;
    const unsigned h0 = evo::hotspotRequests;                           // recoveryCases runs once per baseline
    assert(!preflight(reader, checked) && evo::hotspotRequests == h0);  // without --service: never
    g_wlanAuto = true;
    assert(!preflight(reader, checked) && evo::hotspotRequests <= h0 + 1);  // at most once per 60 s ...
    assert(!preflight(reader, checked) && evo::hotspotRequests <= h0 + 1);
    assert(evo::hotspotRequests >= 1 && evo::hotspotRequests <= 2);    // ... and twice per run in total
    assert(uuidWrites == 0 && configWrites == 0 && disconnects == 0);  // nothing touched meanwhile
    g_wlanAuto = false;
    evo::apAvailable = true;
    flipKnown = false;
    assert(!preflight(reader, checked));
    flipKnown = true;  // unknown stacking: refuse first
    simulated.connected = true;
    assert(!preflight(reader, checked));
    simulated.connected = false;
    simulated.uuidPayload[1] = 0x22;
    simulated.transportUuidPayload = simulated.uuidPayload;
    assert(!preflight(reader, checked));
    simulated.uuidPayload = simulated.transportUuidPayload = b.uuid;
    assert(save(b));
    Baseline loaded;
    assert(load(loaded) && loaded.uuid == b.uuid && loaded.instance == slot);
    transientReads = 2;  // disconnect can leave the two snapshots in transition briefly
    assert(recover(b, reader));
    assert(uuidWrites == 0 && configWrites == 0 && disconnects == 0);

    // A persistent read failure must time out with no mutation and keep the original journal.
    assert(save(b));
    failRead = true;
    uint32_t begin = evo::monoMs();
    assert(!recover(b, reader) && evo::monoMs() - begin >= 3000 && evo::monoMs() - begin < 5000);
    assert(uuidWrites == 0 && configWrites == 0 && disconnects == 0 && access(journal, F_OK) == 0);
    failRead = false;
    assert(recover(b, reader));

    // Successful AA must disconnect only its selected SPP, restore both originals, then remove journal.
    assert(save(b));
    simulated.uuidPayload = simulated.transportUuidPayload = aaPayload();
    simulated.connected = true;
    simulated.state = 2;
    simulated.transportState = 1;
    configuration = 1;
    assert(recover(b, reader));
    assert(uuidWrites == 1 && configWrites == 1 && disconnects == 1);
    assert(access(journal, F_OK) != 0);

    // Partial SetUuid writes the transport copy before the app copy: recover exact originals while idle.
    assert(save(b));
    simulated.transportUuidPayload = aaPayload();
    configuration = 1;
    assert(recover(b, reader));
    assert(uuidWrites == 2 && disconnects == 1 && configuration == 0);

    // Never disconnect an unconfirmed, still-live registration (could be BMW).
    assert(save(b));
    simulated.transportUuidPayload = aaPayload();
    simulated.connected = true;
    assert(!recover(b, reader));
    assert(uuidWrites == 2 && disconnects == 1 && access(journal, F_OK) == 0);
    simulated.connected = false;
    assert(recover(b, reader));

    // Third-party UUID and process replacement must preserve the journal and cause zero writes.
    assert(save(b));
    simulated.uuidPayload[1] = 0x22;
    unsigned before = uuidWrites;
    assert(!recover(b, reader) && uuidWrites == before);
    simulated.uuidPayload = simulated.transportUuidPayload = b.uuid;
    b.identity = "457";
    assert(!recover(b, reader) && uuidWrites == before);
    b.identity = "456";
    assert(recover(b, reader));

    // A missing acknowledgement, failed config restoration or remaining surface is never declared success.
    assert(save(b));
    simulated.uuidPayload = simulated.transportUuidPayload = aaPayload();
    failUuid = true;
    assert(!recover(b, reader) && access(journal, F_OK) == 0);
    failUuid = false;
    configuration = 1;
    failConfig = true;
    assert(!recover(b, reader) && access(journal, F_OK) == 0);
    failConfig = false;
    videoGone = false;
    assert(!recover(b, reader) && access(journal, F_OK) == 0);
    videoGone = true;
    assert(recover(b, reader));
    // A BMW connection appearing on the original UUID must never have its interface changed live.
    assert(save(b));
    simulated.connected = true;
    configuration = 1;
    unsigned configsBefore = configWrites, disconnectsBefore = disconnects;
    assert(!recover(b, reader) && configWrites == configsBefore && disconnects == disconnectsBefore);
    assert(access(journal, F_OK) == 0);
    simulated.connected = false;
    assert(recover(b, reader));
    assert(simulated.uuidPayload == original && simulated.transportUuidPayload == original);
    assert(chdir(cwd) == 0 && rmdir(dir) == 0);
}
// The whole cycle on slot 5, with slot 0 holding BMW Apps and My BMW connected to it. A read of slot 5's interface
// would answer slot 0's: it is saved as its start value 0 and written back at every restore, never read.
static void slot5Cases(const std::vector<uint8_t>& bmw, const std::vector<uint8_t>& generic) {
    uuidWrites = configWrites = disconnects = 0;
    testSlot = 5;
    spare = evo::SppSlotSnapshot();
    spare.state = 2;
    spare.transportState = 1;
    spare.connected = true;
    spare.uuidPayload = spare.transportUuidPayload = bmw;
    spareReadable = true;
    spareConfiguration = 0;  // slot 0's interface: what a read of slot 5 answers
    configuration = 1;       // slot 5's own, left at 1 by an earlier run
    simulated = evo::SppSlotSnapshot();
    simulated.state = 1;
    simulated.uuidPayload = simulated.transportUuidPayload = generic;
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    char dir[] = "/tmp/entry-aa-slot5-test-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    evo::SppSnapshotReader reader;
    Baseline b;
    assert(preflight(reader, b) && b.instance == 5 && b.interface == 0 && b.uuid == generic);
    assert(uuidWrites + configWrites + disconnects == 0);
    // Android Auto registered and connected on slot 5: our link closed, the generic pair and the interface back.
    assert(save(b));
    simulated.uuidPayload = simulated.transportUuidPayload = aaPayload();
    simulated.state = 2;
    simulated.transportState = 1;
    simulated.connected = true;
    assert(recover(b, reader));
    assert(disconnects == 1 && uuidWrites == 1 && configWrites == 1 && configuration == 0);
    assert(simulated.uuidPayload == generic && access(journal, F_OK) != 0);
    // A start that stopped before registering: the interface is written back all the same.
    assert(save(b));
    configuration = 1;
    assert(recover(b, reader) && configWrites == 2 && configuration == 0 && uuidWrites == 1);
    // An unconfirmed write keeps the journal.
    assert(save(b));
    failConfig = true;
    assert(!recover(b, reader) && access(journal, F_OK) == 0);
    failConfig = false;
    // Someone else connected on the generic pair: postponed, nothing written.
    simulated.state = 2;
    simulated.connected = true;
    const unsigned before = configWrites;
    assert(!recover(b, reader) && configWrites == before && access(journal, F_OK) == 0);
    simulated.state = 1;
    simulated.transportState = 0;
    simulated.connected = false;
    assert(recover(b, reader) && access(journal, F_OK) != 0);
    assert(spare.uuidPayload == bmw && spare.connected && spareConfiguration == 0);  // slot 0 never touched
    spareReadable = false;
    assert(chdir(cwd) == 0 && rmdir(dir) == 0);
}
// Our UUID registered, the phone connected, a journal saved: what a stop (or the next start) finds.
static void connectedWithAa(const Baseline& saved) {
    assert(save(saved));
    simulated = evo::SppSlotSnapshot();
    simulated.uuidPayload = simulated.transportUuidPayload = aaPayload();
    simulated.state = 2;
    simulated.transportState = 1;
    simulated.connected = true;
    liveLink = false;
    configuration = 1;  // as the service leaves it
}
// Slot 5 given back while the phone of a lost session keeps looking for the car: a phone that comes back within a few
// tens of milliseconds of each disconnection is never seen idle by a 100 ms check, and every later start is refused.
static void phoneBackCases(const std::vector<uint8_t>& generic) {
    uuidWrites = configWrites = disconnects = 0;
    testSlot = 5;
    spare = evo::SppSlotSnapshot();
    spareReadable = false;
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    char dir[] = "/tmp/entry-aa-phone-test-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    evo::SppSnapshotReader reader;
    Baseline b;
    b.pid = 123;
    b.identity = "456";
    b.instance = 5;
    b.interface = 0;
    b.uuid = generic;
    const std::vector<uint8_t> aa = aaPayload();

    // Back 70 ms after each of two disconnections: the slot is idle before that, and the pair goes back at once.
    connectedWithAa(b);
    phoneReturns = 2;
    uint32_t begin = evo::monoMs();
    assert(recover(b, reader) && evo::monoMs() - begin < 1000);
    assert(disconnects == 1 && uuidWrites == 1 && simulated.uuidPayload == generic && !simulated.connected);
    assert(access(journal, F_OK) != 0);
    usleep((kPhoneBackMs + 30) * 1000);
    phoneStep();
    assert(!phoneComing && simulated.idle());  // no Android Auto record left for the phone to come back to
    phoneReturns = 0;

    // Twice back before the write (once seen by our link, once refused by the stack), then it lets go.
    connectedWithAa(b);
    disconnects = uuidWrites = 0;
    phoneReturns = 2;
    lateWrites.push_back(-1);
    lateWrites.push_back(7);
    assert(recover(b, reader));
    assert(disconnects == 3 && uuidWrites == 2 && lateWrites.empty() && simulated.uuidPayload == generic);
    assert(simulated.transportUuidPayload == generic && configuration == 0 && access(journal, F_OK) != 0);

    // A write answered 4 with only the transport copy changed, the slot still idle: written again, no disconnection.
    connectedWithAa(b);
    disconnects = uuidWrites = 0;
    splitWrites = 1;
    assert(recover(b, reader));
    assert(disconnects == 1 && uuidWrites == 2 && splitWrites == 0 && simulated.uuidPayload == generic);

    // A phone that never lets go: bounded rounds and time, the journal kept, nothing claimed.
    connectedWithAa(b);
    disconnects = uuidWrites = 0;
    phoneReturns = 100;
    lateWrites.assign(10, 7);
    begin = evo::monoMs();
    assert(!recover(b, reader) && access(journal, F_OK) == 0);
    assert(disconnects == kRestoreRounds && uuidWrites == kRestoreRounds && simulated.uuidPayload == aa);
    assert(evo::monoMs() - begin < kRestoreLimitMs);
    // Once it lets go, the next start finishes the restore.
    phoneReturns = 0;
    lateWrites.clear();
    phoneComing = false;
    assert(recover(b, reader) && simulated.uuidPayload == generic && access(journal, F_OK) != 0);
    liveLink = false;
    assert(chdir(cwd) == 0 && rmdir(dir) == 0);
}
// Which slot a start takes: 5 while it is free, 0 where 5 is not available, none while 5 is in use.
static void slotCases(const std::vector<uint8_t>& bmw, const std::vector<uint8_t>& generic) {
    const std::vector<uint8_t> aa = aaPayload();
    evo::SppSlotSnapshot s;
    assert(evo::spareSlotOffer(false, s, aa) == evo::kSlotAbsent);  // unreadable
    s.uuidPayload = s.transportUuidPayload = generic;
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotAbsent);  // state 0: not started on this car
    s.state = 1;
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotFree);
    s.transportState = 4;
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotAbsent);  // started otherwise than slot 0 is
    s.transportState = 0;
    s.connected = true;
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotBusy);
    s.connected = false;
    s.state = 2;
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotBusy);
    s.state = 1;
    s.uuidPayload = s.transportUuidPayload = aa;  // ours already: never a second Android Auto record on slot 0
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotBusy);
    s.transportUuidPayload = generic;  // half of a registration
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotBusy);
    s.uuidPayload = s.transportUuidPayload = bmw;  // someone else's UUID
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotAbsent);
    s.uuidPayload = generic;  // copies that disagree
    assert(evo::spareSlotOffer(true, s, aa) == evo::kSlotAbsent);
    assert(evo::aaSlotAllowed(0) && evo::aaSlotAllowed(5) && !evo::aaSlotAllowed(1) && !evo::aaSlotAllowed(3));

    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    char dir[] = "/tmp/entry-aa-slot-test-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    evo::SppSnapshotReader reader;
    Baseline checked;
    const unsigned writes = uuidWrites + configWrites + disconnects;
    // Slot 5 free, slot 0 idle with the BMW Apps pair: 5, and slot 0 is not even needed.
    testSlot = 5;
    simulated = evo::SppSlotSnapshot();
    simulated.state = 1;
    simulated.uuidPayload = simulated.transportUuidPayload = generic;
    configuration = 0;
    spare = evo::SppSlotSnapshot();
    spare.state = 1;
    spare.uuidPayload = spare.transportUuidPayload = bmw;
    spareReadable = true;
    assert(preflight(reader, checked) && checked.instance == 5 && checked.uuid == generic);
    // Slot 5 still holding Android Auto: refused, although slot 0 is idle.
    simulated.uuidPayload = simulated.transportUuidPayload = aa;
    assert(!preflight(reader, checked));
    // Slot 5 not started: slot 0, as before.
    testSlot = 0;
    simulated = spare;
    spare = evo::SppSlotSnapshot();
    spare.uuidPayload = spare.transportUuidPayload = generic;
    assert(preflight(reader, checked) && checked.instance == 0 && checked.uuid == bmw);
    // Slot 5 holding another UUID: left to its owner, slot 0.
    spare.state = 1;
    spare.uuidPayload = spare.transportUuidPayload = bmw;
    assert(preflight(reader, checked) && checked.instance == 0);
    assert(uuidWrites + configWrites + disconnects == writes);  // a preflight only reads
    // The journal takes slot 5 and slot 0, nothing else.
    Baseline b;
    b.pid = 123;
    b.identity = "456";
    b.instance = 5;
    b.interface = 0;
    b.uuid = generic;
    Baseline loaded;
    assert(save(b) && load(loaded) && loaded.instance == 5 && loaded.uuid == generic);
    assert(unlink(journal) == 0);
    const char bad[] = "ENTRY_AA_TRIAL_V1\n123 456 3 0 011101011101\n";
    int fd = open(journal, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, bad, sizeof(bad) - 1) == static_cast<ssize_t>(sizeof(bad) - 1) && close(fd) == 0);
    assert(!load(loaded));
    assert(unlink(journal) == 0);
    spareReadable = false;
    assert(chdir(cwd) == 0 && rmdir(dir) == 0);
}
static void advertisementCases() {
    evo::AdvertisementSchedule schedule;
    assert(!schedule.due(0) && !schedule.due(14999) && schedule.due(15000));
    assert(!schedule.due(15001) && !schedule.due(44999) && schedule.due(45000));
    assert(!schedule.due(89999) && schedule.due(90000) && !schedule.due(180000));
    evo::SppSnapshotReader reader;
    evo::AlpsSppLink bt;
    testSlot = started = evo::kAaSlot;  // the slot the service's link was started on
    simulated = evo::SppSlotSnapshot();
    simulated.state = 1;
    simulated.uuidPayload = simulated.transportUuidPayload = evo::androidAutoPair();
    const unsigned slot = evo::kAaSlot;
    unsigned before = uuidWrites;
    assert(evo::refreshAdvertisement(bt, reader, slot) && uuidWrites == before + 1);
    assert(simulated.uuidPayload == aaPayload() && simulated.transportUuidPayload == aaPayload());
    before = uuidWrites;
    liveLink = true;
    assert(evo::refreshAdvertisement(bt, reader, slot));
    liveLink = false;
    simulated.connected = true;
    assert(evo::refreshAdvertisement(bt, reader, slot));
    simulated.connected = false;
    simulated.transportState = 1;
    assert(evo::refreshAdvertisement(bt, reader, slot));
    simulated.transportState = 0;
    failRead = true;
    assert(evo::refreshAdvertisement(bt, reader, slot));
    failRead = false;
    simulated.uuidPayload[1] ^= 1;
    simulated.transportUuidPayload = simulated.uuidPayload;
    assert(!evo::refreshAdvertisement(bt, reader, slot) && uuidWrites == before);
    simulated.uuidPayload = simulated.transportUuidPayload = aaPayload();
    failUuid = true;
    assert(!evo::refreshAdvertisement(bt, reader, slot));
    failUuid = false;
}
// --compat: a failed Bluetooth point stops wireless only (#106, F20 2017 over USB with an unknown Alps build).
void compatBluetoothCases() {
    std::string detail;
    assert(!strcmp(bluetoothLevel("ok", false, detail), "ok") && detail.empty());
    assert(!strcmp(bluetoothLevel("ok", true, detail), "ok") && detail.empty());
    detail = "unknown Alps structure (sizes 15024/480/36)";
    assert(!strcmp(bluetoothLevel("block", false, detail), "block"));
    assert(detail == "unknown Alps structure (sizes 15024/480/36); wireless only, USB mode does not use Bluetooth");
    detail = "unknown Alps structure (sizes 15024/480/36)";
    assert(!strcmp(bluetoothLevel("block", true, detail), "info"));
    assert(detail == "unknown Alps structure (sizes 15024/480/36); not used over USB");
    detail.clear();
    assert(!strcmp(bluetoothLevel("warn", true, detail), "info") && detail == "not used over USB");
    detail.clear();
    assert(!strcmp(bluetoothLevel("block", false, detail), "block"));
    assert(detail == "wireless only, USB mode does not use Bluetooth");
}
int main() {
    const uint8_t generic[] = {1, 0x11, 1, 1, 0x11, 1};
    const std::vector<uint8_t> genericPair(generic, generic + 6);
    uint8_t init[16], recv[16];
    assert(evo::parseUuid128("00000000-deca-fade-deca-deafdecacaff", init));
    assert(evo::parseUuid128("00000000-deca-fade-deca-deafdecacafe", recv));
    const std::vector<uint8_t> bmw = evo::buildSetUuidPayload(init, recv);
    std::ifstream fixture("tests/fixtures/spp-bmw-uuids.txt");
    std::string observed;
    assert((fixture >> observed) && observed == hex(bmw));
    assert(bmw.size() == 34 && bmw[16] == 0xff && bmw[33] == 0xfe);
    evo::SppSlotSnapshot slot;
    slot.state = 1;
    slot.uuidPayload = slot.transportUuidPayload = bmw;
    assert(evo::snapshotMatchesBaseline(slot, observed));
    assert(!evo::snapshotMatchesBaseline(slot, ""));
    assert(!evo::snapshotMatchesBaseline(slot, hex(genericPair)));
    slot.connected = true;
    assert(!evo::snapshotMatchesBaseline(slot, observed));
    slot.connected = false;
    slot.state = 2;
    assert(!evo::snapshotMatchesBaseline(slot, observed));
    slot.state = 1;
    slot.transportState = 1;
    assert(!evo::snapshotMatchesBaseline(slot, observed));
    slot.transportState = 0;
    slot.transportUuidPayload = genericPair;
    assert(!evo::snapshotMatchesBaseline(slot, observed));
    slot.uuidPayload = genericPair;
    assert(!evo::snapshotMatchesBaseline(slot, observed));
    slot.uuidPayload = slot.transportUuidPayload = aaPayload();
    assert(!evo::snapshotMatchesBaseline(slot, hex(aaPayload())));
    recoveryCases(genericPair);
    recoveryCases(bmw);
    slot5Cases(bmw, genericPair);
    phoneBackCases(genericPair);
    slotCases(bmw, genericPair);
    advertisementCases();
    compatBluetoothCases();
    puts(
        "PASS supervisor: actual BMW and generic preflight, exact child snapshot, saved pairs reloaded/restored, "
        "partial failures and busy/third-party refusals, slot 5 with slot 0 left to BMW Apps, a phone back on the "
        "slot during the restore, Bluetooth points of the check for wireless only");
}
