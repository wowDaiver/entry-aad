// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Attaches, shows and stacks our surfaces through the IVI LayerManager (ILM) command-line tool.

// Every action runs the car's /usr/bin/LayerManagerControl with posix_spawnp (no shell) and a bounded delay.
// The commands it accepts:
//   get screen|layer|surface <id>
//   set layer|surface <id> visibility <0|1>
//   set layer|surface <id> destination region <x> <y> <w> <h>
//   set layer|surface <id> source region <x> <y> <w> <h>
//   set screen|layer <id> render order [<idarray>]      (replaces the WHOLE list)
//   set surface <id> accept <0|1> input events from devices <kbd:pointer:touch>
// The HMI's logical layer ids (Lua) are NOT the compositor's live ids. Observed on the car: visible layers 0x80000000
// (full screen) and 0x80020000 (partial region). The current state is always read back before a change; no BMW layer
// or surface is reserved for Android Auto.
// Safety rule: the screen's layer order is never changed; we only add OUR surface to an existing layer and keep the
// surfaces already there.
// Stacking direction: winston loads /etc/wayland/compositor.conf, which sets genivi-flip-renderorder = 1. The
// wlxs-genivi plugin then reverses the z step (-0x100 instead of +0x100): the FIRST entry of a layer's render order is
// on TOP. Observed on the car: a surface appended last showed the video behind the HMI (surface 0x0). The setting is
// read again before every attach.
#ifndef ENTRY_AAD_PLATFORM_EVO_ILM_HPP
#define ENTRY_AAD_PLATFORM_EVO_ILM_HPP

#include <stdint.h>

#include <string>
#include <vector>

namespace evo {

class EvoIlm {
public:
    EvoIlm();  // the car's LayerManagerControl and compositor configuration
    // Host tests: a stand-in for the CLI and another compositor configuration.
    EvoIlm(const std::string& ctl, const std::string& compositorConf);

    // Attaches surfaceId to layerId hidden, with regions 0,0,w,h. The layer's list is kept and our surface goes above
    // the others: first when the compositor flips the order, last otherwise. Refuses before any "set" command when the
    // stacking direction cannot be established.
    bool attachSurface(uint32_t surfaceId, uint32_t layerId, uint32_t w, uint32_t h);
    // Reads genivi-flip-renderorder in the compositor configuration. topFirst=true: the first entry of a render order
    // is on top. False when the file is unreadable or ambiguous.
    bool renderOrderTopFirst(bool& topFirst);
    // Our overlay (volume pill) above every surface of the layer, Android Auto included: its whole w x h buffer shown
    // at x, y, unscaled, hidden until setSurfaceVisibility. Same refusals as attachSurface.
    bool attachOverlay(uint32_t surfaceId, uint32_t layerId, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    // Puts surfaceId back on top of the layer if another surface went above it (Android Auto at each new session).
    // The other surfaces keep their relative order. True when it is on top afterwards.
    bool raiseSurface(uint32_t surfaceId, uint32_t layerId);
    bool setSurfaceVisibility(uint32_t surfaceId, bool visible);
    bool setSurfaceOpacity(uint32_t surfaceId, float opacity);  // "set surface <id> opacity <0..1>"
    // Removes our surface from the layer, the other surfaces keeping their order. Called on stop.
    bool detachSurface(uint32_t surfaceId, uint32_t layerId);

    // Read-only.
    bool surfacesOnLayer(uint32_t layerId, std::vector<uint32_t>& out);
    bool surfaceExists(uint32_t surfaceId);
    // Fail closed on an unreadable inventory, occupied ID, hidden/off-screen or non-fullscreen layer.
    bool validateTarget(uint32_t surfaceId, uint32_t layerId, uint32_t w, uint32_t h, bool requireFree = true);
    bool surfaceAbsent(uint32_t surfaceId);
    // Raw output of a "get ..." command.
    std::string get(const char* what, uint32_t id);
    // The size of screen 0, the panel. False when the compositor does not give it.
    bool screenSize(uint32_t& w, uint32_t& h);

    // Pure parsers.
    static bool parseRenderOrder(const std::string& txt, std::vector<uint32_t>& out);
    static bool parseResolution(const std::string& txt, int& w, int& h);
    static bool parseSurfaceList(const std::string& txt, std::vector<uint32_t>& out);
    static bool validLayerGeometry(const std::string& txt, uint32_t w, uint32_t h);
    // Key absent: false (standard GENIVI order). Unknown value or duplicated key: failure.
    static bool parseRenderOrderFlip(const std::string& conf, bool& topFirst);

private:
    bool exec(const std::vector<std::string>& args, std::string* out);
    bool setRenderOrder(uint32_t layerId, const std::vector<uint32_t>& order);
    bool setRegion(uint32_t surfaceId, const char* which, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    bool raiseLocked(uint32_t surfaceId, uint32_t layerId, bool topFirst);
    std::string ctl_;
    std::string compositorConf_;
};

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_EVO_ILM_HPP
