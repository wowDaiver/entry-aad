// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's brand as iDrive is coded: the maker the phone is told about (Android Auto shows it on its exit icon), and
// the round surround of the MINI's central screen.
//
// iDrive's coding variant HMI_BRAND (libhmicoding.so, its table of coding features): va_HMI_BRAND_bmw 0x1F6,
// va_HMI_BRAND_bmw_i 0x1F7, va_HMI_BRAND_mini 0x1F8, va_HMI_BRAND_rr 0x1F9, va_HMI_BRAND_bmw_m 0xE8D. The iDrive app
// asks its variant server for 0x1F8 (hmiapp/idrive_coding.cpp) and writes contract::kBrandFile; the service reads it at
// start. Without the file (an older app), a BMW. Pure: no I/O. tests/test_evo_sdr.cpp.
#ifndef ENTRY_AAD_PLATFORM_CAR_BRAND_HPP
#define ENTRY_AAD_PLATFORM_CAR_BRAND_HPP

#include <stdint.h>
#include <string.h>

namespace aa {

enum class CarBrand { kBmw, kMini };

const unsigned kVariantBrandMini = 0x1F8;  // va_HMI_BRAND_mini

// The maker as the phone is told it, and as the brand file holds it.
inline const char* brandName(CarBrand brand) {
    return brand == CarBrand::kMini ? "MINI" : "BMW";
}

// The brand file's line: "MINI" or "BMW", an end of line allowed; false otherwise (out unchanged).
inline bool parseCarBrand(const char* text, CarBrand& out) {
    if (!text) {
        return false;
    }
    const size_t len = strcspn(text, "\r\n");
    if (len == 4 && !strncmp(text, "MINI", 4)) {
        out = CarBrand::kMini;
        return true;
    }
    if (len == 3 && !strncmp(text, "BMW", 3)) {
        out = CarBrand::kBmw;
        return true;
    }
    return false;
}

// The MINI's central screen sits in a round surround that hides about 40 pixels on each side of a 1280-pixel-wide
// panel, and the same share of a narrower one: the width hidden on each side, in panel pixels. 0 on a BMW.
inline uint32_t surroundSide(CarBrand brand, uint32_t panelWidth) {
    return brand == CarBrand::kMini ? (40 * panelWidth + 640) / 1280 : 0;
}

}  // namespace aa

#endif  // ENTRY_AAD_PLATFORM_CAR_BRAND_HPP
