// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Drives the IVI LayerManager through its GENIVI command-line tool (see evo_ilm.hpp).

// Each action runs /usr/bin/LayerManagerControl through posix_spawnp, never a shell: nothing is injected or
// interpreted. The screen's render order is never rewritten; only OUR surface is added to or removed from a layer's
// render order, the other surfaces kept.
//
// Output formats of the tool:
//   - resolution:                  x=<w>, y=<h>
//   - layer/surface render order:  <decimal>(0x<id>), ...  (with a final comma on the car)
//   - plain hexadecimal lists are accepted too, for tools and tests.
#include "evo_ilm.hpp"

#include <pthread.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "evo_log.hpp"
#include "evo_process.hpp"

namespace evo {

namespace {
// A layer's render order is read, changed and written back: the picture (video thread) and the volume pill (its own
// thread) must not interleave those steps, or one would put back an order without the other's surface.
pthread_mutex_t g_orderMu = PTHREAD_MUTEX_INITIALIZER;
struct OrderLock {
    OrderLock() { pthread_mutex_lock(&g_orderMu); }
    ~OrderLock() { pthread_mutex_unlock(&g_orderMu); }
};
}  // namespace

EvoIlm::EvoIlm() : ctl_("/usr/bin/LayerManagerControl"), compositorConf_("/etc/wayland/compositor.conf") {}

EvoIlm::EvoIlm(const std::string& ctl, const std::string& compositorConf)
    : ctl_(ctl), compositorConf_(compositorConf) {}

static std::string trimmed(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) {
        return "";
    }
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

bool EvoIlm::parseRenderOrderFlip(const std::string& conf, bool& topFirst) {
    std::istringstream input(conf);
    std::string line;
    unsigned found = 0;
    bool value = false;
    while (std::getline(input, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || trimmed(line.substr(0, eq)) != "genivi-flip-renderorder") {
            continue;
        }
        std::string v = trimmed(line.substr(eq + 1));
        if (v == "1" || v == "true" || v == "yes" || v == "on") {
            value = true;
        } else if (v == "0" || v == "false" || v == "no" || v == "off") {
            value = false;
        } else {
            return false;
        }
        ++found;
    }
    if (found > 1) {
        return false;  // Ambiguous: never guess which line winston keeps.
    }
    topFirst = value;
    return true;
}

bool EvoIlm::renderOrderTopFirst(bool& topFirst) {
    std::ifstream file(compositorConf_.c_str(), std::ios::in | std::ios::binary);
    std::string text;
    char buf[4096];
    while (file && text.size() <= 65536) {
        file.read(buf, sizeof(buf));
        text.append(buf, static_cast<size_t>(file.gcount()));
    }
    if (!file.eof() || text.size() > 65536) {
        ELOG("ILM", "compositor configuration unreadable (%s): stacking order unknown", compositorConf_.c_str());
        return false;
    }
    if (!parseRenderOrderFlip(text, topFirst)) {
        ELOG("ILM", "genivi-flip-renderorder invalid or repeated in %s: stacking order unknown",
             compositorConf_.c_str());
        return false;
    }
    return true;
}

bool EvoIlm::exec(const std::vector<std::string>& args, std::string* out) {
    // Logs the exact command line (no shell involved).
    std::string shown = ctl_;
    for (size_t i = 0; i < args.size(); ++i) {
        shown += ' ';
        shown += args[i];
    }
    ELOG("ILM", "%s", shown.c_str());

    std::string captured;
    CommandReport report;
    bool ok = runCommand(ctl_, args, captured, 2000, &report);  // include stderr: parser errors exit 0
    // On this car the tool can exit 0 after a failed ILM call (seen in the car's logs).
    const std::string statusPrefix = "LayerManagerService returned: ";
    size_t status = captured.find(statusPrefix);
    if (captured.find("Interpreter error:") != std::string::npos ||
        (status != std::string::npos && captured.compare(status + statusPrefix.size(), 7, "success") != 0)) {
        ELOG("ILM", "refused by the CLI: %s", captured.c_str());
        ok = false;
    }
    if (!ok) {
        captured.clear();
    }
    if (out) {
        *out = captured;
    }
    if (!ok) {
        ELOG("ILM", "  -> command failed or timed out");
    }
    return ok;
}

static std::string u32s(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "0x%x", v);
    return b;
}

std::string EvoIlm::get(const char* what, uint32_t id) {
    std::vector<std::string> a;
    a.push_back("get");
    a.push_back(what);
    a.push_back(u32s(id));
    std::string out;
    if (!exec(a, &out)) {
        out.clear();
    }
    return out;
}

