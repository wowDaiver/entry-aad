// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The menu's "Update from USB stick" button (platform/update_install.hpp for what it runs). A first press checks the
// stick and changes nothing; the button then names what a second press installs ("Install the update", "Install the
// older version"...), for kUpdateReadyS. The second press stops Android Auto if it runs, then installs; the unit
// restarts by itself. Never during a phone call or in reverse gear: the restart would end the call or take the camera
// away. Any other outcome shows on the button for kUpdateResultS, then it reads "Update from USB stick" again.
// While Android Auto is being stopped for the install, while the install runs and until the unit restarts, no start
// of Android Auto is allowed. If Android Auto was stopped for an install that then changed nothing, it is started
// again. Pure: tests/test_update_flow.cpp.
#ifndef ENTRY_AAD_HMIAPP_UPDATE_FLOW_HPP
#define ENTRY_AAD_HMIAPP_UPDATE_FLOW_HPP

#include "../platform/update_install.hpp"
#include "rhmi_ids.hpp"

namespace evo {
namespace app {

const long kUpdateReadyS = 30;    // the second press must come within this
const long kUpdateResultS = 15;   // an outcome stays on the button this long
const long kUpdateStopS = 45;     // a stop takes 20 s at most, the Bluetooth given back a few more
const long kUpdateRestartS = 60;  // the unit restarts within seconds: past this, the driver restarts it

class UpdateFlow {
public:
    enum Step { kNothing, kStartCheck, kStartInstall, kStopAndroidAuto };
    // What the car is doing, at each press and poll.
    struct Car {
        bool androidAuto;  // Android Auto runs, or is still stopping
        bool mayRestart;   // no phone call and not in reverse gear: the unit may restart
    };

    UpdateFlow() : state_(kIdle), text_(kTextUpdate), since_(0), stoppedAndroidAuto_(false), restart_(false) {}

    // The button.
    Step press(long now, const Car& car) {
        switch (state_) {
            case kIdle:
            case kShowing:
                set(kChecking, kTextUpdateChecking, now);
                return kStartCheck;
            case kReady:
                if (!car.mayRestart) {
                    set(kShowing, kTextUpdateNotNow, now);
                    return kNothing;
                }
                if (car.androidAuto) {
                    set(kStopping, kTextUpdateStopping, now);
                    stoppedAndroidAuto_ = true;
                    return kStopAndroidAuto;
                }
                set(kInstalling, kTextUpdateInstalling, now);
                return kStartInstall;
            default:
                return kNothing;  // checking, stopping, installing, restarting: the press changes nothing
        }
    }

    // Once per status poll.
    Step poll(long now, const Car& car) {
        if (state_ == kReady && now - since_ >= kUpdateReadyS) {
            set(kIdle, kTextUpdate, now);
        } else if (state_ == kShowing && now - since_ >= kUpdateResultS) {
            set(kIdle, kTextUpdate, now);
        } else if (state_ == kStopping && !car.mayRestart) {
            giveUp(kTextUpdateNotNow, now);  // a call or reverse gear came while Android Auto stopped
        } else if (state_ == kStopping && !car.androidAuto) {
            set(kInstalling, kTextUpdateInstalling, now);
            return kStartInstall;
        } else if (state_ == kStopping && now - since_ >= kUpdateStopS) {
            giveUp(kTextUpdateBusy, now);
        } else if (state_ == kRestarting && text_ == kTextUpdateRestarting && now - since_ >= kUpdateRestartS) {
            text_ = kTextUpdateRestartByHand;  // stays: the app on screen is the old one
        }
        return kNothing;
    }

    // The check or the install has ended: its exit code (UpdateResult), or -2 when it ended otherwise (killed,
    // crashed).
    void ended(int code, long now) {
        if (state_ == kChecking) {
            checked(code, now);
        } else if (state_ == kInstalling) {
            installed(code, now);
        }
    }
    // The check or the install could not even start: nothing ran.
    void startFailed(long now) {
        if (state_ == kChecking || state_ == kInstalling) {
            giveUp(kTextUpdateFailed, now);
        }
    }

    unsigned text() const { return text_; }
    bool checking() const { return state_ == kChecking; }
    bool installing() const { return state_ == kInstalling; }
    // Android Auto must not start: it is being stopped for the install, the install runs, or the unit restarts.
    bool holdsAndroidAuto() const { return state_ == kStopping || state_ == kInstalling || state_ == kRestarting; }
    // Once: Android Auto was stopped for an install that then changed nothing; start it again.
    bool takeRestart() {
        const bool restart = restart_;
        restart_ = false;
        return restart;
    }

private:
    enum State { kIdle, kChecking, kReady, kStopping, kInstalling, kShowing, kRestarting };

    void set(State state, unsigned text, long now) {
        state_ = state;
        text_ = text;
        since_ = now;
    }

    // Nothing was installed: the outcome shows, and Android Auto comes back if it was stopped for this.
    void giveUp(unsigned text, long now) {
        set(kShowing, text, now);
        restart_ = stoppedAndroidAuto_;
        stoppedAndroidAuto_ = false;
    }

    void checked(int code, long now) {
        switch (static_cast<UpdateResult>(code)) {
            case UpdateResult::kNewer:
                return set(kReady, kTextUpdateInstall, now);
            case UpdateResult::kOlder:
                return set(kReady, kTextUpdateOlder, now);
            case UpdateResult::kRepair:
                return set(kReady, kTextUpdateReinstall, now);
            case UpdateResult::kPending:
                return set(kReady, kTextUpdateFinish, now);
            default:
                return set(kShowing, outcomeText(code), now);
        }
    }

    void installed(int code, long now) {
        if (code == static_cast<int>(UpdateResult::kInstalled)) {
            stoppedAndroidAuto_ = false;
            return set(kRestarting, kTextUpdateRestarting, now);
        }
        // Part of it may be in place: an install that reached its swap (incomplete), or one killed or crashed at an
        // unknown point. Android Auto stays stopped; the next check says what to do.
        if (code == static_cast<int>(UpdateResult::kIncomplete) || code == -2) {
            stoppedAndroidAuto_ = false;
            return set(kShowing, kTextUpdateIncomplete, now);
        }
        giveUp(outcomeText(code), now);  // refused before any change
    }

    static unsigned outcomeText(int code) {
        switch (static_cast<UpdateResult>(code)) {
            case UpdateResult::kUpToDate:
                return kTextUpToDate;
            case UpdateResult::kNoStick:
                return kTextNoStick;
            case UpdateResult::kNoPackage:
                return kTextNoUpdate;
            case UpdateResult::kInvalid:
                return kTextUpdateInvalid;
            case UpdateResult::kIncompatible:
                return kTextUpdateIncompatible;
            case UpdateResult::kBusy:
                return kTextUpdateBusy;
            case UpdateResult::kNotInstalled:
                return kTextUpdateNotOurs;
            case UpdateResult::kIncomplete:
                return kTextUpdateIncomplete;
            default:
                return kTextUpdateFailed;  // kFailed, a code out of place (1, 2, 127: the program refused to run)
        }
    }

    State state_;
    unsigned text_;
    long since_;
    bool stoppedAndroidAuto_;  // this flow stopped Android Auto for its install
    bool restart_;
};

}  // namespace app
}  // namespace evo

#endif  // ENTRY_AAD_HMIAPP_UPDATE_FLOW_HPP
