#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# DSO update; optional vehicle-pinned EGS firewall rule with exact rollback.
set -eu
new=$1; old=$2; mode=${3:-check}; activation=${4:-single-start}; network=${5:-none}
. "$new/remote/common.sh"
KIT=$new; OLD=$old
case "$mode" in check|upgrade|rollback) ;; *) die UNKNOWN_UPGRADE_OPERATION;; esac
case "$activation" in single-start|persistent-startup-guard) ;; *) die UNKNOWN_ACTIVATION_POLICY;; esac
case "$network" in none|vehicle-egs-hsfz-v1) ;; *) die UNKNOWN_EGS_NETWORK_POLICY;; esac
PERSISTENT_TOKEN=vehicle-telemetry-146-persistent-v1
verify_kit
verify "$OLD/REMOTE_SHA256SUMS" "$OLD" || die OLD_KIT_CHANGED
firmware; layout
root_ro || die ROOT_RW_RECOVERY_REQUIRED
OLD_SHA=$(sha "$OLD/payload/libVehicleTelemetry.so")
case "$OLD_SHA" in
 a0991707d7c3484b392ec42c43bd91e84ae760fe47fad1760468670f4cada2da|f8eb7a0e6b416693fb6f83322be66ff0a7191a713ca0d4932acb84189c1b7157|3299b9063522c964023795741bb61baebf0509ec311cd5a3ecb9aca05e66102e|224a8d206e029a17742624a34670beeb6a976c2b0bc0345907c68ee5998a5754) ;;
 *) die NOT_APPROVED_PREVIOUS_BUILD;; esac
NEW_SHA=$(sha "$KIT/payload/libVehicleTelemetry.so")
[ "$NEW_SHA" != "$OLD_SHA" ] || die NO_NEW_BUILD
BACKUP=$PARENT/.VehicleTelemetry.rollback-$OLD_SHA.so
SAVED_NEW=$PARENT/.VehicleTelemetry.continuous-$NEW_SHA.so
TEMP=$PLUGIN/.lib-continuous-new
RESTORE=$PLUGIN/.lib-restore
match_file() { regular "$1" && [ "$(sha "$1")" = "$2" ]; }
for path in "$BACKUP" "$SAVED_NEW"; do
 if [ -e "$path" ] || [ -L "$path" ]; then
  case "$path" in "$BACKUP") match_file "$path" "$OLD_SHA";; *) match_file "$path" "$NEW_SHA";; esac || die UNKNOWN_BACKUP_PRESERVED
 fi
done
# This updater must never change resources of a running application.
while read -r expected relative; do
 [ "$relative" != libVehicleTelemetry.so ] || continue
 match_file "$KIT/payload/$relative" "$expected" || die NEW_RESOURCE_CHANGED
 match_file "$OLD/payload/$relative" "$expected" || die RESOURCE_UPDATE_NOT_SUPPORTED
 if [ "$relative" = VehicleTelemetry.manifest ]; then continue; fi
 match_file "$PLUGIN/$relative" "$expected" || die INSTALLED_RESOURCE_CHANGED
done <"$KIT/payload.sha256"
owned || die UNKNOWN_PLUGIN_OWNER
for path in "$PLUGIN"/* "$PLUGIN"/.[!.]* "$PLUGIN"/..?*; do
 [ -e "$path" ] || [ -L "$path" ] || continue
 case "${path##*/}" in libVehicleTelemetry.so|VehicleTelemetry.xml|VehicleTelemetry_Texts.zip|VehicleTelemetry_Images.zip|VehicleTelemetry.manifest|VehicleTelemetry.registration-disabled|.vehicle-telemetry-owner) ;; *) die UNKNOWN_PLUGIN_FILE;; esac
 [ ! -L "$path" ] || die PLUGIN_SYMLINK
done
for path in "$PLUGIN/VehicleTelemetry.manifest" "$PLUGIN/$DISABLED"; do
 if [ -e "$path" ] || [ -L "$path" ]; then manifest_valid "$path" || die UNKNOWN_MANIFEST; fi
done
current=$(sha "$PLUGIN/libVehicleTelemetry.so")
case "$current" in "$OLD_SHA"|"$NEW_SHA") ;; *) die UNKNOWN_INSTALLED_DSO;; esac
[ ! -e "$LOCK" ] && [ ! -L "$LOCK" ] || die LOCK_PRESENT

