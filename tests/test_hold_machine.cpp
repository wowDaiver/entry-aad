// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// hmiapp/hold_machine.cpp: which iDrive event and which control message, scenario by scenario, against a recorder.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../hmiapp/hold_machine.hpp"
#include "../hmiapp/rhmi_ids.hpp"

namespace {

using evo::app::HoldIo;
using evo::app::HoldMachine;
using evo::app::kAaState;
using evo::app::kHelpState;
using evo::app::kMenuState;
using evo::app::ServiceState;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

// What the machine did since the last check, in order: "event 52", control messages as sent, "app key removed".
#define CHECK_OUT(car, expected)                                                                               \
    do {                                                                                                       \
        const std::string got_ = (car).take();                                                                 \
        if (got_ != (expected)) {                                                                              \
            printf("FAIL %s:%d: did \"%s\", expected \"%s\"\n", __FILE__, __LINE__, got_.c_str(), (expected)); \
            ++g_failures;                                                                                      \
        }                                                                                                      \
    } while (0)

class Recorder : public HoldIo {
public:
    Recorder() : appKey(false), appKeyAt(0), now(nullptr), reads(0) {}
    void triggerEvent(unsigned event) override {
        char text[16];
        snprintf(text, sizeof(text), "event %u", event);
        out.push_back(text);
    }
    void ownPopup(bool open) override { out.push_back(open ? "own popup 1" : "own popup 0"); }
    void send(const char* message) override { out.push_back(message); }
    void log(const char* line) override { logs.push_back(line); }
    ServiceState readService() override {
        ++reads;
        return service;
    }
    bool appKeyAge(long* seconds) override {
        if (!appKey) {
            return false;
        }
        *seconds = *now - appKeyAt;
        return true;
    }
    void removeAppKey() override {
        appKey = false;
        out.push_back("app key removed");
    }

    std::vector<std::string> out;
    std::vector<std::string> logs;
    ServiceState service;
    bool appKey;
    long appKeyAt;    // when the service wrote the file, on the car's clock below
    const long* now;  // that clock
    int reads;
};

// The machine in the car: its clocks, the service, iDrive's notifications.
struct Car {
    Recorder io;
    HoldMachine hold;
    long now;  // seconds since the head unit started
    long ms;   // the same clock, milliseconds

    Car() : hold(io), now(1000), ms(50000) { io.now = &now; }

    // The status poll, a second after the previous one, with the service as it is now.
    void poll() {
        now += 1;
        ms += 1000;
        hold.tellService(io.service);
        hold.poll(io.service, now);
    }
    void polls(int n) {
        for (int i = 0; i < n; ++i) {
            poll();
        }
    }
    void service(const char* state, int pid, int overlay) { io.service = ServiceState(state, pid, overlay); }
    void overlay(int on) { io.service.overlay = on; }

    // iDrive's notifications.
    void menu(bool focused) { hold.menuFocus(kMenuState, focused, now, ms); }
    void blankFocus(bool focused) { hold.aaFocus(focused); }
    void blankVisible(bool visible) { hold.aaVisible(visible, now); }
    // iDrive on the blank state: the menu loses the focus, the blank state takes it and shows.
    void blankShown() {
        menu(false);
        blankFocus(true);
        blankVisible(true);
    }
    // iDrive on one of its own screens: the blank state loses the focus and hides.
    void blankLeft() {
        blankFocus(false);
        blankVisible(false);
    }
    void call(bool on) { hold.callChanged(on, now); }
    bool gear(int value) { return hold.gearChanged(value, now); }

