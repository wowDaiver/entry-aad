#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
. "$(dirname -- "$0")/common.sh"
verify_kit
mode=${1:-status}
case "$mode" in
    status|collect) firmware; layout; status;;
    check)
        firmware; layout; space; status
        root_ro || die ROOT_RW_RECOVERY_REQUIRED
        [ ! -e "$LOCK" ] && [ ! -L "$LOCK" ] || die LOCK_PRESENT_USE_STATUS
        case "$(plugin_state)" in absent|installed|disabled) ;; *) die UNKNOWN_PLUGIN_PRESERVED;; esac
        loader
        echo CHECK_PASS_NOT_INSTALLED_BY_THIS_COMMAND;;
    unlock)
        firmware; layout
        [ -d "$LOCK" ] && [ ! -L "$LOCK" ] || { echo NO_LOCK; exit 0; }
        regular "$LOCK/owner" && [ "$(cat "$LOCK/owner")" = "$ID" ] || die UNKNOWN_LOCK
        for file in pid boot start; do regular "$LOCK/$file" || die INCOMPLETE_LOCK_REBOOT_ONLY_WHEN_ROOT_RO; done
        lock_active && die WORKER_STILL_ACTIVE
        # Preserve the stale lock as evidence, never kill a process or remove it blindly.
        saved=$LOCK.saved-$(date +%s)-$$
        [ ! -e "$saved" ] && [ ! -L "$saved" ] || die LOCK_ARCHIVE_EXISTS
        mv "$LOCK" "$saved"
        echo STALE_LOCK_PRESERVED_CAN_RUN_DISABLE_OR_INSTALL;;
    *) die UNKNOWN_MODE;;
esac
