// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The car's series as iDrive is coded ("G20", "F48", "F56"): the model the phone is told about when the vehicle file
// gives none (#15).
//
// iDrive's coding feature BAUREIHE (libhmicoding.so, its table of coding features; block 0x3004 SYSTEM_EINSTELLUNGEN,
// byte 4, the byte CarPlay reads to name the car to an iPhone): one variant per series, va_BAUREIHE_<series>. The
// iDrive app asks its variant server for each id below (hmiapp/idrive_coding.cpp); exactly one set, and not
// va_BAUREIHE_Unbekannt, gives the series, written to contract::kSeriesFile; the service reads it at start. Only that
// byte is ever read, never the rest of the block (the car's VIN is in it). Pure: no I/O. tests/test_evo_sdr.cpp.
#ifndef ENTRY_AAD_PLATFORM_CAR_SERIES_HPP
#define ENTRY_AAD_PLATFORM_CAR_SERIES_HPP

#include <stddef.h>
#include <string.h>

namespace aa {

struct SeriesVariant {
    unsigned id;
    const char* series;
};

const unsigned kVariantSeriesUnknown = 0x53C;  // va_BAUREIHE_Unbekannt

// va_BAUREIHE_<series> of libhmicoding.so (G20 software 202111011321).
const SeriesVariant kSeriesVariants[] = {
    // F series
    {0x53D, "F01"},
    {0x53E, "F02"},
    {0x53F, "F03"},
    {0x540, "F04"},
    {0x541, "F06"},
    {0x542, "F07"},
    {0x543, "F10"},
    {0x544, "F11"},
    {0x545, "F12"},
    {0x546, "F13"},
    {0x547, "F15"},
    {0x548, "F16"},
    {0x549, "F18"},
    {0x54A, "F20"},
    {0x54B, "F21"},
    {0x54C, "F22"},
    {0x54D, "F23"},
    {0x54E, "F25"},
    {0x54F, "F26"},
    {0x550, "F30"},
    {0x551, "F31"},
    {0x552, "F32"},
    {0x553, "F33"},
    {0x554, "F34"},
    {0x555, "F35"},
    {0x556, "F36"},
    {0xE9C, "F40"},
    {0xEFF, "F39"},
    {0xF50, "F44"},
    {0xA52, "F52"},
    {0x557, "F45"},
    {0x558, "F46"},
    {0x559, "F47"},
    {0x55A, "F48"},
    {0x55B, "F49"},
    {0x561, "F80"},
    {0x562, "F82"},
    {0x563, "F83"},
    {0xE9B, "F88"},
    {0x564, "F85"},
    {0x565, "F86"},
    {0xC9B, "F87"},
    {0xE12, "F97"},
    {0xE13, "F98"},
    {0xDBB, "F90"},
    {0xE10, "F91"},
    {0xE11, "F92"},
    // MINI
    {0x55C, "F54"},
    {0x55D, "F55"},
    {0x55E, "F56"},
    {0x55F, "F57"},
    {0x560, "F60"},
    // G series
    {0x956, "G11"},
    {0x957, "G12"},
    {0xDBC, "G05"},
    {0xDBD, "G06"},
    {0xDBE, "G07"},
    {0xDC0, "G14"},
    {0xDC1, "G15"},
    {0xDC2, "G16"},
    {0xDC3, "G20"},
    {0xDC4, "G21"},
    {0xDC5, "G22"},
    {0xDC6, "G23"},
    {0xDC7, "G24"},
    {0xDC8, "G26"},
    {0xDC9, "G28"},
    {0xDCA, "G29"},
    {0xB8F, "G30"},
    {0xB90, "G31"},
    {0xB91, "G38"},
    {0xDCC, "G32"},
    {0xDCD, "G01"},
    {0xDCE, "G02"},
    {0xDBF, "G08"},
    // Supra, i
    {0xDCB, "J29"},
    {0x56B, "I01"},
    {0x56C, "I12"},
    {0xEE9, "I15"},
    // Rolls-Royce
    {0x566, "RR04"},
    {0x567, "RR05"},
    {0xB68, "RR06"},
    {0x568, "RR11"},
    {0x569, "RR12"},
    {0x56A, "RR14"},
    {0xF56, "RR21"},
    {0xF57, "RR22"},
    {0xE2D, "RR24"},
    {0xE2E, "RR25"},
    {0xE2C, "RR31"},
};
const size_t kSeriesVariantCount = sizeof(kSeriesVariants) / sizeof(kSeriesVariants[0]);

// The series file's line: 2 to 5 of A-Z and 0-9 ("G20", "RR31"), an end of line allowed; false otherwise (out
// unchanged). out holds at least 6 bytes.
inline bool parseCarSeries(const char* text, char* out) {
    if (!text) {
        return false;
    }
    const size_t len = strcspn(text, "\r\n");
    if (len < 2 || len > 5) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    memcpy(out, text, len);
    out[len] = 0;
    return true;
}

}  // namespace aa

#endif