    std::string take() {
        std::string all;
        for (size_t i = 0; i < io.out.size(); ++i) {
            all += (i ? ", " : "") + io.out[i];
        }
        io.out.clear();
        return all;
    }
    bool logged(const char* text) const {
        for (size_t i = 0; i < io.logs.size(); ++i) {
            if (strstr(io.logs[i].c_str(), text)) {
                return true;
            }
        }
        return false;
    }
};

// A connected phone, gear 1, nothing on screen yet; the service already told the call and the gear.
void connected(Car& c) {
    c.gear(1);
    c.service("connected", 42, 0);
    c.poll();
    CHECK_OUT(c, "await, call 0, gear 1");
}

// Android Auto on screen over the blank state, which has the focus: opened from the menu's Open row.
void heldOnBlank(Car& c) {
    connected(c);
    c.menu(true);
    c.hold.showAndHold("Open row", c.now);
    c.blankShown();
    c.overlay(1);
    c.poll();
    CHECK_OUT(c, "show, event 52");
    CHECK(c.hold.holding() && c.hold.blankShown());
    c.io.logs.clear();
}

void manualOpen() {
    Car c;
    connected(c);
    c.menu(true);
    CHECK_OUT(c, "");  // the menu's focus alone does nothing while nothing is held
    CHECK(c.io.reads == 0);
    c.hold.showAndHold("Open row", c.now);
    CHECK_OUT(c, "show, event 52");  // at once, not at the next poll
    CHECK(c.logged("hold: event 52 -> state 13 (Open row, try 1)"));
    c.blankShown();
    CHECK_OUT(c, "");
    c.overlay(1);
    c.polls(10);
    CHECK_OUT(c, "");  // held: nothing more to do
    CHECK(c.hold.holding() && c.hold.blankShown());
}

void openButNeverCame() {
    Car c;
    connected(c);
    c.menu(true);
    c.hold.showAndHold("entry button", c.now);
    c.blankShown();
    CHECK_OUT(c, "show, event 52");
    c.polls(2);
    CHECK_OUT(c, "");  // the session dropped meanwhile: no overlay, wait 3 s
    c.poll();
    CHECK_OUT(c, "event 51");  // the menu back instead of a blank screen
    CHECK(!c.hold.holding());
    c.polls(3);
    CHECK_OUT(c, "");
}

void automaticStartOverIdriveScreen() {
    Car c;
    connected(c);  // iDrive shows one of its own screens: none of our states
    c.overlay(1);  // Android Auto came up by itself
    c.poll();
    CHECK_OUT(c, "event 51");  // our menu first: event 52 can be refused over iDrive's screens
    CHECK(c.logged("Android Auto came on screen over an iDrive screen, try 1"));
    c.menu(true);
    CHECK_OUT(c, "event 52");  // the menu's focus asks for the blank state at once
    CHECK(c.io.reads == 1);
    c.blankShown();
    c.polls(5);
    CHECK_OUT(c, "");
    CHECK(c.hold.holding() && c.hold.blankShown());
}

void automaticStartRefused() {
    {  // the menu never opens: a menu try, our popup and event 52 under it, two more menu tries a second apart,
       // event 52 once, then Android Auto stays unheld
        Car c;
        connected(c);
        c.overlay(1);
        c.poll();
        CHECK_OUT(c, "event 51");
        c.poll();
        CHECK_OUT(c, "own popup 1");
        CHECK(c.logged("iDrive refused our menu over its screen: our own popup opened (event 53)"));
        c.poll();
        CHECK_OUT(c, "event 52, own popup 0");
        CHECK(c.logged("asked under our popup over an iDrive screen, popup closed"));
        c.poll();
        CHECK_OUT(c, "event 51");
        c.poll();
        CHECK_OUT(c, "event 51");
        c.poll();
        CHECK_OUT(c, "event 52");
        CHECK(c.logged("our menu did not open: state directly"));
        c.poll();
        CHECK_OUT(c, "");  // Android Auto stays on screen: iDrive may refuse until a key is pressed
        CHECK(c.hold.holding());
        CHECK(c.logged("even through our menu: Android Auto stays, without the hold"));
        c.polls(3);
        CHECK_OUT(c, "event 51, event 51, event 51");  // our menu, every second
        c.menu(true);                                  // iDrive performs our events again
        CHECK_OUT(c, "event 52");
        c.blankShown();
        c.polls(20);
        CHECK_OUT(c, "");
        CHECK(c.logged("Android Auto is held"));
    }
    {  // the menu never opens, event 52 under our popup works; once the blank state has the focus, the tries start
       // over
        Car c;
        connected(c);
        c.overlay(1);
        c.polls(3);
        CHECK_OUT(c, "event 51, own popup 1, event 52, own popup 0");
        c.blankFocus(true);  // the focus, without a visibility notification
        c.polls(5);
        CHECK_OUT(c, "");
        CHECK(c.hold.holding());
        c.io.appKey = true;  // later, NAV: event 52, refused over iDrive's navigation
        c.io.appKeyAt = c.now;
        c.blankLeft();
        CHECK_OUT(c, "app key removed, event 52");
        c.poll();
        CHECK_OUT(c, "event 51");  // through our menu again, not given up
    }
    {  // the menu opens but refuses the blank state: event 52 at each poll, 8 at most
        Car c;
        connected(c);
        c.overlay(1);
        c.poll();
        CHECK_OUT(c, "event 51");
        c.menu(true);
        CHECK_OUT(c, "event 52");
        for (int i = 0; i < 7; ++i) {
            c.poll();
            CHECK_OUT(c, "event 52");
        }
        CHECK(c.logged("BACK returned to our menu, try 8"));
        c.polls(3);
        CHECK_OUT(c, "");
        CHECK(c.logged("hold: not entering state 13 (BACK returned to our menu): already tried 8 times"));
        CHECK(c.hold.holding());  // Android Auto stays: the menu is ours, not an iDrive screen
        c.menu(false);
        c.blankVisible(true);  // the blank state shows at last, without a focus notification: the tries start over
        c.ms += 1000;
        c.hold.menuFocus(kHelpState, true, c.now, c.ms);  // a stray push opened Help
        CHECK_OUT(c, "event 52");
    }
}

// iDrive performs no focus event of an app while one of its own popups is on top (seen on the car, opened but not
// started: refused until a key was pressed). The service keeps Android Auto aside when a phone connects ("await")
// while our menu is asked for; after 3 refusals Android Auto shows without the hold, and the asking goes on.
void waitsAsideForIdrive() {
    {  // nothing in the way: our menu opens at the first request, Android Auto comes up on the blank state
        Car c;
        connected(c);
        c.overlay(2);  // a phone has connected: Android Auto aside, waiting for our "show"
        c.poll();
        CHECK_OUT(c, "event 51");
        CHECK(c.logged("Android Auto waits aside: event 51 -> our menu"));
        c.menu(true);
        CHECK_OUT(c, "show, event 52");
        CHECK(c.logged("our menu is open after 0 s and 1 request(s)"));
        c.blankShown();
        c.overlay(1);
        c.polls(20);
        CHECK_OUT(c, "");
        CHECK(c.hold.holding() && c.hold.blankShown());
    }
    {  // iDrive refuses our menu: the blank state is asked for under our own popup, and iDrive performs it
        Car c;
        connected(c);
        c.overlay(2);
        c.polls(3);
        CHECK_OUT(c, "event 51, own popup 1, event 52, own popup 0, show");
        CHECK(c.logged("asked under our popup, popup closed: Android Auto comes up after 2 s"));
        c.blankVisible(true);  // shown, the focus still with what refused our menu
        c.overlay(1);
        c.polls(30);
        CHECK_OUT(c, "");  // held; no popup peek, no event while it has not had the focus
        CHECK(c.hold.holding() && c.hold.blankShown() && !c.hold.popupPeek());
        c.blankFocus(true);
        c.blankFocus(false);  // from now on a lost focus is an iDrive popup again
        c.polls(2);
        CHECK_OUT(c, "popup 1");
    }
    {  // iDrive refuses even that: on screen without the hold, our menu every second; a key frees iDrive later
        Car c;
        connected(c);
        c.overlay(2);
        c.polls(3);
        CHECK_OUT(c, "event 51, own popup 1, event 52, own popup 0, show");
        CHECK(c.hold.holding());
        c.overlay(1);
        c.poll();
        CHECK_OUT(c, "");  // the poll that sees it on screen
        c.polls(60);
        CHECK(c.take().size() == 60 * 8 + 59 * 2);  // "event 51" sixty times, nothing else
        c.menu(true);
        CHECK_OUT(c, "event 52");
        CHECK(c.logged("iDrive opened our menu after 62 request(s): taking the hold"));
        c.blankShown();
        c.polls(20);
        CHECK_OUT(c, "");
        CHECK(c.hold.holding() && c.hold.blankShown());
    }
    {  // shown without the hold, then MENU: the service steps aside, nothing more is asked
        Car c;
        connected(c);
        c.overlay(2);
        c.polls(3);
        c.overlay(1);
        c.polls(3);
        c.take();
        c.overlay(0);
        c.polls(10);
        CHECK_OUT(c, "");
        CHECK(!c.hold.holding());
    }
    {  // the driver waits in our menu for the phone: Android Auto comes up at once, no event 51
        Car c;
        connected(c);
        c.menu(true);
        c.take();
        c.overlay(2);
        c.poll();
        CHECK_OUT(c, "show, event 52");
        CHECK(c.logged("one of our pages has the focus"));
    }
    {  // a call, then the reverse gear: iDrive's call screen and the camera are left alone, then it goes on
        Car c;
        connected(c);
        c.call(true);
        c.overlay(2);
        c.polls(10);
        CHECK_OUT(c, "");
        c.call(false);
        c.gear(evo::app::kReverseGear);
        c.polls(10);
        CHECK_OUT(c, "");
        c.gear(1);
        c.poll();
        CHECK_OUT(c, "event 51");
        c.menu(true);
        CHECK_OUT(c, "show, event 52");
    }
    {  // the phone leaves while Android Auto waits: nothing more is asked
        Car c;
        connected(c);
        c.overlay(2);
        c.polls(2);
        c.take();
        c.service("connecting", 42, 0);
        c.polls(10);
        CHECK_OUT(c, "own popup 0");    // the popup opened for it is closed
        c.service("connected", 42, 2);  // and comes back
        c.poll();
        CHECK_OUT(c, "call 0, gear 1, event 51");
    }
    {  // the "show" got lost: the service still waits, it is sent again
        Car c;
        connected(c);
        c.overlay(2);
        c.poll();
        c.menu(true);
        CHECK_OUT(c, "event 51, show, event 52");
        c.blankShown();
        c.poll();  // still 2: not shown
        CHECK_OUT(c, "show, event 52");
        c.overlay(1);
        c.polls(5);
        CHECK_OUT(c, "");
    }
}

// A service that does not know "await" (an older one) shows Android Auto by itself: after iDrive's refusals it stays
// on screen, and our menu is asked for every second the same way.
void olderServiceRefused() {
    Car c;
    connected(c);
    c.overlay(1);
    c.polls(7);
    CHECK_OUT(c, "event 51, own popup 1, event 52, own popup 0, event 51, event 51, event 52");
    c.polls(3);
    CHECK_OUT(c, "event 51, event 51, event 51");
    c.menu(true);
    CHECK_OUT(c, "event 52");
    c.blankShown();
    c.polls(20);
    CHECK_OUT(c, "");
    CHECK(c.hold.holding());
}

void comesUpOverOurMenu() {
    Car c;
    connected(c);
    c.menu(true);  // Start in the menu, the phone connects meanwhile
    c.overlay(1);
    c.poll();
    CHECK_OUT(c, "event 52");
    CHECK(c.logged("Android Auto came on screen over our menu"));
}

void backToOurMenu() {
    Car c;
    heldOnBlank(c);
    c.blankVisible(false);
    c.menu(true);  // BACK / tilt-left popped iDrive to our menu
    CHECK_OUT(c, "event 52");
    CHECK(c.logged("BACK/tilt popped to our menu (immediate)"));
    c.ms += 250;
    c.hold.menuFocus(kHelpState, true, c.now, c.ms);  // a stray push opened Help within 300 ms
    CHECK_OUT(c, "");
    c.ms += 50;
    c.hold.menuFocus(kHelpState, true, c.now, c.ms);
    CHECK_OUT(c, "event 52");
    c.blankShown();
    c.polls(3);
    CHECK_OUT(c, "");
}

void popupOverBlank() {
    Car c;
    heldOnBlank(c);
    c.blankFocus(false);  // an iDrive popup takes the focus, the blank state stays visible
    CHECK_OUT(c, "");
    c.poll();
    CHECK_OUT(c, "popup 1");  // Android Auto steps aside so the driver sees it
    CHECK(c.hold.popupPeek());
    CHECK(c.logged("popup: iDrive took the focus over state 13 (gear 1): Android Auto picture removed"));
    c.polls(30);  // an alert longer than 10 s: still aside, no event 52 over it
    CHECK_OUT(c, "");
    c.blankFocus(true);  // the popup closes
    c.poll();
    CHECK_OUT(c, "popup 0");
    c.polls(3);
    CHECK_OUT(c, "");
    CHECK(c.hold.holding());
}

void drivingModeThenBack() {
    Car c;
    heldOnBlank(c);
    c.hold.driveModeChanged(c.now);
    c.blankFocus(false);  // iDrive's driving mode display, without a screen change
    c.polls(15);
    CHECK_OUT(c, "");  // its own shorter peek shows it (no popup 1), and its focus loss is not fought for 15 s
    c.poll();
    CHECK_OUT(c, "event 52");  // then after two polls without focus
    CHECK(c.logged("state lost focus, try 1"));
    for (int i = 2; i <= 8; ++i) {  // the focus does not come back: every two polls, 8 at most
        c.poll();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "event 52");
    }
    c.polls(4);
    CHECK_OUT(c, "");
    CHECK(c.logged("(state lost focus): already tried 8 times"));
    c.blankFocus(true);  // the focus comes back: the hold worked, its tries start over
    c.poll();
    CHECK_OUT(c, "");
    c.blankVisible(false);
    c.menu(true);              // the next BACK
    CHECK_OUT(c, "event 52");  // not left under Android Auto
}

