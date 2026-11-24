// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The picture asked of the phone for a given panel.

// Android Auto streams one of three canvases: 800x480, 1280x720 or 1920x1080. The smallest one that holds the panel is
// asked for; the phone is told to keep its UI in the top-left part the panel shows (insets, core/sdr_builder.cpp),
// and the video surface shows that part only (platform/evo_video.cpp). The reference car is one case of the rule: a
// 1280x480 panel in the 1280x720 canvas, its top 480 rows shown.
// A panel partly hidden by its surround (the MINI's round one, platform/car_brand.hpp) keeps the whole picture; the
// phone is told to keep its UI away from the hidden sides.
// The insets are the phone's margins: its own display is the canvas less them, and it takes touches in that display's
// pixels. A panel pixel x is the phone's x - leftInset() (platform/evo_touch.hpp); without a left margin, the same.
#ifndef ENTRY_AAD_CORE_DISPLAY_PLAN_HPP
#define ENTRY_AAD_CORE_DISPLAY_PLAN_HPP

#include <stdint.h>

namespace aa {

struct DisplayPlan {
    uint32_t width, height;              // the panel, and the part of the canvas that is shown
    uint32_t canvasWidth, canvasHeight;  // what the phone streams
    uint32_t resolution;                 // the canvas as the protocol numbers it: 1, 2 or 3
    uint32_t hiddenSide;                 // panel columns hidden on each side by the surround
    // The reference car.
    DisplayPlan() : width(1280), height(480), canvasWidth(1280), canvasHeight(720), resolution(2), hiddenSide(0) {}
    uint32_t bottomInset() const { return canvasHeight - height; }
    uint32_t leftInset() const { return hiddenSide; }
    uint32_t rightInset() const { return canvasWidth - width + hiddenSide; }
    // The phone's own display, the canvas less its margins: the area its touches are given in.
    uint32_t phoneWidth() const { return canvasWidth - leftInset() - rightInset(); }
    uint32_t phoneHeight() const { return canvasHeight - bottomInset(); }
    bool reference() const { return width == 1280 && height == 480; }
    // A panel size already run in a car: the reference one, and 800x480 (docs/COMPATIBILITE.md).
    bool triedInCar() const { return reference() || (width == 800 && height == 480); }
};

// The plan for a panel; false for an empty panel or one larger than every canvas.
inline bool planDisplay(uint32_t panelWidth, uint32_t panelHeight, DisplayPlan& out) {
    static const uint32_t kCanvas[3][2] = {{800, 480}, {1280, 720}, {1920, 1080}};
    if (!panelWidth || !panelHeight) {
        return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
        if (panelWidth <= kCanvas[i][0] && panelHeight <= kCanvas[i][1]) {
            out.width = panelWidth;
            out.height = panelHeight;
            out.canvasWidth = kCanvas[i][0];
            out.canvasHeight = kCanvas[i][1];
            out.resolution = i + 1;
            out.hiddenSide = 0;
            return true;
        }
    }
    return false;
}

}  // namespace aa

#endif