bool EvoIlm::parseRenderOrder(const std::string& txt, std::vector<uint32_t>& out) {
    out.clear();
    size_t p = txt.find("render order:");
    if (p == std::string::npos) {
        return false;
    }
    size_t eol = txt.find('\n', p);
    std::string line = txt.substr(p + 13, eol == std::string::npos ? std::string::npos : eol - p - 13);
    size_t i = 0;
    while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    bool bracket = i < line.size() && line[i] == '[';
    const bool bracketed = bracket;
    if (bracket) {
        ++i;
    }
    std::vector<uint32_t> parsed;
    bool comma = false, nativePair = false;
    for (;;) {
        while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (bracket && i < line.size() && line[i] == ']') {
            ++i;
            bracket = false;
            while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) {
                ++i;
            }
            if (i != line.size()) {
                return false;
            }
        }
        // The car's tool prints decimal(hex) pairs, with a final comma.
        if (i == line.size()) {
            if (bracket || (comma && !(nativePair && !bracketed))) {
                return false;
            }
            break;
        }
        nativePair = line.compare(i, 2, "0x") != 0 && line.compare(i, 2, "0X") != 0;
        unsigned long decimal = 0;
        if (nativePair) {
            if (!isdigit(static_cast<unsigned char>(line[i]))) {
                return false;
            }
            char* end = nullptr;
            errno = 0;
            decimal = strtoul(line.c_str() + i, &end, 10);
            if (errno || decimal > 0xffffffffUL || *end != '(') {
                return false;
            }
            i = static_cast<size_t>(end - line.c_str()) + 1;
        }
        if (line.compare(i, 2, "0x") != 0 && line.compare(i, 2, "0X") != 0) {
            return false;
        }
        char* end = nullptr;
        errno = 0;
        const char* begin = line.c_str() + i + 2;
        unsigned long value = strtoul(begin, &end, 16);
        if (errno || end == begin || value > 0xffffffffUL || !isxdigit(static_cast<unsigned char>(*begin))) {
            return false;
        }
        if (nativePair) {
            if (*end != ')' || decimal != value) {
                return false;
            }
            ++end;
        }
        for (size_t n = 0; n < parsed.size(); ++n) {
            if (parsed[n] == value) {
                return false;
            }
        }
        parsed.push_back(static_cast<uint32_t>(value));
        i = static_cast<size_t>(end - line.c_str());
        comma = false;
        size_t beforeSpace = i;
        while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i < line.size() && line[i] == ',') {
            ++i;
            comma = true;
        } else if (i < line.size() && i == beforeSpace && !(bracket && line[i] == ']')) {
            return false;
        }
    }
    out.swap(parsed);
    return true;
}

bool EvoIlm::parseResolution(const std::string& txt, int& w, int& h) {
    size_t p = txt.find("x=");
    size_t q = txt.find("y=");
    if (p == std::string::npos || q == std::string::npos) {
        return false;
    }
    w = atoi(txt.c_str() + p + 2);
    h = atoi(txt.c_str() + q + 2);
    return w > 0 && h > 0;
}

bool EvoIlm::parseSurfaceList(const std::string& txt, std::vector<uint32_t>& out) {
    out.clear();
    std::istringstream input(txt);
    std::string line;
    unsigned count = 0;
    int used = 0;
    if (!std::getline(input, line) || sscanf(line.c_str(), "%u Surface(s): %n", &count, &used) != 1 || !used ||
        static_cast<size_t>(used) != line.size() || count > 4096) {
        return false;
    }
    std::vector<uint32_t> ids;
    while (std::getline(input, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        unsigned long dec = 0, hex = 0;
        used = 0;
        if (sscanf(line.c_str(), "- Surface %lu (0x%lx) %n", &dec, &hex, &used) != 2 || !used ||
            static_cast<size_t>(used) != line.size() || dec != hex || dec > 0xffffffffUL ||
            std::find(ids.begin(), ids.end(), static_cast<uint32_t>(dec)) != ids.end()) {
            return false;
        }
        ids.push_back(static_cast<uint32_t>(dec));
    }
    if (ids.size() != count) {
        return false;
    }
    out.swap(ids);
    return true;
}

static std::string property(const std::string& text, const char* name) {
    std::istringstream stream(text);
    std::string line, value;
    unsigned found = 0;
    const std::string prefix = std::string("- ") + name + ":";
    while (std::getline(stream, line)) {
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line.compare(p, prefix.size(), prefix)) {
            continue;
        }
        p = line.find_first_not_of(" \t", p + prefix.size());
        value = p == std::string::npos ? "" : line.substr(p);
        size_t end = value.find_last_not_of(" \t\r");
        if (end != std::string::npos) {
            value.resize(end + 1);
        }
        ++found;
    }
    return found == 1 ? value : "";
}