void reverseGear() {
    {  // the camera: Android Auto steps aside by itself, iDrive is not moved
        Car c;
        heldOnBlank(c);
        CHECK(c.gear(2));
        c.blankFocus(false);
        c.overlay(0);
        c.poll();
        CHECK_OUT(c, "");  // neither the menu nor the main menu over the camera
        CHECK(c.logged("stepped aside (state 13 visible 1, hard key 0, gear 2)"));
        CHECK(!c.hold.holding());
        c.blankVisible(true);  // iDrive back on the blank state while in reverse: no show
        CHECK_OUT(c, "");
        CHECK(!c.gear(0));
        CHECK(!c.gear(1));  // Only the latest gear is forwarded after the camera closes.
        c.polls(3);
        CHECK_OUT(c, "");
        CHECK(c.hold.cameraWins());
        c.blankFocus(true);
        CHECK_OUT(c, "gear 1");
        c.overlay(1);  // reverse ended, Android Auto back over the blank state
        c.poll();
        CHECK_OUT(c, "");
        CHECK(c.hold.holding());
    }
    {  // in reverse, iDrive away from the blank state: no hard key step aside, no app key hold, no popup peek
        Car c;
        heldOnBlank(c);
        c.gear(2);
        c.io.appKey = true;
        c.io.appKeyAt = c.now;
        c.blankLeft();
        c.polls(5);
        CHECK_OUT(c, "");
        CHECK(c.io.appKey);
    }
    {  // in reverse, the blank state unfocused under Android Auto is not a popup
        Car c;
        heldOnBlank(c);
        c.gear(2);
        c.blankFocus(false);
        c.poll();
        CHECK_OUT(c, "");
        CHECK(!c.hold.popupPeek());
        c.polls(4);  // the camera keeps the focus before Android Auto steps aside: no event 52 over it
        CHECK_OUT(c, "");
        CHECK(!c.gear(1));
        c.overlay(0);
        c.poll();
        CHECK_OUT(c, "");  // Not an OPTION popup even if the overlay falls after the gear change.
        CHECK(!c.hold.popupPeek());
    }
    {  // A second reverse cycle keeps the camera in charge, including across a service restart.
        Car c;
        heldOnBlank(c);
        CHECK(c.gear(2));
        c.blankLeft();
        c.overlay(0);
        c.poll();
        CHECK(!c.gear(1));
        CHECK(c.gear(2));
        CHECK(!c.gear(1));
        c.service("idle", 0, 0);
        c.poll();
        CHECK_OUT(c, "");
        c.service("running", 43, 0);
        c.poll();
        CHECK_OUT(c, "await, call 0, gear 2");
        c.blankFocus(true);
        CHECK_OUT(c, "gear 1");
    }
    {  // No focus callback: release the gear after the timeout, but keep honoring park assist.
        Car c;
        heldOnBlank(c);
        c.gear(2);
        c.blankLeft();
        c.overlay(0);
        c.poll();
        c.hold.pdcChanged(evo::app::kParkAssistOn);
        CHECK_OUT(c, "pdc 1");
        CHECK(!c.gear(1));
        c.polls(HoldMachine::kCameraWaitS - 1);
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "gear 1");
        CHECK(c.hold.cameraWins());
        c.hold.pdcChanged(0);
        CHECK_OUT(c, "pdc 0");
        CHECK(!c.hold.cameraWins());
    }
    {  // Returning to our menu releases the gear before showing a waiting session.
        Car c;
        heldOnBlank(c);
        c.gear(2);
        c.blankLeft();
        c.overlay(0);
        c.poll();
        CHECK(!c.gear(1));
        c.overlay(2);
        c.poll();
        c.menu(false);
        CHECK_OUT(c, "");
        c.menu(true);
        CHECK_OUT(c, "gear 1, show, event 52");
    }
    {  // An explicit choice can end the wait even without a focus callback and still override park assist.
        Car c;
        heldOnBlank(c);
        c.gear(2);
        c.blankLeft();
        c.overlay(0);
        c.poll();
        c.hold.pdcChanged(evo::app::kParkAssistOn);
        CHECK_OUT(c, "pdc 1");
        CHECK(!c.gear(1));
        c.hold.showAndHold("Open row", c.now);
        CHECK_OUT(c, "gear 1, pdc 0, show, event 52");
        CHECK(!c.hold.cameraWins());
    }
    {  // No handoff when the camera did not interrupt Android Auto.
        Car c;
        connected(c);
        CHECK(c.gear(2));
        CHECK(c.gear(1));
    }
    {  // No handoff when the blank state already has focus.
        Car c;
        heldOnBlank(c);
        CHECK(c.gear(2));
        CHECK(c.gear(1));
        CHECK(!c.hold.cameraWins());
    }
}

