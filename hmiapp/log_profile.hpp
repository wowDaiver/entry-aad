// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Production keeps no log, on flash or in memory. The maintenance profile is one file on the data partition,
// created at installation, by hand or by the menu's "keep logs" box: the app and the service then log as usual
// (where the service's log goes: platform/kept_log.hpp). tests/test_log_profile.cpp.
#ifndef ENTRY_AAD_HMIAPP_LOG_PROFILE_HPP
#define ENTRY_AAD_HMIAPP_LOG_PROFILE_HPP
#include <unistd.h>

inline bool maintenanceProfile(const char* marker) {
    return access(marker, F_OK) == 0;
}

#endif
