// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Registers the Android Auto SPP UUID again while the slot is idle, on a bounded schedule.
#ifndef ENTRY_AAD_PLATFORM_SPP_ADVERTISEMENT_HPP
#define ENTRY_AAD_PLATFORM_SPP_ADVERTISEMENT_HPP
#include "alps_spp.hpp"
#include "evo_log.hpp"
#include "spp_baseline.hpp"
namespace evo {
class AdvertisementSchedule {
public:
    // repeatMs (service mode): after the three early refreshes, one every repeatMs while idle.
    explicit AdvertisementSchedule(uint32_t repeatMs = 0) : next_(0), repeat_(repeatMs), last_(0) {}
    bool due(uint32_t elapsedMs) {
        const uint32_t times[] = {15000, 45000, 90000};
        if (next_ >= 3) {
            if (!repeat_ || elapsedMs - last_ < repeat_) {
                return false;
            }
            last_ = elapsedMs;
            return true;
        }
        if (elapsedMs < times[next_]) {
            return false;
        }
        ++next_;
        last_ = elapsedMs;
        return true;
    }

private:
    unsigned next_;
    uint32_t repeat_, last_;
};
inline std::vector<uint8_t> androidAutoPair() {
    uint8_t uuid[16];
    parseUuid128(kAaUuid, uuid);
    return buildSetUuidPayload(uuid, uuid);
}
// No re-registration if a link arrived meanwhile, the slot is busy, or ownership is unclear.
// The exact current AA pair is registered again; the BMW baseline is never used for a refresh. slot: the one bt was
// started on.
inline bool refreshAdvertisement(AlpsSppLink& bt, SppSnapshotReader& reader, unsigned slot) {
    if (bt.linkUp()) {
        return true;
    }
    SppSlotSnapshot current;
    if (!reader.read(slot, current)) {
        ELOG("Advertisement", "refresh skipped: Alps state unreadable or changing");
        return true;
    }
    if (current.uuidPayload != androidAutoPair() || current.transportUuidPayload != current.uuidPayload) {
        ELOG("Advertisement", "Android Auto UUID pair changed: stopping, no refresh");
        return false;
    }
    if (!current.idle() || bt.linkUp()) {
        ELOG("Advertisement", "refresh skipped: SPP busy");
        return true;
    }
    int result = bt.registerUuid(kAaUuid, 3000);
    if (result != 0) {
        if (bt.linkUp()) {
            return true;  // connection won the race, do not disturb it
        }
        ELOG("Advertisement", "UUID refresh not confirmed (%d): stopping so that Bluetooth is restored", result);
        return false;
    }
    ELOG("Advertisement", "Android Auto advertised again on the idle SPP %u; waiting for the phone", slot);
    return true;
}
}  // namespace evo
#endif