// Park assist (sensors.PDCStatus), opened by its button without reverse (#26): like the camera in reverse.
void parkAssist() {
    {  // its view: Android Auto steps aside, iDrive is not moved; back by itself when it ends
        Car c;
        heldOnBlank(c);
        c.hold.pdcChanged(evo::app::kParkAssistOn);
        CHECK_OUT(c, "pdc 1");
        CHECK(c.hold.cameraWins());
        c.blankLeft();  // iDrive shows the park assist view
        c.overlay(0);
        c.poll();
        CHECK_OUT(c, "");  // neither the menu nor the main menu over it
        CHECK(!c.hold.holding());
        c.polls(5);
        CHECK_OUT(c, "");  // no hard key step aside: it is not a key
        c.hold.pdcChanged(0);
        CHECK_OUT(c, "pdc 0");
        c.overlay(1);    // the service shows the picture again at once
        c.blankShown();  // iDrive back where it was
        c.poll();
        CHECK_OUT(c, "");
        CHECK(c.hold.holding());
    }
    {  // still on after reverse: the driver who picks Android Auto gets it until park assist changes
        Car c;
        heldOnBlank(c);
        c.hold.pdcChanged(evo::app::kParkAssistOn);
        c.overlay(0);
        c.blankLeft();
        c.poll();
        CHECK_OUT(c, "pdc 1");
        c.hold.pdcChanged(evo::app::kParkAssistOn);  // the same value again changes nothing
        CHECK_OUT(c, "");
        c.hold.showAndHold("Open row", c.now);
        CHECK_OUT(c, "pdc 0, show, event 52");
        CHECK(!c.hold.cameraWins() && c.logged("Android Auto chosen over it"));
        c.blankShown();
        c.overlay(1);
        c.poll();
        CHECK_OUT(c, "");
        c.hold.pdcChanged(0);  // it ends: nothing to tell, Android Auto is already up
        CHECK_OUT(c, "");
        c.hold.pdcChanged(evo::app::kParkAssistOn);  // a new one: it owns the screen again
        CHECK_OUT(c, "pdc 1");
    }
    {  // still on after reverse, its view closed (BACK): iDrive back on the blank state brings Android Auto back (#88)
        Car c;
        heldOnBlank(c);
        CHECK(c.gear(2));
        c.blankFocus(false);  // the camera takes the focus, the blank state still visible under it (car log)
        c.overlay(0);
        c.poll();
        c.hold.pdcChanged(evo::app::kParkAssistOn);
        CHECK_OUT(c, "pdc 1");
        CHECK(!c.gear(1));  // out of reverse, park assist stays on
        c.polls(3);
        CHECK_OUT(c, "");
        c.blankFocus(true);  // its view closed: the focus back on the blank state
        CHECK_OUT(c, "gear 1, pdc 0");
        CHECK(!c.hold.cameraWins() && c.logged("its view closed"));
        c.overlay(1);
        c.poll();
        CHECK_OUT(c, "");  // held, nothing more to ask
        CHECK(c.hold.holding());
        c.hold.pdcChanged(2);  // it ends: Android Auto is already up
        CHECK_OUT(c, "");
    }
    {  // in reverse, the focus back on the blank state is not the view closing: the camera keeps the screen
        Car c;
        heldOnBlank(c);
        CHECK(c.gear(2));
        c.hold.pdcChanged(evo::app::kParkAssistOn);
        CHECK_OUT(c, "pdc 1");
        c.blankLeft();
        c.blankFocus(true);
        CHECK_OUT(c, "");
        CHECK(c.hold.cameraWins() && c.hold.parkAssistOwns());
    }
    {  // opened by its button: its view closed while still on, the picture back the same way
        Car c;
        heldOnBlank(c);
        c.hold.pdcChanged(evo::app::kParkAssistOn);
        CHECK_OUT(c, "pdc 1");
        c.blankFocus(false);  // its view
        c.overlay(0);
        c.poll();
        CHECK_OUT(c, "");
        c.blankFocus(true);
        CHECK_OUT(c, "pdc 0");
        CHECK(!c.hold.parkAssistOwns());
    }
    {  // a service started meanwhile is told, with the call
        Car c;
        c.hold.pdcChanged(evo::app::kParkAssistOn);
        CHECK_OUT(c, "pdc 1");
        c.service("running", 42, 0);
        c.poll();
        CHECK_OUT(c, "await, call 0, pdc 1");
    }
}

