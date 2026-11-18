#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Complete resources/DSO upgrade. Previous directory/inodes remain recoverable.
set -eu
new=$1; old=$2; mode=${3:-check}; activation=${4:-persistent-startup-guard}; network=${5:-vehicle-egs-hsfz-v1}
. "$new/remote/common.sh"
KIT=$new; OLD=$old
case "$mode" in check|upgrade|rollback) ;; *) die UNKNOWN_OPERATION;; esac
[ "$activation" = persistent-startup-guard ] || die UNKNOWN_POLICY
case "$network" in none|vehicle-egs-hsfz-v1) ;; *) die UNKNOWN_POLICY;; esac
verify_kit; verify "$OLD/REMOTE_SHA256SUMS" "$OLD" || die OLD_KIT_CHANGED
firmware; layout; root_ro || die ROOT_RW_RECOVERY_REQUIRED
OLD_SHA=$(sha "$OLD/payload/libVehicleTelemetry.so")
case "$OLD_SHA" in
 955e866a10a6b3199d26c97dc1510dc81ee44852bca7eff76396253d4439f172) ;;
 445d920cebf869d0b0c8467c5e8fcbac0467c6b61b0c205fed88fa9c8ea59069) ;;
 f8eb7a0e6b416693fb6f83322be66ff0a7191a713ca0d4932acb84189c1b7157|a0991707d7c3484b392ec42c43bd91e84ae760fe47fad1760468670f4cada2da) ;;
 *) die WRONG_PREDECESSOR;; esac
if [ "$network" = vehicle-egs-hsfz-v1 ]; then
 [ "$OLD_SHA" = f8eb7a0e6b416693fb6f83322be66ff0a7191a713ca0d4932acb84189c1b7157 ] || die WRONG_NETWORK_PREDECESSOR
fi
NEW_SHA=$(sha "$KIT/payload/libVehicleTelemetry.so")
[ "$NEW_SHA" != "$OLD_SHA" ] || die NO_NEW_BUILD
BACKUP=$PARENT/.VehicleTelemetry.before-sources-$NEW_SHA
STAGED=$PARENT/.VehicleTelemetry.stage-sources-$NEW_SHA
SAVED=$PARENT/.VehicleTelemetry.after-rollback-$NEW_SHA
FW=/etc/sysconfig/iptables.rules
IPT=/usr/sbin/iptables
FW_OLD=68e8de7833988c31ec159648fd3c84e88234fc91584c9bf4742e3cfdd518bb19
FW_NEW=1b285bedafabd047f5f95890c1836f8f1747ab2163b68c5dd4f0a15171d64061
FW_BACKUP=$DATA/iptables.rules.before-sources-v2
FW_TEMP=${FW}.vt146-sources-new
match() { regular "$1" && [ "$(sha "$1")" = "$2" ]; }
bundle() {
 bundle_kit=$1; bundle_dir=$2
 [ -d "$bundle_dir" ] && [ ! -L "$bundle_dir" ] || return 1
 for bundle_path in "$bundle_dir"/* "$bundle_dir"/.[!.]* "$bundle_dir"/..?*; do
  [ -e "$bundle_path" ] || [ -L "$bundle_path" ] || continue
  case "${bundle_path##*/}" in libVehicleTelemetry.so|VehicleTelemetry.xml|VehicleTelemetry_Texts.zip|VehicleTelemetry_Images.zip|VehicleTelemetry.manifest|VehicleTelemetry.registration-disabled|.vehicle-telemetry-owner) ;; *) return 1;; esac
  regular "$bundle_path" || return 1
 done
 while read -r bundle_sha bundle_name; do
  bundle_path=$bundle_dir/$bundle_name
  if [ "$bundle_name" = VehicleTelemetry.manifest ] && [ ! -e "$bundle_path" ]; then bundle_path=$bundle_dir/$DISABLED; fi
  match "$bundle_path" "$bundle_sha" || return 1
 done <"$bundle_kit/payload.sha256"
}
fw_rule() { "$IPT" "$1" OUTPUT ${2:-} -o vbr0 -s 169.254.1.0/32 -d 169.254.76.216/32 -p tcp --dport 6801 -j ACCEPT; }
[ ! -e "$LOCK" ] && [ ! -L "$LOCK" ] || die LOCK_PRESENT
[ ! -e "$STAGED" ] && [ ! -L "$STAGED" ] || die STAGING_EXISTS_PRESERVE_STATE
[ ! -e "$FW_TEMP" ] && [ ! -L "$FW_TEMP" ] || die FIREWALL_TEMP_EXISTS
match "$KIT/network/iptables.rules.old" "$FW_OLD" && match "$KIT/network/iptables.rules.new" "$FW_NEW" || die NETWORK_PAYLOAD_CHANGED
if [ "$mode" = rollback ]; then
 bundle "$OLD" "$BACKUP" || bundle "$OLD" "$PLUGIN" || die PREVIOUS_BUNDLE_NOT_AVAILABLE
 match "$FW" "$FW_OLD" || match "$FW" "$FW_NEW" || die UNKNOWN_FIREWALL