bool EvoIlm::validLayerGeometry(const std::string& text, uint32_t w, uint32_t h) {
    if (property(text, "visibility") != "1" || property(text, "opacity") != "1" ||
        property(text, "orientation") != "0 (up is top)") {
        return false;
    }
    const char* regions[] = {"source region", "destination region"};
    for (unsigned i = 0; i < 2; ++i) {
        std::string region = property(text, regions[i]);
        int x, y, rw, rh, used = 0;
        if (sscanf(region.c_str(), "x=%d, y=%d, w=%d, h=%d%n", &x, &y, &rw, &rh, &used) != 4 ||
            static_cast<size_t>(used) != region.size() || x || y || rw <= 0 || rh <= 0 ||
            static_cast<uint32_t>(rw) != w || static_cast<uint32_t>(rh) != h) {
            return false;
        }
    }
    return true;
}

bool EvoIlm::validateTarget(uint32_t surface, uint32_t layer, uint32_t w, uint32_t h, bool requireFree) {
    if (!surface || !w || !h) {
        return false;
    }
    std::string screen = get("screen", 0);
    int sw = 0, sh = 0;
    std::vector<uint32_t> ids;
    if (!parseResolution(screen, sw, sh) || static_cast<uint32_t>(sw) != w || static_cast<uint32_t>(sh) != h ||
        !parseRenderOrder(screen, ids) || std::find(ids.begin(), ids.end(), layer) == ids.end()) {
        return false;
    }
    std::string target = get("layer", layer);
    if (!validLayerGeometry(target, w, h) || !parseRenderOrder(target, ids)) {
        return false;
    }
    if (requireFree && std::find(ids.begin(), ids.end(), surface) != ids.end()) {
        return false;
    }
    std::vector<std::string> args;
    args.push_back("get");
    args.push_back("surfaces");
    std::string inventory;
    if (!exec(args, &inventory) || !parseSurfaceList(inventory, ids)) {
        return false;
    }
    bool present = std::find(ids.begin(), ids.end(), surface) != ids.end();
    return requireFree ? !present : present;
}
bool EvoIlm::screenSize(uint32_t& w, uint32_t& h) {
    int sw = 0, sh = 0;
    if (!parseResolution(get("screen", 0), sw, sh) || sw <= 0 || sh <= 0) {
        return false;
    }
    w = static_cast<uint32_t>(sw);
    h = static_cast<uint32_t>(sh);
    return true;
}

bool EvoIlm::surfaceAbsent(uint32_t surface) {
    std::vector<std::string> args;
    args.push_back("get");
    args.push_back("surfaces");
    std::string text;
    std::vector<uint32_t> ids;
    return exec(args, &text) && parseSurfaceList(text, ids) && std::find(ids.begin(), ids.end(), surface) == ids.end();
}

bool EvoIlm::surfacesOnLayer(uint32_t layerId, std::vector<uint32_t>& out) {
    return parseRenderOrder(get("layer", layerId), out);
}

bool EvoIlm::surfaceExists(uint32_t surfaceId) {
    std::string out = get("surface", surfaceId);
    return out.find("resolution") != std::string::npos || out.find("visibility") != std::string::npos;
}

bool EvoIlm::setSurfaceVisibility(uint32_t surfaceId, bool visible) {
    std::vector<std::string> a;
    a.push_back("set");
    a.push_back("surface");
    a.push_back(u32s(surfaceId));
    a.push_back("visibility");
    a.push_back(visible ? "1" : "0");
    return exec(a, nullptr);
}

bool EvoIlm::setSurfaceOpacity(uint32_t surfaceId, float opacity) {
    if (opacity < 0.f) {
        opacity = 0.f;
    }
    if (opacity > 1.f) {
        opacity = 1.f;
    }
    char v[16];
    snprintf(v, sizeof(v), "%.2f", opacity);
    std::vector<std::string> a;
    a.push_back("set");
    a.push_back("surface");
    a.push_back(u32s(surfaceId));
    a.push_back("opacity");
    a.push_back(v);
    return exec(a, nullptr);
}