void calls() {
    {  // Android Auto comes up during a call, over iDrive's call screen: event 52 directly, the call screen not fought
        Car c;
        connected(c);
        c.call(true);
        c.overlay(1);
        c.poll();
        CHECK_OUT(c, "event 52");
        CHECK(c.logged("Android Auto came on screen during a call"));
        c.polls(20);
        CHECK_OUT(c, "");  // no menu route, no step aside during the call
        c.call(false);
        c.poll();
        CHECK_OUT(c, "event 52");  // the call ended: back on the blank state
        CHECK(c.logged("call ended"));
        c.blankShown();
        c.polls(3);
        CHECK_OUT(c, "");
    }
    {  // a call while Android Auto is held: no popup peek, no event 52, no immediate hold from the menu
        Car c;
        heldOnBlank(c);
        c.call(true);
        c.blankFocus(false);
        c.polls(5);
        CHECK_OUT(c, "");
        c.blankVisible(false);
        c.menu(true);
        CHECK_OUT(c, "");
        CHECK(c.io.reads == 0);
        c.blankShown();  // the call screen closed by itself before the call ended
        c.call(false);
        c.poll();
        CHECK_OUT(c, "");  // the blank state shows: the ended call needs nothing
        c.blankLeft();     // later, a hard key
        c.poll();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "hide");  // a hard key, not "call ended"
    }
    {  // Android Auto shown again by iDrive returning to the blank state: not during a call
        Car c;
        connected(c);
        c.call(true);
        c.blankVisible(true);
        CHECK_OUT(c, "");
    }
}

