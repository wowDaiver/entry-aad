// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Exercise the actual supervisor recovery decisions with simulated Alps/ILM boundaries.
// The real /proc reader and D-Bus encodings have separate process/integration tests.
#ifndef ENTRY_AAD_SUPERVISOR_FAKES_HPP
#define ENTRY_AAD_SUPERVISOR_FAKES_HPP
#define main supervisor_entry_unused
#include "../tools/aa_supervisor.cpp"
#undef main
#include <assert.h>

#include "../platform/spp_advertisement.hpp"

namespace {
evo::SppSlotSnapshot simulated;
unsigned configuration = 0, uuidWrites = 0, configWrites = 0, disconnects = 0;
unsigned transientReads = 0, readerOpens = 0, readerCloses = 0;
bool failUuid = false, failConfig = false, videoGone = true, liveLink = false, failRead = false;
bool flipKnown = true;
}  // namespace
namespace evo {
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
    assert(instance == 0);
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
    assert(instance == 0);
    return true;
}
void AlpsSppLink::setMonitorOnly(bool) {}
void AlpsSppLink::closeLink() {}
bool AlpsSppLink::linkUp() {
    return liveLink;
}
int AlpsSppLink::registerUuid(const char* uuid, int timeout) {
    uint8_t parsed[16];
    assert(parseUuid128(uuid, parsed));
    return registerUuidPayload(buildSetUuidPayload(parsed, parsed), timeout);
}
bool AlpsSppLink::readConfiguration(uint8_t instance, uint16_t id, std::vector<uint8_t>& out, int) {
    assert(instance == 0 && id == 0x6203);
    out.assign(1, configuration);
    return true;
}
bool AlpsSppLink::setDataInterface(uint8_t value, int) {
    ++configWrites;
    if (failConfig) {
        return false;
    }
    configuration = value;
    return true;
}
int AlpsSppLink::registerUuidPayload(const std::vector<uint8_t>& payload, int) {
    ++uuidWrites;
    if (failUuid) {
        return -1;
    }
    simulated.uuidPayload = simulated.transportUuidPayload = payload;
    return 0;
}
bool AlpsSppLink::requestSppDisconnect() {
    ++disconnects;
    simulated.state = 1;
    simulated.transportState = 0;
    simulated.connected = false;
    return true;
}
EvoIlm::EvoIlm() {}
bool EvoIlm::validateTarget(uint32_t, uint32_t, uint32_t, uint32_t, bool) {
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

#endif
