// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The ids of hmiapp/EntryAad.xml the app's code uses: states, events, actions, models, texts, images, components.
// The XML is parsed by the car and must not change for code reasons: these values follow it. A new component
// copies one that already works on the car.
#ifndef ENTRY_AAD_HMIAPP_RHMI_IDS_HPP
#define ENTRY_AAD_HMIAPP_RHMI_IDS_HPP

namespace evo {
namespace app {

// ---- hmiStates -----------------------------------------------------------------------------------------------------
const unsigned kMenuState = 10;  // the app's menu
const unsigned kHelpState = 12;  // "Help", child of the menu
const unsigned kAaState = 13;    // inert blank state held while Android Auto is on screen (exclusivity)
// State handler kinds, as bmw::PluginApp::addHandler(state, kind, ...) takes them.
const unsigned kHandlerFocus = 1;
const unsigned kHandlerVisible = 11;

// ---- focusEvents ---------------------------------------------------------------------------------------------------
const unsigned kEvGoHome = 50;    // -> NativeStateID 1 (iDrive main menu)
const unsigned kEvOpenMenu = 51;  // -> state 10 (the app's menu)
const unsigned kEvHoldAa = 52;    // -> state 13 (the blank state)
// popupEvent -> state 14, an empty popup of the app, opened (true) or closed (false). iDrive performs an app's
// focus event when the topmost popup is an app's (CFocusEvent::isCurrentlyAllowed), and a popup event is not
// subject to that test (CPopupEvent::perform): 3 popups at a time for a local app, given back when they close.
const unsigned kEvOwnPopup = 53;

// ---- raActions -----------------------------------------------------------------------------------------------------
const unsigned kActEntry = 2;  // entry button "Android Auto" (iDrive Media menu)
const unsigned kActStart = 21;
const unsigned kActStop = 22;
const unsigned kActAutostart = 24;
const unsigned kActUsbCable = 26;
const unsigned kActKeepLogs = 27;
const unsigned kActCopyLogs = 28;
const unsigned kActUpdate = 29;

// ---- the entry button ----------------------------------------------------------------------------------------------
const unsigned kEntryTextModel = 3;
const unsigned kEntryImageModel = 4;
const unsigned kTextDefault = 1;     // "Android Auto"
const unsigned kTextConnecting = 2;  // "Android Auto: Connecting..."
const unsigned kImgEntryIcon = 55001;
const unsigned kImgSpinFirst = 55010;  // first frame of the connecting animation
const unsigned kSpinFrames = 8;

// ---- the menu ------------------------------------------------------------------------------------------------------
const unsigned kStartRowModel = 31;
const unsigned kTextStart = 10;          // "Start Android Auto"
const unsigned kTextRowConnecting = 13;  // "Connecting to the phone..."
const unsigned kTextRowOpen = 14;        // "Open Android Auto"
const unsigned kAutostartModel = 34;
const unsigned kUsbCableModel = 38;
const unsigned kKeepLogsModel = 33;   // "Keep logs" checkbox
const unsigned kCopyLogsModel = 42;   // the copy button's text, one of these:
const unsigned kTextCopyLogs = 40;    // "Copy logs to USB stick"
const unsigned kTextCopying = 41;     // "Copying logs..."
const unsigned kTextCopied = 42;      // "Logs copied to the USB stick"
const unsigned kTextNoStick = 43;     // "No USB stick found"
const unsigned kTextNoLogs = 44;      // "No logs: tick Keep logs"
const unsigned kTextCopyFailed = 45;  // "Copy failed"
const unsigned kUpdateModel = 43;     // the update button's text (hmiapp/update_flow.hpp), one of these:
const unsigned kTextUpdate = 46;      // "Update from USB stick"
const unsigned kTextUpdateChecking = 47;
const unsigned kTextUpdateInstall = 48;    // ready, a second press installs: a newer version,
const unsigned kTextUpdateOlder = 49;      // an older one,
const unsigned kTextUpdateReinstall = 50;  // the installed one again (the car's files differ from it),
const unsigned kTextUpdateFinish = 51;     // the rest of an interrupted update
const unsigned kTextUpToDate = 52;
const unsigned kTextNoUpdate = 53;
const unsigned kTextUpdateInvalid = 54;
const unsigned kTextUpdateIncompatible = 55;
const unsigned kTextUpdateStopping = 56;
const unsigned kTextUpdateInstalling = 57;
const unsigned kTextUpdateRestarting = 58;
const unsigned kTextUpdateRestartByHand = 59;
const unsigned kTextUpdateFailed = 60;
const unsigned kTextUpdateIncomplete = 61;
const unsigned kTextUpdateBusy = 62;
const unsigned kTextUpdateNotOurs = 63;
const unsigned kTextUpdateNotNow = 64;  // during a phone call or in reverse gear: the restart would interrupt them
const unsigned kStartButton = 61;
const unsigned kStopButton = 62;  // hidden while Android Auto is stopped
// Component properties, as bmw::PluginApp::setProperty takes them.
const unsigned kPropVisible = 3;
const unsigned kPropWaiting = 8;

}  // namespace app
}  // namespace evo

#endif
