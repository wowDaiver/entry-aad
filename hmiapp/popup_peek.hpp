// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Decides when Android Auto steps aside for an iDrive popup. Pure logic, no HMI call (tests/test_popup_peek.cpp).
// Android Auto is drawn above iDrive, so anything iDrive opens over our blank state stays hidden. Such a popup takes
// the focus from that state while it stays visible, the only reliable signal we already receive without new HMI calls.
// CarPlay gets more detail from CHmiPopupDetector; that needs IHmiStateManager vtable calls this app does not make.
#ifndef ENTRY_AAD_HMIAPP_POPUP_PEEK_HPP
#define ENTRY_AAD_HMIAPP_POPUP_PEEK_HPP

class PopupPeek {
public:
    enum Action { kNone = 0, kStart = 1, kEnd = -1 };
    static const int kDriveModeSeconds = 6;  // its own shorter peek already shows the driving mode popup

    PopupPeek() : peek_(false), decided_(false) {}

    // Once per status poll; times in seconds since the head unit started. covered = iDrive holds the focus over our
    // visible state while Android Auto is on screen. One decision per covered span: a popup still open when the time
    // limit ends does not take the picture again.
    Action step(bool covered, long now, long lastDriveMode) {
        if (!covered) {
            decided_ = false;
            if (!peek_) {
                return kNone;
            }
            peek_ = false;
            return kEnd;
        }
        if (decided_) {
            return kNone;
        }
        decided_ = true;
        if (lastDriveMode && now - lastDriveMode < kDriveModeSeconds) {
            return kNone;
        }
        peek_ = true;
        return kStart;
    }
    // Android Auto is aside for an iDrive popup: no hold event is fired at it until it closes.
    bool active() const { return peek_; }

private:
    bool peek_, decided_;
};

// Whether iDrive draws its driving mode popup at a mode change, so that Android Auto steps aside for it. setting:
// iDrive's own popup setting; forced: the coding that shows it anyway (hmiapp/idrive_coding.hpp reads both); each 1,
// 0, or -1 when it could not be read. Only a setting read off with nothing forcing it means no popup (#28: 3.5 s of
// black at each change); otherwise Android Auto steps aside as it always did.
inline bool driveModePopupExpected(int setting, int forced) {
    return !(setting == 0 && forced == 0);
}

#endif