void appKeys() {
    {  // NAV opened Android Auto's navigation: iDrive switched to its own underneath, event 52 at once
        Car c;
        heldOnBlank(c);
        c.io.appKey = true;
        c.io.appKeyAt = c.now;
        c.now += 1;
        c.blankLeft();
        CHECK_OUT(c, "app key removed, event 52");
        CHECK(c.logged("a key kept Android Auto on screen, try 1"));
        c.blankShown();
        c.polls(3);
        CHECK_OUT(c, "");
        // Later, BACK in Android Auto's map (the service writes the same file): iDrive walks back to its navigation
        // screen, not to our menu. Seen on an F30 (#43): Android Auto stepped aside. Now: event 52 at once, no "hide".
        c.now += 5;
        c.io.appKey = true;
        c.io.appKeyAt = c.now;
        c.blankLeft();
        CHECK_OUT(c, "app key removed, event 52");
        c.blankShown();
        c.polls(4);
        CHECK_OUT(c, "");
    }
    {  // no file (MENU removed the one BACK wrote just before): iDrive left the blank state for a hard key
        Car c;
        heldOnBlank(c);
        c.blankLeft();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "hide");
    }
    {  // the key file appears after iDrive left the blank state: the poll catches it
        Car c;
        heldOnBlank(c);
        c.blankLeft();
        CHECK_OUT(c, "");
        c.io.appKey = true;
        c.io.appKeyAt = c.now + 1;
        c.poll();
        CHECK_OUT(c, "app key removed, event 52");
        CHECK(c.logged("a key kept Android Auto on screen (poll)"));
    }
    {  // a key file older than 2 s is a hard key
        Car c;
        heldOnBlank(c);
        c.io.appKey = true;
        c.io.appKeyAt = c.now - 3;
        c.blankLeft();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "hide");
        CHECK(c.io.appKey);
    }
    {  // during a call the key file is left alone
        Car c;
        heldOnBlank(c);
        c.call(true);
        c.io.appKey = true;
        c.io.appKeyAt = c.now;
        c.blankLeft();
        CHECK_OUT(c, "");
        CHECK(c.io.appKey);
    }
}