FW=/etc/sysconfig/iptables.rules
IPT=/usr/sbin/iptables
FW_OLD=68e8de7833988c31ec159648fd3c84e88234fc91584c9bf4742e3cfdd518bb19
FW_NEW=1b285bedafabd047f5f95890c1836f8f1747ab2163b68c5dd4f0a15171d64061
FW_BACKUP=$DATA/iptables.rules.before-egs-v1
FW_TEMP=${FW}.vt146-new
fw_temp_owned=0
fw_rule() {
 "$IPT" "$1" OUTPUT ${2:-} -o vbr0 -s 169.254.1.0/32 -d 169.254.76.216/32 -p tcp --dport 6801 -j ACCEPT
}
fw_check() {
 [ "$network" = vehicle-egs-hsfz-v1 ] || return 0
 [ "$OLD_SHA" = f8eb7a0e6b416693fb6f83322be66ff0a7191a713ca0d4932acb84189c1b7157 ] || die WRONG_EGS_PREDECESSOR
 match_file "$FW" "$FW_OLD" || match_file "$FW" "$FW_NEW" || die UNKNOWN_FIREWALL_CONFIGURATION
 if [ -e "$FW_BACKUP" ] || [ -L "$FW_BACKUP" ]; then match_file "$FW_BACKUP" "$FW_OLD" || die UNKNOWN_FIREWALL_BACKUP; fi
 if match_file "$FW" "$FW_NEW"; then match_file "$FW_BACKUP" "$FW_OLD" || die MISSING_FIREWALL_BACKUP; fi
 [ ! -e "$FW_TEMP" ] && [ ! -L "$FW_TEMP" ] || die FIREWALL_TEMP_EXISTS
 if [ "$mode" != rollback ]; then
  if [ "$current" = "$OLD_SHA" ]; then match_file "$FW" "$FW_OLD" || die FIREWALL_DSO_STATE_MISMATCH
  else match_file "$FW" "$FW_NEW" || die FIREWALL_DSO_STATE_MISMATCH; fi
 fi
}
fw_install() {
 [ "$network" = vehicle-egs-hsfz-v1 ] || return 0
 match_file "$FW" "$FW_OLD" || return 1
 if [ ! -e "$FW_BACKUP" ]; then cp "$FW" "$FW_BACKUP" || return 1; chmod 600 "$FW_BACKUP"; fi
 match_file "$FW_BACKUP" "$FW_OLD" || return 1
 fw_temp_owned=1
 awk '/^COMMIT$/ {print "-A OUTPUT -o vbr0 -s 169.254.1.0/32 -d 169.254.76.216/32 -p tcp --dport 6801 -j ACCEPT"} {print}' "$FW" >"$FW_TEMP" || return 1
 chmod 644 "$FW_TEMP" || return 1
 match_file "$FW_TEMP" "$FW_NEW" || return 1
 mv "$FW_TEMP" "$FW" || return 1
 fw_temp_owned=0
 fw_rule -C 2>/dev/null || fw_rule -I 1 || return 1
 fw_rule -C || return 1
 echo EGS_FIREWALL_INSTALLED_EXACT_DESTINATION_ONLY
}
fw_restore() {
 [ "$network" = vehicle-egs-hsfz-v1 ] || return 0
 if [ "$fw_temp_owned" = 1 ] && [ -e "$FW_TEMP" ]; then regular "$FW_TEMP" && rm "$FW_TEMP" || return 1; fw_temp_owned=0; fi
 if match_file "$FW" "$FW_NEW"; then
  match_file "$FW_BACKUP" "$FW_OLD" || return 1
  [ ! -e "$FW_TEMP" ] && [ ! -L "$FW_TEMP" ] || return 1
  cp "$FW_BACKUP" "$FW_TEMP" && chmod 644 "$FW_TEMP" && match_file "$FW_TEMP" "$FW_OLD" && mv "$FW_TEMP" "$FW" || return 1
 else match_file "$FW" "$FW_OLD" || return 1; fi
 if fw_rule -C 2>/dev/null; then fw_rule -D || return 1; fi
 if fw_rule -C 2>/dev/null; then return 1; fi
 echo EGS_FIREWALL_ORIGINAL_RESTORED
}
fw_check
if [ "$mode" = check ]; then
 space; loader
 echo DSO_ONLY_UPGRADE_CHECK_PASS_ROOT_RO
 exit 0
