#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from remote/ of a verified private kit. No reboot or vehicle controls.
. "$(dirname -- "$0")/common.sh"
mode=${1:-check}
case "$mode" in check|install|rollback) ;; *) die UNKNOWN_OPERATION;; esac
verify_kit; firmware; layout; space
root_ro || die ROOT_RW_RECOVERY_REQUIRED
owned || die UNKNOWN_PLUGIN_OWNER
OLD_SHA=728c735cd7a45b05da671e5f3c0340fa6002d538625dd0103daaf00b848666ea
NEW_SHA=$(sha "$KIT/payload/libVehicleTelemetry.so")
[ "$NEW_SHA" != "$OLD_SHA" ] || die NO_NEW_BUILD
HELPER=${FIRMWARE_ROOT%/}/opt/vehicle-telemetry
UNIT=${FIRMWARE_ROOT%/}/etc/systemd/system/vehicle-telemetry-most.service
WANTS=${FIRMWARE_ROOT%/}/etc/systemd/system/focussed.target.wants/vehicle-telemetry-most.service
BACKUP=$PARENT/.VehicleTelemetry.before-internal-$OLD_SHA.so
HELPER_SHA=$(sha "$KIT/transport/vehicle-telemetry-most")
UNIT_SHA=$(sha "$KIT/transport/vehicle-telemetry-most.service")
match() { regular "$1" && [ "$(sha "$1")" = "$2" ]; }
absent() { [ ! -e "$1" ] && [ ! -L "$1" ]; }
for path in "${FIRMWARE_ROOT%/}/opt" "${UNIT%/*}" "${WANTS%/*}"; do
 [ -d "$path" ] && [ ! -L "$path" ] || die UNKNOWN_PARENT
 [ "$(mount_row "$path")" = "$(mount_row /)" ] || die SEPARATE_MOUNT
 done
for path in "$BACKUP"; do absent "$path" || match "$path" "$OLD_SHA" || die UNKNOWN_BACKUP; done
current=$(sha "$PLUGIN/libVehicleTelemetry.so")
case "$current" in "$OLD_SHA"|"$NEW_SHA") ;; *) die UNKNOWN_INSTALLED_DSO;; esac
# Existing resources and registration remain byte-for-byte identical.
while read -r expected relative; do
 [ "$relative" != libVehicleTelemetry.so ] || continue
 match "$PLUGIN/$relative" "$expected" || die INSTALLED_RESOURCES_CHANGED