else
 owned || die UNKNOWN_PLUGIN_OWNER
 if bundle "$KIT" "$PLUGIN"; then
  match "$FW" "$FW_NEW" || die FIREWALL_BUNDLE_MISMATCH
  echo ALREADY_UPDATED_NO_REARM; exit 0
 fi
 bundle "$OLD" "$PLUGIN" || die INSTALLED_BUNDLE_CHANGED
 [ ! -e "$BACKUP" ] && [ ! -L "$BACKUP" ] || die BACKUP_EXISTS_RESTART_AND_INSPECT
 [ ! -e "$SAVED" ] && [ ! -L "$SAVED" ] || die PREVIOUS_ROLLBACK_REQUIRES_RESTART
 if [ "$network" = none ]; then
  match "$FW" "$FW_NEW" && fw_rule -C || die EXISTING_DIAGNOSTIC_NETWORK_NOT_VERIFIED
 else
  match "$FW" "$FW_OLD" || die UNKNOWN_FIREWALL
  if fw_rule -C 2>/dev/null; then die UNEXPECTED_EXISTING_RUNTIME_RULE; fi
 fi
fi
if [ -e "$FW_BACKUP" ] || [ -L "$FW_BACKUP" ]; then match "$FW_BACKUP" "$FW_OLD" || die UNKNOWN_FIREWALL_BACKUP; fi
space
if [ "$mode" != rollback ]; then
 loader
 [ ! -e "$DATA/persistent.disabled" ] && [ ! -L "$DATA/persistent.disabled" ] || die PREVIOUS_GUARD_FAILURE_REQUIRES_REVIEW
 for guard_file in persistent.enabled persistent.state persistent.lock trial-arm trial-consumed trial.log trial.log.previous; do
  if [ -e "$DATA/$guard_file" ] || [ -L "$DATA/$guard_file" ]; then regular "$DATA/$guard_file" || die UNKNOWN_GUARD_FILE; fi
 done
fi
if [ "$mode" != rollback ] && pidof entry-aad aa-supervisor >/dev/null 2>&1; then die AA_RUNNING_STOP_FIRST; fi
if [ "$mode" = check ]; then
 echo COMPLETE_BUNDLE_CHECK_PASS_ROOT_RO_NO_INSTALL
 exit 0
fi
mkdir "$LOCK" || die LOCK_PRESENT
printf '%s\n' "$ID" >"$LOCK/owner"; printf '%s\n' "$$" >"$LOCK/pid"
boot_id >"$LOCK/boot"; process_start "$$" >"$LOCK/start"
original_signature=$(signature); original_flags=$(root_flags); rw=0; fw_temp_owned=0
disable_registration() {
 if [ -e "$PLUGIN/VehicleTelemetry.manifest" ]; then
  manifest_valid "$PLUGIN/VehicleTelemetry.manifest" || return 1
  [ ! -e "$PLUGIN/$DISABLED" ] && [ ! -L "$PLUGIN/$DISABLED" ] || return 1
  mv "$PLUGIN/VehicleTelemetry.manifest" "$PLUGIN/$DISABLED"
 fi
}
cancel_enabled() {
 if [ -e "$DATA/persistent.enabled" ] || [ -L "$DATA/persistent.enabled" ]; then
  regular "$DATA/persistent.enabled" && [ "$(cat "$DATA/persistent.enabled")" = "$PERSISTENT_TOKEN" ] || return 1
  rm "$DATA/persistent.enabled"
 fi
}
restore_bundle() {
 if bundle "$OLD" "$PLUGIN"; then return 0; fi
 bundle "$OLD" "$BACKUP" || return 1
 if [ -e "$PLUGIN" ] || [ -L "$PLUGIN" ]; then
  bundle "$KIT" "$PLUGIN" || return 1
  [ ! -e "$SAVED" ] && [ ! -L "$SAVED" ] || return 1
  mv "$PLUGIN" "$SAVED" || return 1
 fi
 mv "$BACKUP" "$PLUGIN" && bundle "$OLD" "$PLUGIN"
}
restore_firewall() {
 [ "$network" != none ] || { match "$FW" "$FW_NEW" && fw_rule -C; return; }
 if [ "$fw_temp_owned" = 1 ] && [ -e "$FW_TEMP" ]; then regular "$FW_TEMP" && rm "$FW_TEMP" || return 1; fw_temp_owned=0; fi
 if match "$FW" "$FW_NEW"; then
  match "$FW_BACKUP" "$FW_OLD" || return 1
  cp "$FW_BACKUP" "$FW_TEMP" && chmod 644 "$FW_TEMP" && match "$FW_TEMP" "$FW_OLD" && mv "$FW_TEMP" "$FW" || return 1
 else match "$FW" "$FW_OLD" || return 1; fi
 if fw_rule -C 2>/dev/null; then fw_rule -D || return 1; fi
 if fw_rule -C 2>/dev/null; then return 1; fi
}
cleanup() {
 rc=$?; set +eu; trap - 0 HUP INT TERM
 if [ "$rw" = 1 ]; then
  if [ "$rc" != 0 ] && [ "$(signature)" = "$original_signature" ]; then
   if root_ro; then remount_root rw || rc=1; fi
   disable_registration || rc=1
   cancel_enabled || rc=1
   restore_bundle || { echo PREVIOUS_BUNDLE_RESTORE_NEEDS_REVIEW >&2; rc=1; }
   restore_firewall || { echo FIREWALL_RESTORE_NEEDS_REVIEW >&2; rc=1; }
   phase sources-failed-registration-disabled || :
  fi
  bounded 15 sync || rc=1
  restore_ro || { echo ROOT_RO_RESTORE_FAILED >&2; rc=1; }
 fi
 if root_ro && lock_active && [ "$(cat "$LOCK/pid")" = "$$" ]; then
  rm "$LOCK/pid" "$LOCK/boot" "$LOCK/start" "$LOCK/owner"; rmdir "$LOCK" || rc=1
 fi
 exit "$rc"
}
trap cleanup 0
trap 'exit 130' HUP INT TERM
for file in .phase-new root-signature root-flags phase; do
 [ ! -e "$DATA/$file" ] && [ ! -L "$DATA/$file" ] || regular "$DATA/$file" || die UNKNOWN_JOURNAL_FILE