fi
if [ "$mode" = upgrade ]; then
 [ "$current" != "$NEW_SHA" ] || { echo ALREADY_UPDATED_NO_REARM; exit 0; }
 space
 if pidof entry-aad aa-supervisor >/dev/null 2>&1; then die AA_RUNNING_STOP_FIRST; fi
 loader
 if [ -e "$DATA/persistent.state" ] || [ -L "$DATA/persistent.state" ]; then
  regular "$DATA/persistent.state" || die UNKNOWN_PERSISTENT_STATE
  read -r magic saved_boot saved_pid rest <"$DATA/persistent.state"
  [ "$magic" = VT146P1 ] || die UNKNOWN_PERSISTENT_STATE
  case "$saved_pid" in ''|0|*[!0-9]*) die UNKNOWN_PERSISTENT_PID;; esac
  if [ -e "$SAVED_NEW" ] && [ "$saved_boot" = "$(boot_id)" ] && kill -0 "$saved_pid" 2>/dev/null; then die RESTART_AFTER_PREVIOUS_ROLLBACK_FIRST; fi
 fi
fi
mkdir "$LOCK" || die LOCK_PRESENT
printf '%s\n' "$ID" >"$LOCK/owner"
printf '%s\n' "$$" >"$LOCK/pid"
boot_id >"$LOCK/boot"; process_start "$$" >"$LOCK/start"
original_signature=$(signature); original_flags=$(root_flags); rw=0
disable_registration() {
 if [ -e "$PLUGIN/VehicleTelemetry.manifest" ]; then
  manifest_valid "$PLUGIN/VehicleTelemetry.manifest" || return 1
  mv "$PLUGIN/VehicleTelemetry.manifest" "$PLUGIN/$DISABLED"
 fi
}
cancel_arm() {
 if [ -e "$DATA/trial-arm" ] || [ -L "$DATA/trial-arm" ]; then
  regular "$DATA/trial-arm" && [ "$(cat "$DATA/trial-arm")" = "$TRIAL_TOKEN" ] || return 1
  rm "$DATA/trial-arm"
 fi
}
cancel_persistent() {
 if [ -e "$DATA/persistent.enabled" ] || [ -L "$DATA/persistent.enabled" ]; then
  regular "$DATA/persistent.enabled" && [ "$(cat "$DATA/persistent.enabled")" = "$PERSISTENT_TOKEN" ] || return 1
  rm "$DATA/persistent.enabled"
 fi
}
restore_previous() {
 if match_file "$PLUGIN/libVehicleTelemetry.so" "$OLD_SHA"; then return 0; fi
 match_file "$PLUGIN/libVehicleTelemetry.so" "$NEW_SHA" && match_file "$BACKUP" "$OLD_SHA" || return 1
 if [ ! -e "$SAVED_NEW" ]; then ln "$PLUGIN/libVehicleTelemetry.so" "$SAVED_NEW" || return 1; fi
 match_file "$SAVED_NEW" "$NEW_SHA" || return 1
 [ ! -e "$RESTORE" ] && [ ! -L "$RESTORE" ] || return 1
 ln "$BACKUP" "$RESTORE" && mv "$RESTORE" "$PLUGIN/libVehicleTelemetry.so"
}
cleanup_upgrade() {
 rc=$?; set +eu; trap - 0 HUP INT TERM
 if [ "$rw" = 1 ]; then
  if [ "$rc" != 0 ] && [ "$(signature)" = "$original_signature" ]; then
   if root_ro; then remount_root rw || rc=1; fi
   disable_registration || rc=1
   cancel_arm || rc=1
   cancel_persistent || rc=1
   restore_previous || { echo PREVIOUS_DSO_RESTORE_NEEDS_REVIEW >&2; rc=1; }
   fw_restore || { echo FIREWALL_RESTORE_NEEDS_REVIEW >&2; rc=1; }
   phase upgrade-failed-registration-disabled || :
  fi
  bounded 15 sync || rc=1
  restore_ro || { echo ROOT_RO_RESTORE_FAILED >&2; rc=1; }
 fi
 if lock_active && [ "$(cat "$LOCK/pid")" = "$$" ]; then
  rm "$LOCK/pid" "$LOCK/boot" "$LOCK/start" "$LOCK/owner"; rmdir "$LOCK" || rc=1
 fi
 exit "$rc"
}
trap cleanup_upgrade 0
trap 'exit 130' HUP INT TERM
for file in .phase-new root-signature root-flags phase; do
 [ ! -e "$DATA/$file" ] && [ ! -L "$DATA/$file" ] || regular "$DATA/$file" || die UNKNOWN_JOURNAL_FILE
