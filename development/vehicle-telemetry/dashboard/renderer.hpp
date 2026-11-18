// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#include <memory>
#include <vector>

#include "../telemetry_model.hpp"
#include "panel_data.hpp"
namespace telemetry {
const unsigned DashboardWidth = 1200, DashboardHeight = 424;
class DashboardRenderer {
public:
    DashboardRenderer(unsigned width, unsigned height, const char* font);
    ~DashboardRenderer();
    const std::vector<unsigned char>& render(const Model&, double now, bool demo,
                                             const PanelExtras& extras = PanelExtras(), bool recording = false);
    bool changed() const { return changed_; }
    unsigned frames() const { return frames_; }
    unsigned width() const { return width_; }
    unsigned height() const { return height_; }

private:
    struct Font;
    struct DialCache;
    std::unique_ptr<Font> font_;
    std::unique_ptr<DialCache> dials_;
    uint64_t signature_;
    bool changed_;
    unsigned frames_;
    unsigned width_, height_;
    std::vector<unsigned char> background_, pixels_, scanlines_, compressed_, png_;
    void pixel(int, int, unsigned, unsigned = 255);
    void rect(int, int, int, int, unsigned);
    void line(double, double, double, double, double, unsigned);
    void text(int, int, const char16_t*, unsigned, unsigned, bool centered = false, bool strong = false);
    int text_width(const char16_t*, unsigned);
    void value(int, int, const PanelSample&, const char16_t*, unsigned, unsigned, const char* format = "%.0f",
               bool centered = false, bool left_aligned = false);
    void ascii(int, int, const char*, unsigned, unsigned, bool centered = false, bool strong = false);
    void ring(double, double, double, double, double, unsigned);
    void dial(double, double, double, double, double, double);
    void icon(unsigned, double, double, double, unsigned);
    void base();
    DashboardRenderer(const DashboardRenderer&);
    DashboardRenderer& operator=(const DashboardRenderer&);
};
}  // namespace telemetry