done <"$KIT/payload.sha256"
if ! absent "$HELPER"; then
 [ -d "$HELPER" ] && [ ! -L "$HELPER" ] || die UNKNOWN_HELPER_DIRECTORY
 for path in "$HELPER"/* "$HELPER"/.[!.]* "$HELPER"/..?*; do
  absent "$path" && continue
  [ "$path" = "$HELPER/vehicle-telemetry-most" ] && match "$path" "$HELPER_SHA" || die UNKNOWN_HELPER_FILE
 done
fi
absent "$UNIT" || match "$UNIT" "$UNIT_SHA" || die UNKNOWN_UNIT
if ! absent "$WANTS"; then
 [ -L "$WANTS" ] && [ "$(readlink "$WANTS")" = ../vehicle-telemetry-most.service ] || die UNKNOWN_ENABLE_LINK
fi
for path in "$PLUGIN/.most-new" "$HELPER/.most-new" "$UNIT.new" "$LOCK"; do absent "$path" || die STALE_TRANSACTION_PRESERVED; done
if [ "$current" = "$NEW_SHA" ]; then
 match "$BACKUP" "$OLD_SHA" && match "$HELPER/vehicle-telemetry-most" "$HELPER_SHA" &&
 match "$UNIT" "$UNIT_SHA" && [ -L "$WANTS" ] || die INCOMPLETE_UPDATE_RECOVERY_REQUIRED
fi
if [ "$mode" = check ]; then loader; echo INTERNAL_TRANSPORT_PREFLIGHT_PASS_ROOT_RO; exit 0; fi
if [ "$mode" = install ] && [ "$current" = "$NEW_SHA" ]; then echo ALREADY_INSTALLED_ROOT_RO; exit 0; fi
if [ "$mode" = rollback ]; then match "$BACKUP" "$OLD_SHA" || die MISSING_PREVIOUS_DSO; fi
if pidof entry-aad aa-supervisor >/dev/null 2>&1; then die AA_RUNNING_STOP_FIRST; fi
if pidof vehicle-telemetry-most >/dev/null 2>&1; then die TRANSPORT_RUNNING_STOP_FIRST; fi
if [ "$mode" = install ]; then loader; fi
mkdir "$LOCK" || die LOCK_PRESENT
printf '%s\n' "$ID" >"$LOCK/owner"; printf '%s\n' "$$" >"$LOCK/pid"
boot_id >"$LOCK/boot"; process_start "$$" >"$LOCK/start"
original_signature=$(signature); original_flags=$(root_flags); rw=0
restore_old() {
 for path in "$PLUGIN/.most-new" "$HELPER/.most-new" "$UNIT.new"; do
  if ! absent "$path"; then regular "$path" && rm "$path" || return 1; fi
 done
 if ! match "$PLUGIN/libVehicleTelemetry.so" "$OLD_SHA"; then
  match "$BACKUP" "$OLD_SHA" || return 1
  cp "$BACKUP" "$PLUGIN/.most-new" && chmod 755 "$PLUGIN/.most-new" &&
  match "$PLUGIN/.most-new" "$OLD_SHA" && mv "$PLUGIN/.most-new" "$PLUGIN/libVehicleTelemetry.so" || return 1
 fi
 if [ -L "$WANTS" ] && [ "$(readlink "$WANTS")" = ../vehicle-telemetry-most.service ]; then rm "$WANTS" || return 1; fi
 if match "$UNIT" "$UNIT_SHA"; then rm "$UNIT" || return 1; fi
 if match "$HELPER/vehicle-telemetry-most" "$HELPER_SHA"; then rm "$HELPER/vehicle-telemetry-most" || return 1; fi
 if [ -d "$HELPER" ] && [ ! -L "$HELPER" ]; then rmdir "$HELPER" || return 1; fi
}
cleanup() {
 rc=$?; set +eu; trap - 0 HUP INT TERM
 if [ "$rw" = 1 ]; then
  if [ "$rc" != 0 ] && [ "$(signature)" = "$original_signature" ]; then
   root_ro && remount_root rw
   restore_old || { echo ROLLBACK_REQUIRES_REVIEW >&2; rc=1; }
  fi
  bounded 15 sync || rc=1
  restore_ro || { echo ROOT_RO_RESTORE_FAILED >&2; rc=1; }
 fi
 if lock_active && [ "$(cat "$LOCK/pid")" = "$$" ]; then
  rm "$LOCK/pid" "$LOCK/boot" "$LOCK/start" "$LOCK/owner"; rmdir "$LOCK" || rc=1
 fi
 exit "$rc"
}
trap cleanup 0
trap 'exit 130' HUP INT TERM
rw=1; remount_root rw || die ROOT_REMOUNT_FAILED
[ "$(signature)" = "$original_signature" ] || die ROOT_IDENTITY_CHANGED
if [ "$mode" = install ]; then
 if absent "$BACKUP"; then ln "$PLUGIN/libVehicleTelemetry.so" "$BACKUP"; fi
 match "$BACKUP" "$OLD_SHA" || die BACKUP_MISMATCH
 if absent "$HELPER"; then mkdir -m 755 "$HELPER"; fi
 cp "$KIT/transport/vehicle-telemetry-most" "$HELPER/.most-new"; chmod 755 "$HELPER/.most-new"
 match "$HELPER/.most-new" "$HELPER_SHA" || die HELPER_READBACK_FAILED
 mv "$HELPER/.most-new" "$HELPER/vehicle-telemetry-most"
 cp "$KIT/transport/vehicle-telemetry-most.service" "$UNIT.new"; chmod 644 "$UNIT.new"
 match "$UNIT.new" "$UNIT_SHA" || die UNIT_READBACK_FAILED
 mv "$UNIT.new" "$UNIT"
 if absent "$WANTS"; then ln -s ../vehicle-telemetry-most.service "$WANTS"; fi
 cp "$KIT/payload/libVehicleTelemetry.so" "$PLUGIN/.most-new"; chmod 755 "$PLUGIN/.most-new"
 match "$PLUGIN/.most-new" "$NEW_SHA" || die DSO_READBACK_FAILED
 mv "$PLUGIN/.most-new" "$PLUGIN/libVehicleTelemetry.so"
 verify "$KIT/payload.sha256" "$PLUGIN" || die FINAL_READBACK_FAILED
else
 restore_old || die ROLLBACK_FAILED
fi
bounded 15 sync
restore_ro || die ROOT_RO_RESTORE_FAILED
rw=0
echo "INTERNAL_TRANSPORT_${mode}_PASS_ROOT_RO_PREVIOUS_DSO_PRESERVED"
echo NO_REBOOT_SENT
