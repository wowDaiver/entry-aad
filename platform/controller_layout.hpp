// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Which key each controller button is, the way iDrive itself decides it: from the car's coding.
//
// Read in the reference car's HmiMain3D (iDrive 202111011321). iDrive turns a button into one of its own key codes:
// 1 MENU, 2 BACK, 3 OPTION, 4 MEDIA, 5 RADIO and 15 (the media key of the small F controllers, 15 with variant
// 0x5141), 6 TEL (COM), 7 NAV (G style), 17 map / navigation (MAP on G controllers, NAV on F ones). Several buttons
// depend on coding variants, read with IHmiVariantServer::isVariantSet; 0xB80 is the G-style controller (COM and MAP
// keys):
//   older protocol, E5.01 C20, cluster 0x00, one sensor bit per button (CHmiCCSController::pushbuttonToKeycode):
//     0x01 1, 0x02 2, 0x04 3, 0x10 4;
//     0x08: variant 0x5141 without 0xB80: 15; without 0xB80: 5; 0x4E59: 6; role 1: 5; else 6;
//     0x20: neither 0x4F1E nor 0x5486: nothing; 0xB80: 7; else 17;
//     0x40: 0xB80: 17; 0x4E59: 6; else nothing.
//   CCS35up, C47/C4F, one byte per button (CDevCCS35up::pushbuttonsToKeycodeAction):
//     bytes 0, 1, 2, 4: 1, 2, 3, 4; byte 3: 0xB80 ? 6 : 5; byte 5: 0xB80 ? 7 : 17; byte 6: 0xB80 ? 17 : 6.
// A 5-key F20 ZBE3 (#55) sends MEDIA as 0x08 and COM as 0x40: read as COM and MAP before this table. That car is
// coded 0x5141 without 0xB80: its MEDIA is iDrive's code 15, the only button that can give it.
//
// Then iDrive's code becomes one of the decoder's slots (evo_ccs.hpp): Android Auto treats a key as on the G20, where
// MEDIA, NAV and COM open its apps. So RADIO and 15, the only media key of the small F controllers, are MEDIA
// (choice of 14/11/2026), and the F controllers' NAV key (code 17) is NAV; only a G controller's MAP stays MAP.
//
// The iDrive app reads the variants inside HmiMain3D and writes them to contract::kControllerFile; without that file
// (an older app), the G-style default below is the table used before. Pure: no I/O. tests/test_evo_input.cpp.
#ifndef ENTRY_AAD_PLATFORM_CONTROLLER_LAYOUT_HPP
#define ENTRY_AAD_PLATFORM_CONTROLLER_LAYOUT_HPP

#include <stdio.h>
#include <string.h>

namespace aa {

// The variants iDrive reads for its keys, and its processing unit role (IHmiVariantServer::getRole).
struct ControllerCoding {
    bool known;   // read from the car; false: the G-style default
    bool gStyle;  // variant 0xB80
    bool v5141, v4e59, v4f1e, v5486;
    int role;
};

// The variant numbers, as iDrive asks for them.
const unsigned kVariantGStyle = 0xB80, kVariant5141 = 0x5141, kVariant4E59 = 0x4E59, kVariant4F1E = 0x4F1E,
               kVariant5486 = 0x5486;

// The table used before the car's coding was read: a G controller, every key present.
inline ControllerCoding defaultControllerCoding() {
    ControllerCoding c;
    c.known = false;
    c.gStyle = true;
    c.v5141 = false;
    c.v4e59 = c.v4f1e = c.v5486 = true;
    c.role = 0;
    return c;
}

// Slots of the decoder (evo_ccs.hpp, same order); -1 none.
enum { kSlotMenu, kSlotBack, kSlotOptions, kSlotCom, kSlotMedia, kSlotNav, kSlotMap, kNoSlot = -1 };

// iDrive's key code of an older protocol sensor bit (cluster 0x00); -1 none.
inline int olderSensorCode(unsigned sensor, const ControllerCoding& c) {
    switch (sensor) {
        case 0x01:
            return 1;
        case 0x02:
            return 2;
        case 0x04:
            return 3;
        case 0x10:
            return 4;
        case 0x08:
            if (c.v5141 && !c.gStyle) {
                return 15;
            }
            return !c.gStyle ? 5 : c.v4e59 ? 6 : c.role == 1 ? 5 : 6;
        case 0x20:
            if (!c.v4f1e && !c.v5486) {
                return -1;
            }
            return c.gStyle ? 7 : 17;
        case 0x40:
            return c.gStyle ? 17 : c.v4e59 ? 6 : -1;
        default:
            return -1;
    }
}

// iDrive's key code of a CCS35up push button byte (0 to 6); -1 none.
inline int ccs35upByteCode(int byte, const ControllerCoding& c) {
    switch (byte) {
        case 0:
            return 1;
        case 1:
            return 2;
        case 2:
            return 3;
        case 3:
            return c.gStyle ? 6 : 5;
        case 4:
            return 4;
        case 5:
            return c.gStyle ? 7 : 17;
        case 6:
            return c.gStyle ? 17 : 6;
        default:
            return -1;
    }
}

// The decoder slot of an iDrive key code; kNoSlot for a key Android Auto does not use.
inline int slotOfIdriveCode(int code, bool gStyle) {
    switch (code) {
        case 1:
            return kSlotMenu;
        case 2:
            return kSlotBack;
        case 3:
            return kSlotOptions;
        case 4:
        case 5:
        case 15:
            return kSlotMedia;
        case 6:
            return kSlotCom;
        case 7:
            return kSlotNav;
        case 17:
            return gStyle ? kSlotMap : kSlotNav;
        default:
            return kNoSlot;
    }
}

inline int olderSensorSlot(unsigned sensor, const ControllerCoding& c) {
    return slotOfIdriveCode(olderSensorCode(sensor, c), c.gStyle);
}

inline int ccs35upByteSlot(int byte, const ControllerCoding& c) {
    return slotOfIdriveCode(ccs35upByteCode(byte, c), c.gStyle);
}

// One line, e.g. "coding b80=1 5141=0 4e59=1 4f1e=1 5486=1 role=1".
inline void formatControllerCoding(const ControllerCoding& c, char* out, size_t size) {
    snprintf(out, size, "coding b80=%d 5141=%d 4e59=%d 4f1e=%d 5486=%d role=%d", c.gStyle ? 1 : 0, c.v5141 ? 1 : 0,
             c.v4e59 ? 1 : 0, c.v4f1e ? 1 : 0, c.v5486 ? 1 : 0, c.role);
}

// The line above, every field present; false otherwise (out unchanged).
inline bool parseControllerCoding(const char* text, ControllerCoding& out) {
    int b80 = -1, v5141 = -1, v4e59 = -1, v4f1e = -1, v5486 = -1, role = -1;
    if (!text || sscanf(text, "coding b80=%d 5141=%d 4e59=%d 4f1e=%d 5486=%d role=%d", &b80, &v5141, &v4e59, &v4f1e,
                        &v5486, &role) != 6) {
        return false;
    }
    const int flags[] = {b80, v5141, v4e59, v4f1e, v5486};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        if (flags[i] != 0 && flags[i] != 1) {
            return false;
        }
    }
    if (role < 0 || role > 255) {
        return false;
    }
    out.known = true;
    out.gStyle = b80 == 1;
    out.v5141 = v5141 == 1;
    out.v4e59 = v4e59 == 1;
    out.v4f1e = v4f1e == 1;
    out.v5486 = v5486 == 1;
    out.role = role;
    return true;
}

}  // namespace aa

#endif
