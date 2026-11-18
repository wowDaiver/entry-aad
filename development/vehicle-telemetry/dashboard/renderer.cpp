// SPDX-License-Identifier: GPL-3.0-or-later
#include "renderer.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <stdexcept>
extern "C" int compress2(unsigned char*, unsigned long*, const unsigned char*, unsigned long, int);
namespace telemetry {
struct DashboardRenderer::Font {
    struct Glyph {
        unsigned code, size, index;
        FT_Pos advance;
        int left, top;
        unsigned width, height;
        std::vector<unsigned char> alpha;
    };
    FT_Library library;
    FT_Face face;
    std::vector<Glyph> glyphs;
    Glyph scratch;
    explicit Font(const char* path) : library(0), face(0) {
        if (FT_Init_FreeType(&library)) {
            throw std::runtime_error("font initialization failed");
        }
        if (FT_New_Face(library, path, 0, &face)) {
            FT_Done_FreeType(library);
            throw std::runtime_error("dashboard font unavailable");
        }
    }
    ~Font() {
        FT_Done_Face(face);
        FT_Done_FreeType(library);
    }
    const Glyph& glyph(unsigned code, unsigned size) {
        for (size_t i = 0; i < glyphs.size(); ++i) {
            if (glyphs[i].code == code && glyphs[i].size == size) {
                return glyphs[i];
            }
        }
        if (FT_Set_Pixel_Sizes(face, 0, size) || FT_Load_Char(face, code, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) {
            throw std::runtime_error("font glyph failed");
        }
        FT_GlyphSlot g = face->glyph;
        if (g->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && g->bitmap.width) {
            throw std::runtime_error("unexpected glyph format");
        }
        Glyph item;
        item.code = code;
        item.size = size;
        item.index = FT_Get_Char_Index(face, code);
        item.advance = g->advance.x;
        item.left = g->bitmap_left;
        item.top = g->bitmap_top;
        item.width = g->bitmap.width;
        item.height = g->bitmap.rows;
        item.alpha.resize(item.width * item.height);
        for (unsigned y = 0; y < item.height; ++y) {
            memcpy(item.alpha.data() + y * item.width, g->bitmap.buffer + y * g->bitmap.pitch, item.width);
        }
        // A fixed bound even if a future caller renders arbitrary labels.
        if (glyphs.size() < 512) {
            glyphs.push_back(std::move(item));
            return glyphs.back();
        }
        scratch = std::move(item);
        return scratch;
    }
};
struct DashboardRenderer::DialCache {
    struct Pixel {
        int x, y;
        double fraction;
        unsigned alpha;
    };
    struct Dial {
        double x, y, r, scale;
        std::vector<Pixel> pixels;
    };
    Dial values[3];
    unsigned used;
    DialCache() : used(0) {}
};
namespace {
const double Pi = 3.14159265358979323846;
unsigned color(unsigned r, unsigned g, unsigned b) {
    return (r << 16) | (g << 8) | b;
}
unsigned crc(const unsigned char* p, size_t n) {
    unsigned c = ~0u;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned j = 0; j < 8; ++j) {
            c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320u);
        }
    }
    return ~c;
}
void word(std::vector<unsigned char>& out, unsigned v) {
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<unsigned char>(v >> (i * 8)));
    }
}
void chunk(std::vector<unsigned char>& out, const char* kind, const unsigned char* p, size_t n) {
    word(out, n);
    size_t at = out.size();
    out.insert(out.end(), kind, kind + 4);
    if (n) {
        out.insert(out.end(), p, p + n);
    }
    word(out, crc(out.data() + at, n + 4));
}
void number(char* out, size_t size, const PanelSample& s, const char* format = "%.0f") {
    if (s.has_value()) {
        snprintf(out, size, format, s.value);
    } else {
        snprintf(out, size, "--");
    }
}
unsigned ink(const PanelSample& s) {
    return s.status == Valid ? 0xffffff : 0x8896a5;
}
void selector(char16_t* out, const PanelData& p) {
    const PanelSample& mode = p.rows[PanelData::Mode];
    const PanelSample& gear = p.rows[PanelData::Gear];
    out[0] = u'—';
    out[1] = 0;
    if (!mode.has_value()) {
        return;
    }
    const char16_t* name = enum_value(DriveMode, mode.value);
    out[0] = name[0];
    if ((out[0] == u'D' || out[0] == u'M' || out[0] == u'S') && gear.has_value() && mode.status == gear.status &&
        fabs(mode.received_at - gear.received_at) <= 2) {
        const char16_t* g = enum_value(SelectorPosition, gear.value);
        if (g[0] >= u'1' && g[0] <= u'8') {
            out[1] = g[0];
            out[2] = 0;
        }
    }
}
}  // namespace
DashboardRenderer::DashboardRenderer(unsigned w, unsigned h, const char* path)
    : font_(), dials_(new DialCache), signature_(0), changed_(true), frames_(0), width_(w), height_(h) {
    if (w < 480 || w > 1280 || h < 240 || h > 480) {
        throw std::runtime_error("dashboard size outside bound");
    }
    font_.reset(new Font(path));
    pixels_.resize(w * h * 3);
    background_.resize(pixels_.size());
    scanlines_.resize((w * 3 + 1) * h);
    compressed_.resize(scanlines_.size() + scanlines_.size() / 100 + 128);
    png_.reserve(compressed_.size() + 64);
    base();
    background_ = pixels_;
}
DashboardRenderer::~DashboardRenderer() {}
void DashboardRenderer::pixel(int x, int y, unsigned c, unsigned a) {
    if (x < 0 || y < 0 || x >= int(width_) || y >= int(height_)) {
        return;
    }
    size_t at = (size_t(y) * width_ + x) * 3;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned v = (c >> (16 - i * 8)) & 255;
        pixels_[at + i] = (v * a + pixels_[at + i] * (255 - a) + 127) / 255;
    }
}
void DashboardRenderer::rect(int x, int y, int w, int h, unsigned c) {
    for (int j = std::max(y, 0); j < std::min(y + h, int(height_)); ++j) {
        for (int i = std::max(x, 0); i < std::min(x + w, int(width_)); ++i) {
            pixel(i, j, c);
        }
    }
}
void DashboardRenderer::line(double x1, double y1, double x2, double y2, double width, unsigned c) {
    double dx = x2 - x1, dy = y2 - y1, den = dx * dx + dy * dy, r = width * .5;
    for (int y = int(floor(std::min(y1, y2) - r - 1)); y <= int(ceil(std::max(y1, y2) + r + 1)); ++y) {
        for (int x = int(floor(std::min(x1, x2) - r - 1)); x <= int(ceil(std::max(x1, x2) + r + 1)); ++x) {
            double t = den ? ((x + .5 - x1) * dx + (y + .5 - y1) * dy) / den : 0;
            t = std::max(0., std::min(1., t));
            double a = std::max(0., std::min(1., r + .5 - hypot(x + .5 - x1 - t * dx, y + .5 - y1 - t * dy)));
            if (a) {
                pixel(x, y, c, unsigned(a * 255));
            }
        }
    }
}
int DashboardRenderer::text_width(const char16_t* str, unsigned size) {
    if (FT_Set_Pixel_Sizes(font_->face, 0, size)) {
        throw std::runtime_error("font size failed");
    }
    FT_Pos length = 0;
    FT_UInt previous = 0;
    for (const char16_t* p = str; *p; ++p) {
        const Font::Glyph& g = font_->glyph(*p, size);
        FT_Vector kern = {};
        if (previous && g.index && FT_HAS_KERNING(font_->face)) {
            FT_Get_Kerning(font_->face, previous, g.index, FT_KERNING_DEFAULT, &kern);
        }
        length += kern.x + g.advance;
        previous = g.index;
    }
    return int((length + 32) / 64);
}
void DashboardRenderer::text(int x, int baseline, const char16_t* str, unsigned size, unsigned c, bool centered,
                             bool strong) {
    int length = text_width(str, size);
    if (centered) {
        x -= length / 2;
    }
    FT_Pos pen = FT_Pos(x) * 64;
    FT_UInt previous = 0;
    for (const char16_t* p = str; *p; ++p) {
        const Font::Glyph& g = font_->glyph(*p, size);
        FT_Vector kern = {};
        if (previous && g.index && FT_HAS_KERNING(font_->face)) {
            FT_Get_Kerning(font_->face, previous, g.index, FT_KERNING_DEFAULT, &kern);
        }
        pen += kern.x;
        for (unsigned j = 0; j < g.height; ++j) {
            for (unsigned i = 0; i < g.width + (strong ? 1 : 0); ++i) {
                unsigned a = i < g.width ? g.alpha[j * g.width + i] : 0;
                if (strong && i) {
                    a = std::max(a, unsigned(g.alpha[j * g.width + i - 1]));
                }
                pixel(int((pen + 32) / 64) + g.left + i, baseline - g.top + j, c, a);
            }
        }
        pen += g.advance;
        previous = g.index;
    }
}
void DashboardRenderer::ascii(int x, int y, const char* s, unsigned size, unsigned c, bool center, bool strong) {
    char16_t b[96];
    size_t i = 0;
    for (; s[i] && i < 95; ++i) {
        b[i] = static_cast<unsigned char>(s[i]);
    }
    b[i] = 0;
    text(x, y, b, size, c, center, strong);
}
void DashboardRenderer::value(int x, int baseline, const PanelSample& sample, const char16_t* unit, unsigned size,
                              unsigned unit_size, const char* format, bool centered, bool left_aligned) {
    char n[32];
    number(n, sizeof(n), sample, format);
    char16_t digits[32];
    unsigned i = 0;
    for (; n[i] && i < 31; ++i) {
        digits[i] = static_cast<unsigned char>(n[i]);
    }
    digits[i] = 0;
    int gap = unit[0] ? int(size / 5) : 0;
    int nw = text_width(digits, size), uw = text_width(unit, unit_size), total = nw + gap + uw;
    int left = left_aligned ? x : x - (centered ? total / 2 : total);
    text(left, baseline, digits, size, ink(sample));
    text(left + nw + gap, baseline, unit, unit_size, ink(sample));
}
void DashboardRenderer::ring(double x, double y, double radius, double from, double to, unsigned c) {
    for (double a = from; a < to; a += 1) {
        double t = a * Pi / 180, s = std::min(a + 1, to) * Pi / 180;
        line(x + radius * cos(t), y + radius * sin(t), x + radius * cos(s), y + radius * sin(s),
             std::min(3., radius * .17), c);
    }
}
void DashboardRenderer::dial(double cx, double cy, double r, double scale, double progress, double fresh) {
    DialCache::Dial* cached = 0;
    for (unsigned i = 0; i < dials_->used; ++i) {
        DialCache::Dial& d = dials_->values[i];
        if (d.x == cx && d.y == cy && d.r == r && d.scale == scale) {
            cached = &d;
            break;
        }
    }
    if (!cached) {
        cached = &dials_->values[dials_->used < 3 ? dials_->used++ : 0];
        cached->x = cx;
        cached->y = cy;
        cached->r = r;
        cached->scale = scale;
        cached->pixels.clear();
        const double half = 6 * scale;
        for (int y = int(cy - r - half - 1); y <= int(cy + r + half + 1); ++y) {
            for (int x = int(cx - r - half - 1); x <= int(cx + r + half + 1); ++x) {
                double dx = x + .5 - cx, dy = y + .5 - cy, d = hypot(dx, dy);
                double coverage = std::max(0., std::min(1., half + .5 - fabs(d - r)));
                if (!coverage) {
                    continue;
                }
                double angle = atan2(dy, dx) * 180 / Pi;
                if (angle < 140) {
                    angle += 360;
                }
                if (angle > 400) {
                    continue;
                }
                double segment = fmod(angle - 140, 260. / 30);
                if (segment < .35 || segment > 8.05) {
                    continue;
                }
                cached->pixels.push_back({x, y, (angle - 140) / 260, unsigned(coverage * 255)});
            }
        }
    }
    for (size_t i = 0; i < cached->pixels.size(); ++i) {
        const DialCache::Pixel& p = cached->pixels[i];
        unsigned c = p.fraction < progress ? (fresh ? 0xd6e4ec : 0x8a9da9) : 0x536878;
        pixel(p.x, p.y, c, p.alpha);
    }
    for (unsigned n = 0; n <= 6; ++n) {
        double a = (140 + n * 260. / 6) * Pi / 180;
        line(cx + (r + 11 * scale) * cos(a), cy + (r + 11 * scale) * sin(a), cx + (r + 18 * scale) * cos(a),
             cy + (r + 18 * scale) * sin(a), 1.5 * scale, 0xaebbc5);
    }
    if (progress >= 0) {
        double a = (140 + 260 * progress) * Pi / 180;
        line(cx + (r - 10 * scale) * cos(a), cy + (r - 10 * scale) * sin(a), cx + (r + 12 * scale) * cos(a),
             cy + (r + 12 * scale) * sin(a), 4 * scale, fresh ? 0x00baf4 : 0x8a9da9);
    }
}
void DashboardRenderer::icon(unsigned kind, double x, double y, double s, unsigned c) {
    // Shared 40px drawing grid, with strokes that remain visible on a 480px display.
    const double w = 2.8 * s;
    if (kind == 0) {  // Thermometer and coolant waves.
        line(x - 3 * s, y - 16 * s, x - 3 * s, y + 4 * s, w, c);
        line(x + 3 * s, y - 16 * s, x + 3 * s, y + 4 * s, w, c);
        line(x - 3 * s, y - 16 * s, x + 3 * s, y - 16 * s, w, c);
        ring(x, y + 8 * s, 6 * s, 0, 360, c);
        line(x, y - 9 * s, x, y + 10 * s, 2.8 * s, c);
        for (int j = 0; j < 3; ++j) {
            line(x + 7 * s, y - (12 - j * 6) * s, x + 12 * s, y - (12 - j * 6) * s, w, c);
        }
        for (int j = 0; j < 2; ++j) {
            line(x - 19 * s, y + (7 + j * 8) * s, x - 13 * s, y + (4 + j * 8) * s, w, c);
            line(x - 13 * s, y + (4 + j * 8) * s, x - 8 * s, y + (7 + j * 8) * s, w, c);
        }
    } else if (kind == 2) {  // Selector gate, instead of the former microchip outline.
        line(x - 11 * s, y - 13 * s, x - 11 * s, y + 13 * s, w, c);
        line(x + 11 * s, y - 13 * s, x + 11 * s, y + 13 * s, w, c);
        line(x - 11 * s, y, x + 11 * s, y, w, c);
        ring(x - 11 * s, y - 13 * s, 4 * s, 0, 360, c);
        ring(x + 11 * s, y + 13 * s, 4 * s, 0, 360, c);
    } else if (kind == 3) {  // Engine / load.
        line(x - 18 * s, y - 8 * s, x - 18 * s, y + 10 * s, w, c);
        line(x - 18 * s, y + 10 * s, x - 8 * s, y + 10 * s, w, c);
        line(x - 8 * s, y + 10 * s, x - 3 * s, y + 15 * s, w, c);
        line(x - 3 * s, y + 15 * s, x + 13 * s, y + 15 * s, w, c);
        line(x + 13 * s, y + 15 * s, x + 13 * s, y - 10 * s, w, c);
        line(x + 13 * s, y - 10 * s, x - 18 * s, y - 10 * s, w, c);
        line(x - 7 * s, y - 15 * s, x + 5 * s, y - 15 * s, w, c);
        line(x - 1 * s, y - 15 * s, x - 1 * s, y - 10 * s, w, c);
        line(x - 23 * s, y - 5 * s, x - 23 * s, y + 6 * s, w, c);
        line(x - 23 * s, y, x - 18 * s, y, w, c);
        line(x + 13 * s, y - 3 * s, x + 20 * s, y - 7 * s, w, c);
        line(x + 20 * s, y - 7 * s, x + 20 * s, y + 11 * s, w, c);
        line(x + 20 * s, y + 11 * s, x + 13 * s, y + 7 * s, w, c);
    } else if (kind == 4) {  // Speed / revs.
        ring(x, y + 4 * s, 17 * s, 145, 395, c);
        line(x, y + 4 * s, x + 9 * s, y - 8 * s, w, c);
        ring(x, y + 4 * s, 2 * s, 0, 360, c);
    } else if (kind == 5 || kind == 8 || kind == 9) {  // Battery, current and recuperation.
        line(x - 18 * s, y - 10 * s, x + 16 * s, y - 10 * s, w, c);
        line(x + 16 * s, y - 10 * s, x + 16 * s, y + 12 * s, w, c);
        line(x + 16 * s, y + 12 * s, x - 18 * s, y + 12 * s, w, c);
        line(x - 18 * s, y + 12 * s, x - 18 * s, y - 10 * s, w, c);
        line(x + 20 * s, y - 3 * s, x + 20 * s, y + 5 * s, 3 * s, c);
        if (kind == 5) {
            line(x - 12 * s, y + 1 * s, x - 4 * s, y + 1 * s, w, c);
            line(x - 8 * s, y - 3 * s, x - 8 * s, y + 5 * s, w, c);
            line(x + 4 * s, y + 1 * s, x + 11 * s, y + 1 * s, w, c);
        } else {
            line(x + 2 * s, y - 7 * s, x - 5 * s, y + 2 * s, w, c);
            line(x - 5 * s, y + 2 * s, x + 4 * s, y + 2 * s, w, c);
            line(x + 4 * s, y + 2 * s, x - 3 * s, y + 9 * s, w, c);
        }
    } else if (kind == 7) {  // Parking brake, with the native-style side brackets.
        ring(x, y, 12 * s, 0, 360, c);
        ring(x, y, 19 * s, 140, 220, c);
        ring(x, y, 19 * s, -40, 40, c);
        text(x, y + 7 * s, u"P", unsigned(21 * s), c, true);
    } else if (kind == 10) {  // Torque / rotating shaft.
        ring(x, y, 16 * s, 35, 310, c);
        line(x + 10 * s, y - 12 * s, x + 1 * s, y - 14 * s, w, c);
        line(x + 10 * s, y - 12 * s, x + 8 * s, y - 3 * s, w, c);
        line(x, y - 8 * s, x, y + 8 * s, w, c);
        line(x - 6 * s, y, x + 6 * s, y, w, c);
    } else if (kind == 11) {  // Power.
        line(x + 3 * s, y - 17 * s, x - 10 * s, y + 2 * s, w, c);
        line(x - 10 * s, y + 2 * s, x + 2 * s, y + 2 * s, w, c);
        line(x + 2 * s, y + 2 * s, x - 3 * s, y + 17 * s, w, c);
        line(x - 3 * s, y + 17 * s, x + 12 * s, y - 4 * s, w, c);
        line(x + 12 * s, y - 4 * s, x, y - 4 * s, w, c);
        line(x, y - 4 * s, x + 3 * s, y - 17 * s, w, c);
    }
}
void DashboardRenderer::base() {
    for (unsigned y = 0; y < height_; ++y) {
        double t = double(y) / height_;
        rect(0, y, width_, 1, color(22 - 13 * t, 33 - 18 * t, 46 - 25 * t));
    }
    double sx = double(width_) / DashboardWidth, sy = double(height_) / DashboardHeight, scale = std::min(sx, sy);
    const char16_t* names[] = {u"Температура ОЖ", u"Обороты", u"Скорость"};
    const unsigned icons[] = {0, 4, 4};
    for (unsigned i = 0; i < 3; ++i) {
        double cx = (200 + 400 * i) * sx;
        icon(icons[i], cx, 56 * sy, .65 * scale, 0xffffff);
        text(cx, 93 * sy, names[i], unsigned(19 * scale), 0xd7e2ed, true);
        ascii(cx - 110 * scale, 234 * sy, i == 0 ? "50" : "0", unsigned(15 * scale), 0xabbccc, true);
        text(cx + 110 * scale, 234 * sy,
             i == 0   ? u"130 °C"
             : i == 1 ? u"6000"
                      : u"240",
             unsigned(15 * scale), 0xabbccc, true);
    }
    for (unsigned i = 0; i < 6; ++i) {
        int x = (12 + (i % 3) * 396) * sx, y = (258 + (i / 3) * 53) * sy;
        rect(x, y, 384 * sx, 46 * sy, 0x263644);
        rect(x + 18 * sx, y + 36 * sy, 348 * sx, 4 * sy, 0x506273);
    }
    rect(12 * sx, 365 * sy, 1176 * sx, 1, 0x4d6276);
    const char16_t* labels[] = {u"Селектор", u"Темп. АКПП", u"Ручник", u"Рекуперация"};
    for (unsigned i = 0; i < 4; ++i) {
        int x = 300 * i * sx;
        if (i) {
            rect(x, 376 * sy, 1, 40 * sy, 0x4d6276);
        }
        text(x + 64 * sx, 387 * sy, labels[i], unsigned(16 * scale), 0xbdccdb);
    }
}
const std::vector<unsigned char>& DashboardRenderer::render(const Model& m, double now, bool demo,
                                                            const PanelExtras& extras, bool recording) {
    PanelData p(m, extras, now);
    uint64_t sig = recording ? 3 : demo ? 1 : 2;
    bool stale = false;
    for (unsigned i = 0; i < PanelData::Count; ++i) {
        const PanelSample& r = p.rows[i];
        uint64_t bits = 0;
        if (r.has_value()) {
            memcpy(&bits, &r.value, sizeof(bits));
        }
        sig = (sig ^ bits) * 1099511628211ULL;
        sig = (sig ^ r.status) * 1099511628211ULL;
        stale = stale || r.status == Stale;
    }
    changed_ = png_.empty() || signature_ != sig;
    if (!changed_) {
        return png_;
    }
    signature_ = sig;
    ++frames_;
    pixels_ = background_;
    double sx = double(width_) / DashboardWidth, sy = double(height_) / DashboardHeight, scale = std::min(sx, sy);
    const unsigned large[] = {PanelData::Temp, PanelData::Rev, PanelData::Velocity};
    const double low[] = {50, 0, 0}, high[] = {130, 6000, 240};
    for (unsigned i = 0; i < 3; ++i) {
        const PanelSample& r = p.rows[large[i]];
        double cx = (200 + 400 * i) * sx;
        double f = r.has_value() ? std::max(0., std::min(1., (r.value - low[i]) / (high[i] - low[i]))) : -1;
        dial(cx, 128 * sy, 106 * scale, scale, f, r.status == Valid);
        value(cx, 157 * sy, r,
              i == 0   ? u"°C"
              : i == 2 ? u"км/ч"
                       : u"",
              unsigned(64 * scale), unsigned(20 * scale), "%.0f", true);
        if (i == 0) {
            const PanelSample& oil = p.rows[PanelData::Oil];
            char n[32];
            number(n, sizeof(n), oil);
            char16_t label[64] = u"Масло  ";
            unsigned at = 7;
            for (unsigned j = 0; n[j] && at < 56; ++j) {
                label[at++] = static_cast<unsigned char>(n[j]);
            }
            label[at++] = u' ';
            label[at++] = u'°';
            label[at++] = u'C';
            label[at] = 0;
            text(cx, 189 * sy, label, unsigned(18 * scale), ink(oil), true);
        }
        if (i == 1) {
            text(cx, 189 * sy, u"об/мин", unsigned(18 * scale), 0xd7e2ed, true);
        }
        if (i == 2) {
            char16_t gear[4];
            selector(gear, p);
            char16_t label[32] = u"Селектор  ";
            unsigned at = 10;
            for (unsigned j = 0; gear[j]; ++j) {
                label[at++] = gear[j];
            }
            label[at] = 0;
            text(cx, 189 * sy, label, unsigned(18 * scale), ink(p.rows[PanelData::Mode]), true);
        }
    }
    const unsigned fields[] = {PanelData::Load, PanelData::Torque,  PanelData::Power,
                               PanelData::Volt, PanelData::Current, PanelData::Soc};
    const char16_t* labels[] = {u"Нагрузка", u"Момент", u"Мощность · расч.", u"Напряжение", u"Ток АКБ", u"Заряд АКБ"};
    const char16_t* units[] = {u"%", u"Н·м", u"л.с.", u"В", u"А", u"%"};
    const unsigned icons[] = {3, 10, 11, 5, 8, 5};
    const double maxs[] = {100, 500, 250, 16, 100, 100};
    for (unsigned i = 0; i < 6; ++i) {
        const PanelSample& r = p.rows[fields[i]];
        int x = (12 + (i % 3) * 396) * sx, y = (258 + (i / 3) * 53) * sy;
        icon(icons[i], x + 28 * sx, y + 19 * sy, .53 * scale, r.status == Valid ? 0xffffff : 0x8896a5);
        text(x + 52 * sx, y + 26 * sy, labels[i], unsigned(18 * scale), 0xe3edf6);
        value(x + 366 * sx, y + 27 * sy, r, units[i], unsigned(25 * scale), unsigned(18 * scale),
              i == 3   ? "%.1f"
              : i == 4 ? "%+.1f"
                       : "%.0f");
        double f = r.has_value() ? std::max(0., std::min(1., fabs(r.value) / maxs[i])) : 0;
        int start = x + 18 * sx, w = 348 * sx;
        if (i == 4) {
            start += w / 2;
            if (r.value < 0) {
                start -= int(w * f / 2);
            }
            w /= 2;
        }
        if (f) {
            rect(start, y + 36 * sy, int(w * f), 4 * sy, r.status == Valid ? 0x00c8ff : 0x8896a5);
        }
        if (i == 4) {
            rect(x + 192 * sx, y + 36 * sy, 1, 4 * sy, 0xe5f5ff);
        }
    }
    const unsigned bottom_fields[] = {PanelData::Mode, PanelData::Atf, PanelData::Brake, PanelData::Regen};
    const unsigned bottom_icons[] = {2, 0, 7, 9};
    for (unsigned i = 0; i < 4; ++i) {
        const PanelSample& r = p.rows[bottom_fields[i]];
        unsigned c = r.status == Valid ? 0x44d5ff : 0x8896a5;
        if (i == 2 && r.status == Valid) {
            c = r.value ? 0xff526a : 0xffffff;
        }
        if (i == 3 && r.status == Valid && r.value) {
            c = 0x5af0b2;
        }
        icon(bottom_icons[i], (32 + 300 * i) * sx, 400 * sy, .82 * scale, c);
    }
    char16_t gear[4];
    selector(gear, p);
    text(64 * sx, 414 * sy, gear, unsigned(26 * scale), ink(p.rows[PanelData::Mode]));
    value(364 * sx, 414 * sy, p.rows[PanelData::Atf], u"°C", unsigned(26 * scale), unsigned(21 * scale), "%.0f", false,
          true);
    const PanelSample& brake = p.rows[PanelData::Brake];
    const PanelSample& regen = p.rows[PanelData::Regen];
    text(664 * sx, 414 * sy, brake.has_value() ? (brake.value ? u"Затянут" : u"Снят") : u"—", unsigned(25 * scale),
         brake.status == Valid && brake.value ? 0xff526a : ink(brake));
    text(964 * sx, 414 * sy, regen.has_value() ? (regen.value ? u"Активна" : u"Не активна") : u"—",
         unsigned(25 * scale), regen.status == Valid && regen.value ? 0x5af0b2 : ink(regen));
    bool any = false;
    for (unsigned i = 0; i < PanelData::Count; ++i) {
        any = any || p.rows[i].has_value();
    }
    const char16_t* note = recording ? (stale  ? u"Офлайн · запись · Последние данные"
                                        : !any ? u"Офлайн · запись · Нет данных"
                                               : u"Офлайн · запись ответов блоков")
                           : stale ? u"Последние данные · серые значения устарели"
                           : !any  ? u"Нет данных"
                           : demo  ? u"Эмулятор · тестовые данные"
                                   : u"";
    text(600 * sx, 250 * sy, note, unsigned(12 * scale), stale ? 0xf2c36b : 0xa9bed1, true);
    // RGB PNG, Sub filtering + zlib level 1. Fixed bounded input, no external image codec.
    for (unsigned y = 0; y < height_; ++y) {
        size_t out = size_t(y) * (width_ * 3 + 1), src = size_t(y) * width_ * 3;
        scanlines_[out++] = 1;
        for (unsigned x = 0; x < width_ * 3; ++x) {
            scanlines_[out + x] = pixels_[src + x] - (x >= 3 ? pixels_[src + x - 3] : 0);
        }
    }
    unsigned long size = compressed_.size();
    int err = compress2(compressed_.data(), &size, scanlines_.data(), scanlines_.size(), 1);
    if (err) {
        throw std::runtime_error("dashboard PNG compression failed");
    }
    png_.clear();
    const unsigned char magic[] = {137, 80, 78, 71, 13, 10, 26, 10};
    png_.insert(png_.end(), magic, magic + 8);
    unsigned char header[13] = {0, 0, 0, 0, 0, 0, 0, 0, 8, 2, 0, 0, 0};
    for (int j = 0; j < 4; ++j) {
        header[j] = width_ >> ((3 - j) * 8);
        header[j + 4] = height_ >> ((3 - j) * 8);
    }
    chunk(png_, "IHDR", header, 13);
    chunk(png_, "IDAT", compressed_.data(), size);
    chunk(png_, "IEND", 0, 0);
    return png_;
}
}  // namespace telemetry