void hardKeys() {
    {  // MENU: confirmed on a second poll, Android Auto steps aside; no event when it does
        Car c;
        heldOnBlank(c);
        c.blankLeft();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "hide");
        CHECK(c.logged("hold: iDrive left state 13 (hard key): asked the service to step aside"));
        CHECK(!c.hold.holding());
        c.overlay(0);
        c.poll();
        CHECK_OUT(c, "");  // iDrive already went elsewhere: no main menu
        CHECK(c.logged("stepped aside (state 13 visible 0, hard key 1, gear 1)"));
    }
    {  // two sightings more than 3 s apart start over
        Car c;
        heldOnBlank(c);
        c.blankLeft();
        c.poll();
        c.now += 4;
        c.poll();
        CHECK_OUT(c, "");
        c.poll();
        CHECK_OUT(c, "hide");
    }
    {  // away for one poll, back, then away for one poll again within 3 s: two short departures are not a hard key
        Car c;
        heldOnBlank(c);
        c.blankLeft();
        c.poll();  // first sighting
        c.blankShown();
        c.poll();  // back on the blank state: that sighting is forgotten
        c.blankLeft();
        c.poll();  // a new first sighting, not the confirmation of the old one
        CHECK_OUT(c, "");
        CHECK(c.hold.holding());
        c.poll();  // still away a second later: a hard key
        CHECK_OUT(c, "hide");
    }
    {  // iDrive back on the blank state before the service stepped aside: still no main menu, it was asked to go
        Car c;
        heldOnBlank(c);
        c.blankLeft();
        c.polls(2);
        CHECK_OUT(c, "hide");
        c.blankShown();
        CHECK_OUT(c, "");  // Android Auto still on screen: no show
        c.overlay(0);
        c.poll();
        CHECK_OUT(c, "");
    }
}

void stepsAsideByItself() {
    {  // the phone's exit icon: iDrive main menu
        Car c;
        heldOnBlank(c);
        c.overlay(0);
        c.poll();
        CHECK_OUT(c, "event 50");
        CHECK(!c.hold.holding());
    }
    {  // OPTION: iDrive's option panel over the blank state, the app's menu
        Car c;
        heldOnBlank(c);
        c.blankFocus(false);
        c.overlay(0);
        c.poll();
        CHECK_OUT(c, "event 51");
        CHECK(c.logged("hold: OPTION: event 51 -> Android Auto menu"));
    }
}

void backToBlankWhileMinimized() {
    Car c;
    heldOnBlank(c);
    c.blankLeft();  // a hard key: Android Auto steps aside
    c.polls(2);
    c.overlay(0);
    c.poll();
    CHECK_OUT(c, "hide");
    const int reads = c.io.reads;
    c.blankShown();  // BACK walked iDrive's history back to the blank state
    CHECK_OUT(c, "show");
    CHECK(c.io.reads == reads + 1);
    CHECK(c.logged("iDrive went back to the Android Auto screen"));
    c.overlay(1);
    c.poll();
    CHECK_OUT(c, "");  // over the blank state already
    CHECK(c.hold.holding());
    {
        Car d;  // not while the phone is not connected
        d.service("running", 42, 0);
        d.poll();
        d.take();
        d.blankVisible(true);
        CHECK_OUT(d, "");
    }
}

void startWhileRunning() {
    Car c;
    connected(c);
    c.menu(true);
    c.hold.requestShow();  // Start while the service runs
    CHECK_OUT(c, "show");
    c.overlay(1);
    c.poll();
    CHECK_OUT(c, "event 52");
}

