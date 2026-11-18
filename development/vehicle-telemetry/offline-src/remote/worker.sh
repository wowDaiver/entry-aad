#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
. "$(dirname -- "$0")/common.sh"
mode=${1:-}
case "$mode" in install|disable) ;; *) die UNKNOWN_OPERATION;; esac
verify_kit; firmware; layout
mkdir "$LOCK" 2>/dev/null || die LOCK_PRESENT_USE_STATUS
printf '%s\n' "$ID" >"$LOCK/owner"
printf '%s\n' "$$" >"$LOCK/pid"
boot_id >"$LOCK/boot"
process_start "$$" >"$LOCK/start"
rw=0; operation_started=0; original_signature=; original_flags=
cleanup() {
    rc=$?; set +eu; trap - 0 HUP INT TERM
    if [ "$rw" = 1 ]; then
        # On a caught installation error, stop future loading; mapped DSO/resources stay intact.
        if [ "$(signature)" = "$original_signature" ] && [ "$rc" != 0 ] && [ "$mode" = install ] && owned && manifest_valid "$PLUGIN/VehicleTelemetry.manifest"; then
            if { [ ! -e "$PLUGIN/$DISABLED" ] && [ ! -L "$PLUGIN/$DISABLED" ]; } || manifest_valid "$PLUGIN/$DISABLED"; then
                mv "$PLUGIN/VehicleTelemetry.manifest" "$PLUGIN/$DISABLED" || rc=1
            fi
        fi
        bounded 15 sync || rc=1
        restore_ro || { echo ROOT_RO_RESTORE_FAILED_RUN_STATUS_AND_DISABLE >&2; rc=1; }
    fi
    if [ "$operation_started" = 1 ] && [ "$rc" != 0 ]; then phase failed || :; fi
    if [ "$rc" != 0 ] && regular "$DATA/trial-arm" && [ "$(cat "$DATA/trial-arm")" = "$TRIAL_TOKEN" ]; then rm "$DATA/trial-arm" || :; fi
    if [ "$rc" != 0 ] && regular "$DATA/persistent.enabled" && [ "$(cat "$DATA/persistent.enabled")" = "$PERSISTENT_TOKEN" ]; then rm "$DATA/persistent.enabled" || :; fi
    printf '%s\n' "$rc" >"$KIT/result.tmp" && mv "$KIT/result.tmp" "$KIT/result"
    # Delete only this live worker's complete, known lock.
    if lock_active && [ "$(cat "$LOCK/pid")" = "$$" ]; then
        rm "$LOCK/pid" "$LOCK/boot" "$LOCK/start" "$LOCK/owner"
        rmdir "$LOCK" || :
    fi
    exit "$rc"
}
trap cleanup 0
trap 'exit 130' HUP INT TERM
state=$(plugin_state)
case "$mode:$state" in
    install:installed) root_ro || die ROOT_RW_RECOVERY_REQUIRED; echo ALREADY_INSTALLED_NO_FILES_CHANGED; exit 0;;
    install:absent|install:disabled) root_ro || die ROOT_RW_RECOVERY_REQUIRED;;
    disable:absent)
        if root_ro; then echo NOTHING_TO_DISABLE; exit 0; fi
        regular "$DATA/owner" && [ "$(cat "$DATA/owner")" = "$ID" ] || die ROOT_RW_WITHOUT_OWN_JOURNAL;;
    disable:*) owned || die UNKNOWN_PLUGIN_PRESERVED;;
    *) die PARTIAL_OR_UNKNOWN_PLUGIN_PRESERVED;;
esac
if [ "$mode" = disable ]; then
    if [ -e "$PLUGIN/VehicleTelemetry.manifest" ] || [ -L "$PLUGIN/VehicleTelemetry.manifest" ]; then
        manifest_valid "$PLUGIN/VehicleTelemetry.manifest" || die UNKNOWN_MANIFEST_PRESERVED
        if [ -e "$PLUGIN/$DISABLED" ] || [ -L "$PLUGIN/$DISABLED" ]; then
            manifest_valid "$PLUGIN/$DISABLED" || die UNKNOWN_DISABLED_FILE
        fi
    fi
fi
if [ "$mode" = install ]; then
    [ ! -e "$DATA/persistent.disabled" ] && [ ! -L "$DATA/persistent.disabled" ] || die GUARD_DISABLED_REVIEW_REQUIRED
    [ ! -e "$DATA/persistent.state.new" ] && [ ! -L "$DATA/persistent.state.new" ] || die INTERRUPTED_GUARD_STATE_PRESERVED
    space
    if pidof entry-aad aa-supervisor >/dev/null 2>&1; then die AA_RUNNING_STOP_IN_ITS_MENU_AND_REPEAT; fi
    loader
    # Complete immutable payload only; never overwrite an existing DSO.
    verify "$KIT/payload.sha256" "$KIT/payload" || die PAYLOAD_CHECKSUM_MISMATCH
fi
original_signature=$(signature)
if root_ro; then
    original_flags=$(root_flags)
else
    # Only our interrupted transaction can authorize restoring its recorded RO flags.
    regular "$DATA/root-signature" && regular "$DATA/root-flags" || die UNKNOWN_RW_ROOT
    [ "$(cat "$DATA/root-signature")" = "$original_signature" ] || die ROOT_IDENTITY_CHANGED
    original_flags=$(cat "$DATA/root-flags")
    printf '%s\n' "$original_flags" | has_ro_flag || die BAD_SAVED_ROOT_FLAGS