done
printf '%s\n' "$original_signature" >"$DATA/root-signature"
printf '%s\n' "$original_flags" >"$DATA/root-flags"
phase sources-prepared
rw=1; remount_root rw || die ROOT_REMOUNT_FAILED
[ "$(signature)" = "$original_signature" ] || die ROOT_IDENTITY_CHANGED
disable_registration || die CANNOT_DISABLE_REGISTRATION
cancel_enabled || die UNKNOWN_ACTIVATION_FILE
if [ "$mode" = upgrade ]; then
 mkdir "$STAGED"; chmod 755 "$STAGED"
 while read -r expected name; do
  target=$STAGED/$name
  if [ "$name" = VehicleTelemetry.manifest ]; then target=$STAGED/$DISABLED; fi
  cp "$KIT/payload/$name" "$target"; chmod 644 "$target"
  match "$target" "$expected" || die STAGED_FILE_MISMATCH
 done <"$KIT/payload.sha256"
 chmod 755 "$STAGED/libVehicleTelemetry.so"; chmod 600 "$STAGED/.vehicle-telemetry-owner"
 bundle "$KIT" "$STAGED" || die STAGED_BUNDLE_MISMATCH
 # Directory renames preserve all mapped/held old inodes, including DSO/ZIPs.
 mv "$PLUGIN" "$BACKUP"; mv "$STAGED" "$PLUGIN"
 bundle "$OLD" "$BACKUP" && bundle "$KIT" "$PLUGIN" || die BUNDLE_READBACK_FAILED
 if [ "$network" != none ]; then
  if [ ! -e "$FW_BACKUP" ]; then cp "$FW" "$FW_BACKUP"; chmod 600 "$FW_BACKUP"; fi
  match "$FW_BACKUP" "$FW_OLD" || die FIREWALL_BACKUP_FAILED
  fw_temp_owned=1; cp "$KIT/network/iptables.rules.new" "$FW_TEMP"; chmod 644 "$FW_TEMP"
  match "$FW_TEMP" "$FW_NEW" || die FIREWALL_STAGED_MISMATCH
  mv "$FW_TEMP" "$FW"; fw_temp_owned=0
  fw_rule -I 1 && fw_rule -C || die FIREWALL_RUNTIME_FAILED
 fi
 suffix=$NEW_SHA-$(boot_id)-$$
 for file in trial-arm trial-consumed trial.log trial.log.previous persistent.disabled persistent.state persistent.state.new persistent.lock; do
  if [ -e "$DATA/$file" ] || [ -L "$DATA/$file" ]; then
   regular "$DATA/$file" || die UNKNOWN_GUARD_FILE
   [ ! -e "$DATA/$file.saved-$suffix" ] && [ ! -L "$DATA/$file.saved-$suffix" ] || die GUARD_ARCHIVE_EXISTS
   mv "$DATA/$file" "$DATA/$file.saved-$suffix"
  fi
 done
 (umask 077; set -C; printf '%s\n' "$PERSISTENT_TOKEN" >"$DATA/persistent.enabled")
 bounded 15 sync
 mv "$PLUGIN/$DISABLED" "$PLUGIN/VehicleTelemetry.manifest"
 verify "$KIT/payload.sha256" "$PLUGIN" || die FINAL_READBACK_FAILED
 phase sources-upgrade-complete
else
 restore_bundle || die BUNDLE_ROLLBACK_FAILED
 disable_registration || die OLD_REGISTRATION_STILL_ACTIVE
 restore_firewall || die FIREWALL_ROLLBACK_FAILED
 phase sources-rollback-registration-disabled
fi
bounded 15 sync; restore_ro || die ROOT_RO_RESTORE_FAILED
rw=0
echo COMPLETE_BUNDLE_OPERATION_PASS_ROOT_RO_PREVIOUS_INODES_PRESERVED
echo NO_REBOOT_SENT
