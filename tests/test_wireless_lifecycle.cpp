// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The service's wireless loop (wireless_loop) and app commands (service_app), with a fake Alps link and handshake.
// The AA session runs on a socketpair. The peeks' timing on its own: tests/test_peek_timing.cpp.
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

#include "alps_spp.hpp"
#include "evo_ap.hpp"
#include "evo_log.hpp"
#include "service_app.hpp"
#include "service_contract.hpp"
#include "wireless_setup.hpp"

static std::atomic<int> starts(0), waits(0), listens(0), serves(0), prepared(0), sppDrops(0);
static std::atomic<bool> fixtureLink(true), borrowed(false);
static std::atomic<bool> phoneKeepsLink(false);  // the phone keeps its Bluetooth link after accepting the Wi-Fi
static evo::ServiceApp* instance = 0;
static volatile sig_atomic_t quitFlag = 0;
namespace evo {
AlpsSppLink::AlpsSppLink() : p_(0) {}
AlpsSppLink::~AlpsSppLink() {}
void AlpsSppLink::stop() {}
bool AlpsSppLink::running() {
    return true;
}
bool AlpsSppLink::linkUp() {
    return fixtureLink;
}
int AlpsSppLink::waitLink(int) {
    ++waits;
    bool expected = false;
    if (fixtureLink && borrowed.compare_exchange_strong(expected, true)) {
        return 42;
    }
    usleep(10000);
    return -1;
}
void AlpsSppLink::closeLink() {
    fixtureLink = false;
    borrowed = false;
}
// Asked of the Bluetooth stack: only that makes the phone see its link close (closeLink() closes our end).
bool AlpsSppLink::requestSppDisconnect() {
    ++sppDrops;
    return true;
}
// The access point is made ready (no U-APSD) once before every handshake, never during one.
UapsdOffer withdrawAccessPointUapsd() {
    ++prepared;
    return UapsdOffer::kWithdrawn;
}
}  // namespace evo
namespace aa {
WirelessSetup::WirelessSetup(int fd, const ApInfo& ap, uint16_t port, int peekMs)
    : fd_(fd), ap_(ap), port_(port), peekMs_(peekMs), foreign_(false), spoke_(true), heard_(0) {}
bool WirelessSetup::handshake(int) {
    assert(fd_ == 42 && peekMs_ == 300 && port_ == 50000);
    assert(prepared == starts + 1);
    pthread_mutex_lock(&instance->mu);
    instance->tcpPreparing = true;
    pthread_mutex_unlock(&instance->mu);
    if (!phoneKeepsLink) {
        fixtureLink = false;
    }
    ++starts;
    return true;
}
bool WirelessSetup::serve(int) {
    if (!phoneKeepsLink) {
        return false;
    }
    ++serves;
    usleep(10000);
    return fixtureLink;
}
bool WirelessSetup::listen(int) {
    ++listens;
    usleep(10000);
    return true;
}
}  // namespace aa
static void preparing(bool value) {
    pthread_mutex_lock(&instance->mu);
    instance->tcpPreparing = value;
    pthread_mutex_unlock(&instance->mu);
}
static void awaitStarts(int count) {
    for (unsigned i = 0; i < 200 && starts < count; ++i) {
        usleep(10000);
    }
    assert(starts == count);
}
static void awaitServes() {
    const int before = serves;
    for (unsigned i = 0; i < 200 && serves < before + 3; ++i) {
        usleep(10000);
    }
    assert(serves >= before + 3);
}
static bool linkClosedWithin(unsigned ms) {
    for (unsigned i = 0; i < ms / 10 && fixtureLink; ++i) {
        usleep(10000);
    }
    return !fixtureLink;
}
// A session on a socketpair that ends by itself, without ByeBye: the phone's side of the connection closes.
static void loseSession(evo::ServiceApp& app) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(app.session.start(pair[0]) && app.session.isActive());
    preparing(false);  // the connection is set up: the session alone keeps the projection busy
    usleep(50000);
    assert(fixtureLink);  // a running session leaves the link alone
    close(pair[1]);
    for (unsigned i = 0; i < 200 && app.session.isActive(); ++i) {
        usleep(10000);
    }
    assert(!app.session.isActive());
}

static void mediaClicks(evo::ServiceApp& app) {
    const unsigned states[] = {0, 1, 0, 1, 0};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        evo::ServiceApp::mediaKey(states[i], &app);
    }
}

