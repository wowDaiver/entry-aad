// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Which original BMW SPP UUID pairs the supervisor accepts to save and later restore exactly.
#ifndef ENTRY_AAD_PLATFORM_SPP_BASELINE_HPP
#define ENTRY_AAD_PLATFORM_SPP_BASELINE_HPP
#include "evo_spp_snapshot.hpp"
namespace evo {
inline std::string uuidPairHex(const std::vector<uint8_t>& bytes) {
    const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}
inline bool baselineSupported(const std::vector<uint8_t>& payload) {
    const std::string encoded = uuidPairHex(payload);
    // Generic SPP, or the distinct initiator/record UUIDs observed on the car (both copies in agreement).
    // Never substitute either constant for the actual snapshot when saving/restoring.
    return encoded == "011101011101" ||
           encoded == "0300000000decafadedecadeafdecacaff0300000000decafadedecadeafdecacafe";
}
inline bool snapshotMatchesBaseline(const SppSlotSnapshot& slot, const std::string& savedPair) {
    return slot.idle() && slot.uuidPayload == slot.transportUuidPayload && baselineSupported(slot.uuidPayload) &&
           uuidPairHex(slot.uuidPayload) == savedPair;
}
// The Alps stack creates six SPP slots, 0 to 5 (libevo_btappl.so spp_instance_init), each with the generic pair, and
// BMW sets up two of them (stack/etc/evo_config.xml): 0 holds the BMW Apps pair that the My BMW app connects to
// (vmostAuxInBT), 1 is CarPlay's. Android Auto takes 5: the BMW Apps service keeps its slot, and a slot the stack
// picks for itself (cmGetIdleInsForSppHfp) is searched from 0 upwards, skipping slots that carry another UUID.
// Slot 0, as before, only on a car whose slot 5 is not started.
const unsigned kAaSlot = 5, kAaFallbackSlot = 0;
inline bool aaSlotAllowed(unsigned slot) {
    return slot == kAaSlot || slot == kAaFallbackSlot;
}
inline bool genericPair(const std::vector<uint8_t>& payload) {
    return uuidPairHex(payload) == "011101011101";
}
// What slot 5 offers, read before anything is changed. kSlotFree: started and idle like slot 0 is, still the generic
// pair. kSlotBusy: connected, or already carrying the Android Auto pair: this start is refused and tried again later,
// never moved to slot 0, which would advertise Android Auto twice. kSlotAbsent: unreadable, not started on this car,
// in another state, or holding another UUID: Android Auto stays on slot 0, as before.
enum SlotOffer { kSlotFree, kSlotBusy, kSlotAbsent };
inline SlotOffer spareSlotOffer(bool readable, const SppSlotSnapshot& slot, const std::vector<uint8_t>& aaPair) {
    if (!readable || slot.state == 0) {
        return kSlotAbsent;
    }
    if (slot.uuidPayload == aaPair || slot.transportUuidPayload == aaPair || slot.connected || slot.state == 2) {
        return kSlotBusy;
    }
    if (!slot.idle() || slot.uuidPayload != slot.transportUuidPayload || !genericPair(slot.uuidPayload)) {
        return kSlotAbsent;
    }
    return kSlotFree;
}
}  // namespace evo
#endif
