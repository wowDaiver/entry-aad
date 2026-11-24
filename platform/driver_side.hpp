// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The driver's side as the car is coded: the phone lays Android Auto's screen out for it (core/sdr_builder.hpp).
//
// iDrive's coding feature TYP_LENKUNG (libhmicoding.so, its table of coding features, like HMI_BRAND in
// car_brand.hpp): va_TYP_LENKUNG_linkslenker 0x954, va_TYP_LENKUNG_rechtslenker 0x955. The iDrive app asks its variant
// server for both (hmiapp/idrive_coding.cpp) and writes contract::kDriverSideFile when exactly one is set; the
// service reads it at start. The vehicle file's "side=" still wins; without either, left-hand drive. Pure: no I/O.
// tests/test_evo_sdr.cpp.
#ifndef ENTRY_AAD_PLATFORM_DRIVER_SIDE_HPP
#define ENTRY_AAD_PLATFORM_DRIVER_SIDE_HPP

#include <string.h>

namespace aa {

const unsigned kVariantLeftHandDrive = 0x954;   // va_TYP_LENKUNG_linkslenker
const unsigned kVariantRightHandDrive = 0x955;  // va_TYP_LENKUNG_rechtslenker

// The side as the file holds it.
inline const char* driverSideName(bool right) {
    return right ? "right" : "left";
}

// The file's line: "left" or "right", an end of line allowed; false otherwise (right unchanged).
inline bool parseDriverSide(const char* text, bool& right) {
    if (!text) {
        return false;
    }
    const size_t len = strcspn(text, "\r\n");
    if (len == 5 && !strncmp(text, "right", 5)) {
        right = true;
        return true;
    }
    if (len == 4 && !strncmp(text, "left", 4)) {
        right = false;
        return true;
    }
    return false;
}

}  // namespace aa

#endif  // ENTRY_AAD_PLATFORM_DRIVER_SIDE_HPP
