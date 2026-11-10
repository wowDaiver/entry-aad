// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Exercise the actual supervisor recovery decisions with simulated Alps/ILM boundaries.
#include "supervisor_fakes.hpp"

static void recoveryCases(const std::vector<uint8_t>& original) {
    uuidWrites = configWrites = disconnects = configuration = 0;
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    char dir[] = "/tmp/entry-aa-recovery-test-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    Baseline b;
    b.pid = 123;
    b.identity = "456";
    b.instance = 0;
    b.interface = 0;
    b.uuid = original;
    evo::SppSnapshotReader reader;
    simulated.state = 1;
    simulated.uuidPayload = simulated.transportUuidPayload = b.uuid;
    Baseline checked;
    assert(preflight(reader, checked) && checked.uuid == b.uuid && checked.interface == 0);
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
    assert(load(loaded) && loaded.uuid == b.uuid);
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
static void advertisementCases() {
    evo::AdvertisementSchedule schedule;
    assert(!schedule.due(0) && !schedule.due(14999) && schedule.due(15000));
    assert(!schedule.due(15001) && !schedule.due(44999) && schedule.due(45000));
    assert(!schedule.due(89999) && schedule.due(90000) && !schedule.due(180000));
    evo::SppSnapshotReader reader;
    evo::AlpsSppLink bt;
    simulated = evo::SppSlotSnapshot();
    simulated.state = 1;
    simulated.uuidPayload = simulated.transportUuidPayload = evo::androidAutoPair();
    unsigned before = uuidWrites;
    assert(evo::refreshAdvertisement(bt, reader) && uuidWrites == before + 1);
    assert(simulated.uuidPayload == aaPayload() && simulated.transportUuidPayload == aaPayload());
    before = uuidWrites;
    liveLink = true;
    assert(evo::refreshAdvertisement(bt, reader));
    liveLink = false;
    simulated.connected = true;
    assert(evo::refreshAdvertisement(bt, reader));
    simulated.connected = false;
    simulated.transportState = 1;
    assert(evo::refreshAdvertisement(bt, reader));
    simulated.transportState = 0;
    failRead = true;
    assert(evo::refreshAdvertisement(bt, reader));
    failRead = false;
    simulated.uuidPayload[1] ^= 1;
    simulated.transportUuidPayload = simulated.uuidPayload;
    assert(!evo::refreshAdvertisement(bt, reader) && uuidWrites == before);
    simulated.uuidPayload = simulated.transportUuidPayload = aaPayload();
    failUuid = true;
    assert(!evo::refreshAdvertisement(bt, reader));
    failUuid = false;
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
    advertisementCases();
    puts(
        "PASS supervisor: actual BMW and generic preflight, exact child snapshot, saved pairs reloaded/restored, "
        "partial failures and busy/third-party refusals");
}