done
printf '%s\n' "$original_signature" >"$DATA/root-signature"
printf '%s\n' "$original_flags" >"$DATA/root-flags"
phase "$mode-prepared"
rw=1; remount_root rw || die ROOT_REMOUNT_FAILED
[ "$(signature)" = "$original_signature" ] || die ROOT_IDENTITY_CHANGED
disable_registration || die CANNOT_DISABLE_REGISTRATION
cancel_arm || die UNKNOWN_ARM
phase "$mode-writing"
if [ "$mode" = upgrade ]; then
 if [ ! -e "$BACKUP" ]; then ln "$PLUGIN/libVehicleTelemetry.so" "$BACKUP"; fi
 match_file "$BACKUP" "$OLD_SHA" || die BACKUP_MISMATCH
 [ ! -e "$TEMP" ] && [ ! -L "$TEMP" ] || die STAGED_DSO_EXISTS
 cp "$KIT/payload/libVehicleTelemetry.so" "$TEMP"; chmod 755 "$TEMP"
 match_file "$TEMP" "$NEW_SHA" || die STAGED_DSO_MISMATCH
 # Atomic rename keeps both old and new mapped inodes intact via backup links.
 mv "$TEMP" "$PLUGIN/libVehicleTelemetry.so"
 resources_valid || die NEW_READBACK_FAILED
 fw_install || die EGS_FIREWALL_INSTALL_FAILED
 suffix=$NEW_SHA-$(boot_id)-$$
 for file in trial-consumed trial.log trial.log.previous persistent.enabled persistent.disabled persistent.state persistent.state.new persistent.lock; do
  if [ -e "$DATA/$file" ] || [ -L "$DATA/$file" ]; then
   regular "$DATA/$file" || die UNKNOWN_TRIAL_FILE
   if [ "$file" = trial-consumed ]; then [ "$(cat "$DATA/$file")" = "$TRIAL_TOKEN" ] || die UNKNOWN_CONSUMED_PERMIT; fi
   if [ "$file" = persistent.enabled ]; then [ "$(cat "$DATA/$file")" = "$PERSISTENT_TOKEN" ] || die UNKNOWN_PERSISTENT_ENABLE; fi
   [ ! -e "$DATA/$file.saved-$suffix" ] && [ ! -L "$DATA/$file.saved-$suffix" ] || die TRIAL_ARCHIVE_EXISTS
   mv "$DATA/$file" "$DATA/$file.saved-$suffix"
  fi
 done
 if [ "$activation" = persistent-startup-guard ]; then
  (umask 077; set -C; printf '%s\n' "$PERSISTENT_TOKEN" >"$DATA/persistent.enabled")
  chmod 600 "$DATA/persistent.enabled"
 else
  (umask 077; set -C; printf '%s\n' "$TRIAL_TOKEN" >"$DATA/trial-arm")
  chmod 600 "$DATA/trial-arm"
 fi
 bounded 15 sync
 # Exactly the same manifest/resources; registration published last.
 mv "$PLUGIN/$DISABLED" "$PLUGIN/VehicleTelemetry.manifest"
 verify "$KIT/payload.sha256" "$PLUGIN" || die FINAL_READBACK_FAILED
else
 cancel_persistent || die UNKNOWN_PERSISTENT_ENABLE
 fw_restore || die EGS_FIREWALL_ROLLBACK_FAILED
 restore_previous || die ROLLBACK_FAILED
 [ ! -e "$PLUGIN/VehicleTelemetry.manifest" ] || die REGISTRATION_STILL_ACTIVE
fi
bounded 15 sync
restore_ro || die ROOT_RO_RESTORE_FAILED
rw=0; phase "$mode-complete"
echo "${mode}_COMPLETE_ROOT_RO_PREVIOUS_DSO_PRESERVED"
echo "ACTIVATION=$activation"
echo NO_REBOOT_SENT