fi
if [ ! -d "$DATA" ]; then
    mkdir -m 700 "$DATA"; printf '%s\n' "$ID" >"$DATA/owner"
fi
# Every persistent journal file we replace must be a regular file, never a symlink.
for file in .phase-new root-signature root-flags phase; do
    [ ! -e "$DATA/$file" ] && [ ! -L "$DATA/$file" ] || regular "$DATA/$file" || die UNKNOWN_JOURNAL_FILE
done
printf '%s\n' "$original_signature" >"$DATA/root-signature"
printf '%s\n' "$original_flags" >"$DATA/root-flags"
phase prepared; operation_started=1
if [ "$mode" = install ]; then
    if [ -e "$DATA/persistent.enabled" ] || [ -L "$DATA/persistent.enabled" ]; then
        regular "$DATA/persistent.enabled" && [ "$(cat "$DATA/persistent.enabled")" = "$PERSISTENT_TOKEN" ] || die UNKNOWN_PERSISTENT_ENABLE
    else
        (umask 077; set -C; printf '%s\n' "$PERSISTENT_TOKEN" >"$DATA/persistent.enabled") || die CANNOT_ENABLE_PERSISTENT
    fi
    chmod 600 "$DATA/persistent.enabled"
else
    if [ -e "$DATA/persistent.enabled" ] || [ -L "$DATA/persistent.enabled" ]; then
        regular "$DATA/persistent.enabled" && [ "$(cat "$DATA/persistent.enabled")" = "$PERSISTENT_TOKEN" ] || die UNKNOWN_PERSISTENT_ENABLE
        rm "$DATA/persistent.enabled"
    fi
    if [ -e "$DATA/trial-arm" ] || [ -L "$DATA/trial-arm" ]; then
        regular "$DATA/trial-arm" && [ "$(cat "$DATA/trial-arm")" = "$TRIAL_TOKEN" ] || die UNKNOWN_TRIAL_PERMIT
        rm "$DATA/trial-arm"
    fi
fi
bounded 15 sync
rw=1
remount_root rw || die ROOT_REMOUNT_FAILED
[ "$(signature)" = "$original_signature" ] || die ROOT_IDENTITY_CHANGED
phase writing
if [ "$mode" = install ]; then
    if [ "$state" = absent ]; then
        stage=$PARENT/.VehicleTelemetry-$ID
        [ ! -e "$stage" ] && [ ! -L "$stage" ] || die STAGE_PRESENT_PRESERVED
        mkdir "$stage"
        while read -r expected relative; do
            [ "$relative" != VehicleTelemetry.manifest ] || continue
            cp "$KIT/payload/$relative" "$stage/$relative"
            chmod 644 "$stage/$relative"
            [ "$(sha "$stage/$relative")" = "$expected" ] || die STAGED_FILE_MISMATCH
        done <"$KIT/payload.sha256"
        chmod 755 "$stage/libVehicleTelemetry.so" "$stage"
        mv "$stage" "$PLUGIN"
    else
        resources_valid && manifest_valid "$PLUGIN/$DISABLED" || die DISABLED_FILES_CHANGED
    fi
    # Manifest is the last published file. A crash before this leaves no active registration.
    [ ! -e "$PLUGIN/VehicleTelemetry.manifest" ] && [ ! -L "$PLUGIN/VehicleTelemetry.manifest" ] || die MANIFEST_APPEARED
    [ ! -e "$PLUGIN/.manifest-new" ] && [ ! -L "$PLUGIN/.manifest-new" ] || die MANIFEST_STAGE_OCCUPIED
    cp "$KIT/payload/VehicleTelemetry.manifest" "$PLUGIN/.manifest-new"
    chmod 644 "$PLUGIN/.manifest-new"
    manifest_valid "$PLUGIN/.manifest-new" || die MANIFEST_STAGE_MISMATCH
    mv "$PLUGIN/.manifest-new" "$PLUGIN/VehicleTelemetry.manifest"
    verify "$KIT/payload.sha256" "$PLUGIN" || die INSTALLED_READBACK_FAILED
else
    if [ -e "$PLUGIN/VehicleTelemetry.manifest" ] || [ -L "$PLUGIN/VehicleTelemetry.manifest" ]; then
        manifest_valid "$PLUGIN/VehicleTelemetry.manifest" || die UNKNOWN_MANIFEST_PRESERVED
        if [ -e "$PLUGIN/$DISABLED" ] || [ -L "$PLUGIN/$DISABLED" ]; then
            manifest_valid "$PLUGIN/$DISABLED" || die UNKNOWN_DISABLED_FILE
        fi
        mv "$PLUGIN/VehicleTelemetry.manifest" "$PLUGIN/$DISABLED"
    fi
    [ ! -e "$PLUGIN/VehicleTelemetry.manifest" ] && [ ! -L "$PLUGIN/VehicleTelemetry.manifest" ] || die DISABLE_FAILED
fi
bounded 15 sync
restore_ro || die ROOT_RO_RESTORE_FAILED
rw=0
phase "$mode-complete"
echo "${mode}_VERIFIED_ROOT_RO_MANUAL_HU_RESTART_REQUIRED"
echo NO_REBOOT_COMMAND_SENT
