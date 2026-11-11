// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The restart a change of link asks for (the menu's USB checkbox while Android Auto runs): the service reads its link
// once, at its start, so it is stopped, then started again once nothing of it is left. Pure: tested on the PC
// (tests/test_pending_restart.cpp).
#ifndef ENTRY_AAD_HMIAPP_PENDING_RESTART_HPP
#define ENTRY_AAD_HMIAPP_PENDING_RESTART_HPP

namespace evo {
namespace app {

// A stop takes 20 s at most, the supervisor's Bluetooth restore a few more: past this, the start is left to the menu.
const long kRestartGiveUpS = 45;

class PendingRestart {
public:
    enum Turn { kNothing, kStart, kGiveUp };

    PendingRestart() : asked_(false), since_(0) {}
    void ask(long nowSeconds) {
        asked_ = true;
        since_ = nowSeconds;
    }
    // A start or a stop by hand: the user decides again.
    void cancel() { asked_ = false; }
    bool asked() const { return asked_; }
    // Once per status poll. busy: a stop is in progress, the service is there, or its supervisor still is.
    // kStart and kGiveUp are each returned once.
    Turn turn(long nowSeconds, bool busy) {
        if (!asked_) {
            return kNothing;
        }
        if (!busy) {
            asked_ = false;
            return kStart;
        }
        if (nowSeconds - since_ >= kRestartGiveUpS) {
            asked_ = false;
            return kGiveUp;
        }
        return kNothing;
    }

private:
    bool asked_;
    long since_;
};

}  // namespace app
}  // namespace evo

#endif