void serviceRestart() {
    {  // started in reverse: the call and the gear when it becomes reachable, again when the phone connects
        Car c;
        c.gear(2);
        c.service("connecting", -1, 0);  // the supervisor in its preflight
        c.poll();
        CHECK_OUT(c, "");
        c.service("running", 42, 0);
        c.poll();
        CHECK_OUT(c, "await, call 0, gear 2");  // "await": a phone that connects waits for our "show"
        c.poll();
        CHECK_OUT(c, "");
        c.service("connected", 42, 0);
        c.poll();
        CHECK_OUT(c, "call 0, gear 2");
        c.poll();
        CHECK_OUT(c, "");
        c.call(true);
        c.service("idle", 0, 0);  // the service ends and starts again
        c.poll();
        CHECK_OUT(c, "");
        c.service("running", 43, 0);
        c.poll();
        CHECK_OUT(c, "await, call 1, gear 2");
    }
    {  // no gear received yet: only the call
        Car c;
        c.service("running", 42, 0);
        c.poll();
        CHECK_OUT(c, "await, call 0");
    }
    {  // restarted while Android Auto was held: iDrive main menu, then the new overlay is held again
        Car c;
        heldOnBlank(c);
        c.service("idle", 0, 0);
        c.poll();
        CHECK_OUT(c, "event 50");
        c.blankLeft();  // iDrive on its main menu
        c.service("connected", 43, 1);
        c.poll();
        CHECK_OUT(c, "await, call 0, gear 1, event 51");
        c.menu(true);
        CHECK_OUT(c, "event 52");
    }
}

}  // namespace

// The machine counts in seconds since the head unit started, never with the date (which the GPS sets while driving):
// right after a start, when that clock is still close to 0, every delay holds.
void shortlyAfterStart() {
    Car c;
    c.now = 1;
    c.ms = 1000;
    heldOnBlank(c);
    c.blankLeft();  // a hard key
    c.poll();
    CHECK_OUT(c, "");
    c.poll();
    CHECK_OUT(c, "hide");
    {  // an app key file written a second ago, as the app reports its age
        Car d;
        d.now = 1;
        heldOnBlank(d);
        d.io.appKey = true;
        d.io.appKeyAt = d.now - 1;
        d.blankLeft();
        CHECK_OUT(d, "app key removed, event 52");
    }
}

// Android Auto brought back over iDrive's media screen (double MEDIA), which refuses our pages: our own popup gets the
// blank state under it, so the knob stops driving the media screen underneath (seen on the car).
static void heldThenMediaScreen(Car& c) {
    heldOnBlank(c);
    c.blankLeft();  // MEDIA: iDrive's media screen
    c.polls(2);
    CHECK_OUT(c, "hide");  // the hard key, confirmed on a second poll
    c.overlay(0);
    c.poll();
    c.take();
    c.hold.showAndHold("double MEDIA", c.now);
    CHECK_OUT(c, "show, event 52");
    c.overlay(1);
    c.poll();
    CHECK_OUT(c, "event 51");
    c.poll();
    CHECK_OUT(c, "own popup 1");
}
void recallOverIdriveScreen() {
    {
        Car c;
        heldThenMediaScreen(c);
        c.poll();
        CHECK_OUT(c, "event 52, own popup 0");
        c.blankVisible(true);  // iDrive performed it: shown, the focus still with its screen
        c.polls(10);
        CHECK_OUT(c, "");  // held; no popup peek, no event while it has not had the focus
        CHECK(c.hold.holding() && c.hold.blankShown() && !c.hold.popupPeek());
        c.blankFocus(true);
        c.polls(3);
        CHECK_OUT(c, "");
    }
    {  // Android Auto steps aside while our popup is open: the popup goes with it
        Car c;
        heldThenMediaScreen(c);
        c.overlay(0);
        c.poll();
        CHECK(c.take().find("own popup 0") != std::string::npos);
        CHECK(!c.hold.holding());
        c.polls(3);
        CHECK_OUT(c, "");
    }
}

int main() {
    shortlyAfterStart();
    manualOpen();
    openButNeverCame();
    automaticStartOverIdriveScreen();
    automaticStartRefused();
    waitsAsideForIdrive();
    olderServiceRefused();
    recallOverIdriveScreen();
    comesUpOverOurMenu();
    backToOurMenu();
    popupOverBlank();
    drivingModeThenBack();
    reverseGear();
    parkAssist();
    calls();
    appKeys();
    hardKeys();
    stepsAsideByItself();
    backToBlankWhileMinimized();
    startWhileRunning();
    serviceRestart();
    if (g_failures) {
        printf("FAIL hold machine: %d checks failed\n", g_failures);
        return 1;
    }
    puts(
        "PASS hold machine: manual open, automatic start through the menu, refusals, waiting aside, recall over an "
        "iDrive screen, BACK, popups, driving mode, "
        "reverse, park assist, calls, app keys, hard keys, stepping aside, service restart");
    return 0;
}