static void mediaRecall(evo::ServiceApp& app) {
    const char* request = evo::contract::kMediaRecallFile;
    unlink(request);
    quitFlag = 0;
    app.ready = app.videoEnabled = true;
    app.focus = false;
    mediaClicks(app);
    assert(!app.focus && evo::takeMediaRecall(request, static_cast<int>(getpid()), evo::monoMs()));
    assert(!evo::takeMediaRecall(request, static_cast<int>(getpid()), evo::monoMs()));
    // Every gate is checked while the real socketpair session is active.
    bool* gates[] = {&app.focus, &app.asking, &app.reverse, &app.inCall, &app.stopping};
    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); ++i) {
        *gates[i] = true;
        mediaClicks(app);
        assert(access(request, F_OK) != 0);
        *gates[i] = false;
    }
    app.ready = false;
    mediaClicks(app);
    assert(access(request, F_OK) != 0);
    app.ready = true;
    quitFlag = 1;
    mediaClicks(app);
    assert(access(request, F_OK) != 0);
    quitFlag = 0;
    const char* verbs[] = {"call", "gear", "hide", "show", "quit"};
    for (unsigned i = 0; i < sizeof(verbs) / sizeof(verbs[0]); ++i) {
        app.focus = app.reverse = app.inCall = false;
        quitFlag = 0;
        mediaClicks(app);
        assert(access(request, F_OK) == 0);
        evo::ControlCommand command;
        command.verb = verbs[i];
        command.arg = i == 1 ? 2 : 1;
        command.hasArg = i < 2;
        evo::ServiceApp::command(command, &app);
        assert(access(request, F_OK) != 0);  // a queued request cannot outlive a guard or focus change
    }
    app.focus = app.reverse = app.inCall = false;
    quitFlag = 0;
    evo::ServiceApp::mediaKey(0, &app);
    evo::ServiceApp::mediaKey(1, &app);
    evo::ServiceApp::mediaKey(0, &app);
    evo::ControlCommand call;
    call.verb = "call";
    call.hasArg = true;
    call.arg = 1;
    evo::ServiceApp::command(call, &app);
    call.arg = 0;
    evo::ServiceApp::command(call, &app);
    evo::ServiceApp::mediaKey(1, &app);
    evo::ServiceApp::mediaKey(0, &app);
    assert(access(request, F_OK) != 0);  // two clicks straddling a call never pair
    app.mediaPress.cancel();
    app.videoEnabled = app.ready = false;
    app.focus = true;
    puts(
        "PASS service double MEDIA: live session, guarded request only, focus unchanged, queued and partial gestures "
        "cancelled");
}
// One main-loop turn of the peeks, Android Auto on screen or not (no video in this test).
static void onScreen(evo::ServiceApp& app, bool on, uint32_t later = 0) {
    app.peek.turn(evo::monoMs() + later, on);
}
int main() {
    evo::ServiceApp app(50000, false, &quitFlag);
    instance = &app;
    {  // iDrive popup: picture aside with a time limit, back as soon as it closes, nothing when not on screen
        evo::ControlCommand on, off;
        on.verb = off.verb = "popup";
        on.hasArg = off.hasArg = true;
        on.arg = 1;
        off.arg = 0;
        onScreen(app, false);
        evo::ServiceApp::command(on, &app);
        assert(!app.peek.hideUntil() && !app.peek.popupOpen());
        onScreen(app, true);
        evo::ServiceApp::command(on, &app);
        assert(app.peek.hideUntil() && app.peek.popupOpen());
        assert(static_cast<int32_t>(app.peek.hideUntil() - evo::monoMs()) <= 120000);  // until popup 0, 2 min at most
        evo::ServiceApp::command(off, &app);
        assert(!app.peek.hideFrom() && !app.peek.hideUntil() && !app.peek.popupOpen());
        evo::ServiceApp::command(off, &app);
        assert(!app.peek.hideUntil());
        onScreen(app, false);
    }
    {  // Volume: dimmed until the car reports a level; the pill only once it is ready, never over iDrive
        evo::ServiceApp::volumeEvent(1, &app);
        assert(!app.peek.dimUntil() && !app.peek.hideUntil());  // Android Auto not on screen: nothing
        onScreen(app, true);
        evo::ServiceApp::volumeEvent(1, &app);
        assert(app.peek.dimUntil() && static_cast<int32_t>(app.peek.dimUntil() - evo::monoMs()) <= 2500);
        onScreen(app, true, 3000);  // the dim ran out
        assert(!app.peek.dimUntil());
        evo::VolumeLevel level = {12, 0, 40};
        evo::ServiceApp::volumeLevel(level, &app);
        assert(app.peek.levelSeen());
        evo::ServiceApp::volumeEvent(1, &app);
        assert(app.peek.dimUntil());  // the pill never started here: still dimmed
        onScreen(app, true, 3000);
        app.peek.refuseOpacity(evo::monoMs(), evo::monoMs());  // compositor refused the opacity: stepped aside instead
        onScreen(app, true, 3000);                             // that step aside ran out
        assert(app.peek.dimRefused() && !app.peek.hideUntil());
        evo::ServiceApp::volumeEvent(1, &app);
        assert(!app.peek.dimUntil() && app.peek.hideUntil());
        onScreen(app, false);
    }
    {  // Hard keys iDrive acts on: the picture steps aside, the phone is told NATIVE
        onScreen(app, true);
        evo::ServiceApp::hardKey("NAV", &app);  // an Android Auto app: not this callback's business
        assert(app.focus && !app.pendingFocus);
        evo::ServiceApp::hardKey("MENU", &app);
        assert(!app.focus && app.pendingFocus == 2);
        app.focus = true;
        app.pendingFocus = 0;
        onScreen(app, false);
    }
    {  // "await": the next session starts aside and says so, until "show"; "hide" ends the wait too
        evo::ControlCommand await, show, hide;
        await.verb = "await";
        show.verb = "show";
        hide.verb = "hide";
        await.hasArg = show.hasArg = hide.hasArg = false;
        await.arg = show.arg = hide.arg = 0;
        assert(!app.awaitShow && !app.asking);
        evo::ServiceApp::command(await, &app);
        assert(app.awaitShow && !app.asking && app.focus);  // nothing changes for a session already there
        app.focus = false;  // as startProjection leaves it for a phone that connects now
        app.asking = true;
        evo::ServiceApp::command(show, &app);
        assert(app.focus && !app.asking && app.pendingFocus == 1 && app.awaitShow);
        app.focus = false;
        app.asking = true;
        evo::ServiceApp::command(hide, &app);
        assert(!app.focus && !app.asking && app.pendingFocus == 2);
        app.focus = true;
        app.pendingFocus = 0;
        app.awaitShow = false;  // the rest of this test: a service that shows by itself
    }
    {  // quit from the iDrive app reaches the main loop's flag
        evo::ControlCommand quit;
        quit.verb = "quit";
        quit.hasArg = false;
        quit.arg = 0;
        evo::ServiceApp::command(quit, &app);
        assert(quitFlag == 1);
    }
    preparing(true);
    pthread_t thread;
    assert(pthread_create(&thread, 0, evo::ServiceApp::wireless, &app) == 0);
    usleep(250000);
    assert(starts == 0 && listens > 0);  // the link is listened to, no handshake goes into it
    preparing(false);
    awaitStarts(1);
    int peers[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, peers) == 0);
    assert(app.session.start(peers[0]) && app.session.isActive());
    mediaRecall(app);
    preparing(false);
    fixtureLink = true;  // phone reopens SPP while TCP projection is active
    const int listened = listens;
    usleep(350000);
    assert(starts == 1 && app.session.isActive() && listens > listened);
    app.session.stop();
    app.ready = app.videoEnabled = true;
    app.focus = false;
    mediaClicks(app);
    assert(access(evo::contract::kMediaRecallFile, F_OK) != 0);
    app.ready = app.videoEnabled = false;
    close(peers[1]);
    awaitStarts(2);  // rearming is allowed only after the real session ended
    {  // A phone that keeps its Bluetooth link after accepting (MINI F56, Galaxy Z Fold): a lost session closes the
       // link, the phone opens it again and gets a new handshake. A session that is not the wireless loop's (USB) or
       // that ended with ByeBye (tests/test_session_phone.cpp) leaves the link alone.
        app.session.setDisconnectCallback(evo::ServiceApp::ended, &app);
        phoneKeepsLink = true;
        preparing(false);
        fixtureLink = true;  // the phone opens the link
        awaitStarts(3);
        awaitServes();
        assert(fixtureLink && app.lostSessions() == 0 && sppDrops == 0);
        loseSession(app);
        assert(app.lostSessions() == 1 && linkClosedWithin(2000) && sppDrops == 1);
        fixtureLink = true;  // the phone opens it again at once
        awaitStarts(4);
        awaitServes();
        pthread_mutex_lock(&app.mu);
        app.projectionOverUsb = true;
        pthread_mutex_unlock(&app.mu);
        loseSession(app);
        assert(app.lostSessions() == 1 && !linkClosedWithin(300) && starts == 4 && sppDrops == 1);
        pthread_mutex_lock(&app.mu);
        app.projectionOverUsb = false;
        pthread_mutex_unlock(&app.mu);
        phoneKeepsLink = false;
    }
    app.requestStop();
    pthread_join(thread, 0);
    pthread_mutex_lock(&app.mu);
    assert(app.wirelessDone && app.sppLinks == 4 && app.wppReply && app.wppAccepted);
    pthread_mutex_unlock(&app.mu);
    puts(
        "PASS actual wireless loop: no handshake during TCP preparation or active session, rearmed after session stop, "
        "Bluetooth link closed after a lost wireless session; app commands and volume through the peeks; PC socketpair "
        "only");
}