bool EvoIlm::attachSurface(uint32_t surfaceId, uint32_t layerId, uint32_t w, uint32_t h) {
    bool topFirst = false;
    if (!renderOrderTopFirst(topFirst)) {
        return false;  // Before any "set": a picture hidden behind the HMI is worse than no picture.
    }
    OrderLock lock;
    std::vector<uint32_t> order;
    if (!surfacesOnLayer(layerId, order)) {
        return false;  // Never replace an unreadable order with our surface alone.
    }
    if (!setSurfaceVisibility(surfaceId, false)) {
        return false;
    }
    // Source and destination regions: the whole layer.
    if (!setRegion(surfaceId, "source", 0, 0, w, h) || !setRegion(surfaceId, "destination", 0, 0, w, h)) {
        return false;
    }
    // Layer render order: the existing list is kept and our surface goes above the others.
    // With genivi-flip-renderorder=1 (the car's setting) the first entry is on top; otherwise the last one is.
    bool present = false;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == surfaceId) {
            present = true;
        }
    }
    if (!present) {
        if (topFirst) {
            order.insert(order.begin(), surfaceId);
        } else {
            order.push_back(surfaceId);
        }
    }
    ELOG("ILM", "compositor genivi-flip-renderorder=%d: surface 0x%x placed %s of layer 0x%x (above iDrive)",
         topFirst ? 1 : 0, surfaceId, topFirst ? "at the head of the list" : "at the end of the list", layerId);
    return setRenderOrder(layerId, order) && setSurfaceVisibility(surfaceId, false);
}

bool EvoIlm::setRegion(uint32_t surfaceId, const char* which, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    char v[4][16];
    snprintf(v[0], sizeof(v[0]), "%u", x);
    snprintf(v[1], sizeof(v[1]), "%u", y);
    snprintf(v[2], sizeof(v[2]), "%u", w);
    snprintf(v[3], sizeof(v[3]), "%u", h);
    std::vector<std::string> a;
    a.push_back("set");
    a.push_back("surface");
    a.push_back(u32s(surfaceId));
    a.push_back(which);
    a.push_back("region");
    for (int i = 0; i < 4; ++i) {
        a.push_back(v[i]);
    }
    return exec(a, nullptr);
}

bool EvoIlm::attachOverlay(uint32_t surfaceId, uint32_t layerId, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    bool topFirst = false;
    if (!surfaceId || !w || !h || w > 1920 || h > 1080 || x > 1920 || y > 1080 || !renderOrderTopFirst(topFirst)) {
        return false;
    }
    OrderLock lock;
    std::vector<uint32_t> order;
    if (!surfacesOnLayer(layerId, order)) {
        return false;
    }
    if (!setSurfaceVisibility(surfaceId, false) || !setRegion(surfaceId, "source", 0, 0, w, h) ||
        !setRegion(surfaceId, "destination", x, y, w, h)) {
        return false;
    }
    return raiseLocked(surfaceId, layerId, topFirst);
}

bool EvoIlm::raiseSurface(uint32_t surfaceId, uint32_t layerId) {
    bool topFirst = false;
    if (!renderOrderTopFirst(topFirst)) {
        return false;
    }
    OrderLock lock;
    return raiseLocked(surfaceId, layerId, topFirst);
}

bool EvoIlm::raiseLocked(uint32_t surfaceId, uint32_t layerId, bool topFirst) {
    std::vector<uint32_t> order;
    if (!surfacesOnLayer(layerId, order)) {
        return false;
    }
    if (!order.empty() && (topFirst ? order.front() : order.back()) == surfaceId) {
        return true;  // already on top: no command
    }
    order.erase(std::remove(order.begin(), order.end(), surfaceId), order.end());
    if (topFirst) {
        order.insert(order.begin(), surfaceId);
    } else {
        order.push_back(surfaceId);
    }
    return setRenderOrder(layerId, order);
}

bool EvoIlm::setRenderOrder(uint32_t layerId, const std::vector<uint32_t>& order) {
    // The car tool's Expression::getUintArray expects ONE comma-separated token (empty list: no token).
    // Checked against the car's own tool under QEMU, with only its ILM calls stubbed.
    std::vector<std::string> a;
    a.push_back("set");
    a.push_back("layer");
    a.push_back(u32s(layerId));
    a.push_back("render");
    a.push_back("order");
    std::string list;
    for (size_t i = 0; i < order.size(); ++i) {
        if (i) {
            list += ',';
        }
        list += u32s(order[i]);
    }
    if (!list.empty()) {
        a.push_back(list);
    }
    if (!exec(a, nullptr)) {
        return false;
    }
    std::vector<uint32_t> actual;
    if (!surfacesOnLayer(layerId, actual) || actual != order) {
        ELOG("ILM", "surface order not confirmed after the command");
        return false;
    }
    ELOG("ILM", "surface order read back and confirmed (%u surfaces)", static_cast<unsigned>(order.size()));
    return true;
}

bool EvoIlm::detachSurface(uint32_t surfaceId, uint32_t layerId) {
    OrderLock lock;
    std::vector<uint32_t> order;
    if (!surfacesOnLayer(layerId, order)) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == surfaceId) {
            found = true;
        }
    }
    if (!found) {
        return true;
    }
    order.erase(std::remove(order.begin(), order.end(), surfaceId), order.end());
    return setRenderOrder(layerId, order);
}

}  // namespace evo
