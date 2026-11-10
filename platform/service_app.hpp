// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The entry-aad service's state and callbacks: phone session, video, inputs, iDrive app commands, picture peeks.

// platform/main.cpp builds one ServiceApp, wires its callbacks to the components and runs the main loop, which calls
// turn() every 100 ms. Threads: the main loop, the TCP accept thread (connected), the session thread (frames, focus,
// microphone, navigation, ended), the vMOST thread (keys, volume), the control thread (command) and the wireless loop.
#ifndef ENTRY_AAD_PLATFORM_SERVICE_APP_HPP
#define ENTRY_AAD_PLATFORM_SERVICE_APP_HPP

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "aa_session.hpp"
#include "alps_spp.hpp"
#include "evo_control.hpp"
#include "evo_knob.hpp"
#include "evo_mic.hpp"
#include "evo_pill.hpp"
#include "evo_video.hpp"
#include "kombi_nav.hpp"
#include "peek_timing.hpp"
#include "tcp_server.hpp"
#include "wireless_loop.hpp"
#include "wireless_setup.hpp"

namespace evo {

class SppSnapshotReader;

class ServiceApp : public WirelessHost {
public:
    // What one turn of the main loop decided, for the inputs, the iDrive app and the loop itself.
    struct Turn {
        bool failed;          // the wireless loop has ended: the service stops
        bool active;          // an Android Auto session runs
        bool onScreen;        // what the iDrive app is told (peeks included)
        bool shown;           // the picture is on screen, not stepped aside
        bool pictureUp;       // the compositor shows our surface
        bool pictureStarted;  // the first picture has arrived
        int focusMsg;         // 1 tell the phone FOCUSED, 2 NATIVE, 0 nothing (after the inputs took their turn)
        bool idleForAdvert;   // nothing connected or being prepared: the advertisement may be refreshed
    };

    // port: the TCP port told to the phone. quit: the main loop's flag, set by the "quit" command.
    ServiceApp(uint16_t port, bool videoEnabled, volatile sig_atomic_t* quit);
    ~ServiceApp();

    // ---- main loop ----
    void markReady();    // callbacks may now start a projection
    void requestStop();  // the wireless loop and new connections stop
    // Session, focus and peeks into what shows: the video layer, its opacity, the pill. Holds videoMu while
    // LayerManagerControl runs. service: started by the iDrive app (aa-supervisor --service).
    Turn turn(bool service);
    // Registers the Android Auto UUID again while nothing is connected (refreshAdvertisement). False when that
    // failed and the service must stop.
    bool refreshAdvertisementIfIdle(bool service, SppSnapshotReader& reader);
    // Car day/night (CDS vehicle.dayNightChangeOver via the iDrive app): 1 day, 2 night, 0 no change, 3 invalid.
    void applyDayNight(int v, const char* src);

    // ---- callbacks, user = this ----
    static void frame(uint32_t, const uint8_t* data, size_t len, uint64_t pts, void* user);
    static void codec(const uint8_t* data, size_t len, void* user);
    static void touchEvent(const aa::TouchEventOut& ev, void* user);
    static void volumeEvent(unsigned steps, void* user);
    static void volumeLevel(const VolumeLevel& level, void* user);
    static void hardKey(const char* name, void* user);
    static void knobEvent(const aa::CcsEvent& ev, void* user);
    static void appKey(const char* name, void* user);
    static void command(const ControlCommand& c, void* user);
    static void videoFocus(bool projected, uint32_t reason, void* user);
    static void micRequest(bool open, void* user);
    static void micData(const uint8_t* pcm, size_t len, void* user);
    static void navStatus(uint16_t msgId, const uint8_t* body, size_t len, void* user);
    static void ended(void* user);
    static void connected(int fd, void* user);
    static void connectedTransport(aa::Transport* transport, void* user);
    static void* wireless(void* user);  // thread: runWirelessLoop, then the failure the main loop sees

    // ---- WirelessHost ----
    bool stopRequested() override;
    bool projectionBusy() override;
    void sppLinkOpened() override;
    void handshakeEnded(bool spoke, bool accepted) override;

    // ---- components, wired by main() ----
    EvoKnob* knob;  // phone keys during a call; set by main() before the control thread starts
    AlpsSppLink bt;
    EvoVideo video;
    aa::AASession session;
    EvoMic mic;                // car mic for the Assistant (stopped before the session it posts to)
    evo::kombi::Arrows kombi;  // AA turn-by-turn -> HUD/cluster arrows, SOME/IP 0xB006 to the KOMBI
    aa::TcpServer server;
    aa::ApInfo ap;
    EvoVideoConfig videoConfig;
    uint16_t port;
    bool videoEnabled;

    // ---- state ----
    // mu: state flags only, held briefly (the vMOST, session and control threads take it). videoMu: the picture's
    // LayerManagerControl runs and video restarts, which can last seconds; only the main loop and connected() take it,
    // always after connectMu and never while holding mu.
    pthread_mutex_t mu, connectMu, videoMu;
    bool stopping, wirelessDone, focus, ready, tcpPreparing;
    bool reverse;      // service: reverse gear from the iDrive app hides the picture (camera)
    int pendingFocus;  // service: 1 tell the phone FOCUSED, 2 NATIVE (sent from the main loop)
    PeekTiming peek;   // under mu
    EvoPill pill;
    unsigned sppLinks, tcpLinks, usbLinks;
    std::atomic<unsigned> videoFrames;  // counted on the session thread without mu
    bool wppReply, wppAccepted;

private:
    volatile sig_atomic_t* quit_;
};

}  // namespace evo

#endif
