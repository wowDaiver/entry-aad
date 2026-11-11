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

// ---- raActions -----------------------------------------------------------------------------------------------------
const unsigned kActEntry = 2;  // entry button "Android Auto" (iDrive Media menu)
const unsigned kActStart = 21;
const unsigned kActStop = 22;
const unsigned kActAutostart = 24;
const unsigned kActUsbCable = 26;

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
const unsigned kStartButton = 61;
const unsigned kStopButton = 62;  // hidden while Android Auto is stopped
// Component properties, as bmw::PluginApp::setProperty takes them.
const unsigned kPropVisible = 3;
const unsigned kPropWaiting = 8;

}  // namespace app
}  // namespace evo

#endif
