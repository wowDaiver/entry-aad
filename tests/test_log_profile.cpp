// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Production writes the service's output nowhere; the marker file turns the logs on and off without a restart.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../hmiapp/log_profile.hpp"

int main() {
    char marker[] = "/tmp/entry-aad-profile-XXXXXX";
    FILE* created = fdopen(mkstemp(marker), "w");
    assert(created);
    fclose(created);
    assert(maintenanceProfile(marker));
    assert(!unlink(marker));
    assert(!maintenanceProfile(marker));
    puts("PASS production profile without logs, maintenance profile with logs");
    return 0;
}
