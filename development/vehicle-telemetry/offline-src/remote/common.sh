#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
KIT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ID=vehicle-telemetry-146-v1
PARENT=/opt/hmi/data/ro/Plugins
PLUGIN=$PARENT/VehicleTelemetry
DISABLED=VehicleTelemetry.registration-disabled
DATA=/var/local/data/VehicleTelemetry146
LOCK=/tmp/vehicle-telemetry-146-v1.lock
FIRMWARE_ROOT=/
TRIAL_TOKEN=vehicle-telemetry-146-single-start-v1
PERSISTENT_TOKEN=vehicle-telemetry-146-persistent-v1
ACTIVATION_POLICY=persistent-startup-guard
die() { echo "STOP: $*" >&2; exit 1; }
regular() { [ -f "$1" ] && [ ! -L "$1" ]; }
sha() {
    if command -v sha256sum >/dev/null 2>&1; then
        line=$(sha256sum "$1") || return 1; value=${line%% *}
    else
        line=$(openssl dgst -sha256 "$1") || return 1; value=${line##* }
    fi
    [ "${#value}" -eq 64 ] || return 1
    case "$value" in *[!0-9a-f]*) return 1;; esac
    printf '%s\n' "$value"
}
verify() {
    count=0
    while read -r expected relative; do
        case "$relative" in ''|/*|*..*) return 1;; esac
        regular "$2/$relative" || return 1
        [ "$(sha "$2/$relative")" = "$expected" ] || return 1
        count=$((count+1))
    done <"$1"
    [ "$count" -gt 0 ]
}
verify_kit() { verify "$KIT/REMOTE_SHA256SUMS" "$KIT" || die KIT_CHECKSUM_MISMATCH; }
firmware() {
    # Read-only fingerprinting may follow vendor library links; no BMW file is written.
    while read -r expected relative; do
        [ -f "$FIRMWARE_ROOT/$relative" ] && [ "$(sha "$FIRMWARE_ROOT/$relative")" = "$expected" ] || die FIRMWARE_146_MISMATCH
    done <"$KIT/firmware.sha256"
}
mount_row() {
    awk -v p="$1" '($2=="/" || p==$2 || index(p,$2"/")==1) && length($2)>=n {n=length($2);r=$0} END{print r}' /proc/mounts
}
root_flags() { mount_row / | awk '{print $4}'; }
# This HU's BusyBox 1.21.1 grep -x rejects even an exact "ro" line.
# Match mount options as fields instead of depending on that broken applet.
has_ro_flag() { awk -F, '{for(i=1;i<=NF;i++)if($i=="ro")found=1} END{exit !found}'; }
root_ro() { root_flags | has_ro_flag; }
signature() {
    row=$(mount_row /)
    printf '%s\n' "$row" | awk '{print $1 "|" $3}'
    awk '$5=="/" {print $3;exit}' /proc/self/mountinfo
}
layout() {
    [ "$(id -u)" = 0 ] || die ROOT_REQUIRED
    case "$(mount_row "$KIT" | awk '{print $3}')" in tmpfs|ramfs) ;; *) die RAM_STAGE_REQUIRED;; esac
    [ "$(mount_row / | awk '{print $3}')" = ext4 ] || die UNSUPPORTED_ROOT_FS
    for path in /opt /opt/hmi /opt/hmi/data /opt/hmi/data/ro "$PARENT"; do
        [ -d "$path" ] && [ ! -L "$path" ] || die "UNEXPECTED_PATH: $path"
        [ "$(mount_row "$path")" = "$(mount_row /)" ] || die "SEPARATE_MOUNT: $path"
    done
    if [ -e "$PLUGIN" ] || [ -L "$PLUGIN" ]; then
        [ -d "$PLUGIN" ] && [ ! -L "$PLUGIN" ] || die UNKNOWN_PLUGIN_PATH
    fi
    [ "$(mount_row /var/local/data)" != "$(mount_row /)" ] || die DATA_NOT_SEPARATE
    [ "$(mount_row /var/local/data | awk '{print $3}')" = ext4 ] || die DATA_NOT_EXT4
    mount_row /var/local/data | awk '{n=split($4,a,",");for(i=1;i<=n;i++)if(a[i]=="rw")exit 0;exit 1}' || die DATA_NOT_RW
    [ -d /var/local/data ] && [ ! -L /var/local/data ] || die DATA_PATH_UNEXPECTED
    journal_valid
}
journal_valid() {
    if [ -e "$DATA" ] || [ -L "$DATA" ]; then
        [ -d "$DATA" ] && [ ! -L "$DATA" ] || die UNKNOWN_JOURNAL
        regular "$DATA/owner" && [ "$(cat "$DATA/owner")" = "$ID" ] || die UNKNOWN_JOURNAL_OWNER
        for file in phase root-signature root-flags; do
            [ ! -L "$DATA/$file" ] || die UNKNOWN_JOURNAL_FILE
        done
    fi
}
space() {
    for path in / /var/local/data /tmp; do
        free=$(df -Pk "$path" | awk 'NR==2 {print $4}')
        case "$free" in ''|*[!0-9]*) die CANNOT_CHECK_FREE_SPACE;; esac
        [ "$free" -ge 16384 ] || die "LESS_THAN_16MIB_FREE: $path"
    done
}
owned() {
    [ -d "$PLUGIN" ] && [ ! -L "$PLUGIN" ] && regular "$PLUGIN/.vehicle-telemetry-owner" &&
        [ "$(cat "$PLUGIN/.vehicle-telemetry-owner")" = "$ID" ]
}
manifest_hash() { awk '$2=="VehicleTelemetry.manifest" {print $1}' "$KIT/payload.sha256"; }
manifest_valid() { regular "$1" && [ "$(sha "$1")" = "$(manifest_hash)" ]; }
resources_valid() {
    owned || return 1
    while read -r expected relative; do
        [ "$relative" != VehicleTelemetry.manifest ] || continue
        regular "$PLUGIN/$relative" && [ "$(sha "$PLUGIN/$relative")" = "$expected" ] || return 1
    done <"$KIT/payload.sha256"
    for path in "$PLUGIN"/* "$PLUGIN"/.[!.]* "$PLUGIN"/..?*; do
        [ -e "$path" ] || [ -L "$path" ] || continue
        case "${path##*/}" in libVehicleTelemetry.so|VehicleTelemetry.xml|VehicleTelemetry_Texts.zip|VehicleTelemetry_Images.zip|VehicleTelemetry.manifest|VehicleTelemetry.registration-disabled|.vehicle-telemetry-owner) ;;
            *) return 1;; esac
        [ ! -L "$path" ] || return 1
    done
}
plugin_state() {
    if [ ! -e "$PLUGIN" ] && [ ! -L "$PLUGIN" ]; then echo absent
    elif ! owned; then echo unknown
    elif resources_valid && manifest_valid "$PLUGIN/VehicleTelemetry.manifest"; then echo installed
    elif resources_valid && manifest_valid "$PLUGIN/$DISABLED" && [ ! -e "$PLUGIN/VehicleTelemetry.manifest" ]; then echo disabled
    else echo partial-or-changed; fi
}
bounded() {
    seconds=$1; shift
    help=$(busybox timeout --help 2>&1 || :)
    case "$help" in *'-t SECS'*) busybox timeout -t "$seconds" "$@";; *) busybox timeout "$seconds" "$@";; esac
}
boot_id() { cat /proc/sys/kernel/random/boot_id; }
process_start() { [ -r "/proc/$1/stat" ] && awk '{print $22}' "/proc/$1/stat"; }
remount_root() { mount -o "remount,$1" /; }
loader() {
    bounded 20 env LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes \
        LD_LIBRARY_PATH=/usr/local/lib:/opt/hmi/lib:/usr/lib:/lib \
        /lib/ld-linux-armhf.so.3 "$KIT/payload/libVehicleTelemetry.so" >"$KIT/loader.log" 2>&1 || die LOADER_FAILED
    cat "$KIT/loader.log"
    if grep -Ei 'not found|undefined symbol|not defined in file|cannot open|relocation error|bad ELF|wrong ELF|symbol lookup error' "$KIT/loader.log"; then die UNRESOLVED_DEPENDENCIES; fi
    grep -q libAppFramework.A.so "$KIT/loader.log" && grep -q libCoreFramework.A.so "$KIT/loader.log" || die INCOMPLETE_LOADER_REPORT
    echo LOADER_TRACE_PASS_NOT_PLUGIN_EXECUTION
}
lock_active() {
    [ -d "$LOCK" ] && [ ! -L "$LOCK" ] || return 1
    for file in owner pid boot start; do regular "$LOCK/$file" || return 1; done
    [ "$(cat "$LOCK/owner")" = "$ID" ] || return 1
    pid=$(cat "$LOCK/pid"); case "$pid" in ''|*[!0-9]*) return 1;; esac
    [ "$(cat "$LOCK/boot")" = "$(boot_id)" ] || return 1
    start=$(process_start "$pid") || return 1
    [ -n "$start" ] && [ "$(cat "$LOCK/start")" = "$start" ]
}
status() {
    printf 'FILES=%s\n' "$(plugin_state)"
    if root_ro; then echo ROOT_MODE=ro; else echo ROOT_MODE=rw; fi
    if [ -e "$LOCK" ] || [ -L "$LOCK" ]; then
        if lock_active; then echo LOCK=active; else echo LOCK=stale-or-incomplete; fi
    else echo LOCK=absent; fi
    if regular "$DATA/phase"; then printf 'PHASE=%s\n' "$(cat "$DATA/phase")"; else echo PHASE=none; fi
    if regular "$DATA/trial-consumed"; then echo TRIAL=consumed-next-HMI-start-disabled
    elif regular "$DATA/trial-arm" && [ "$(cat "$DATA/trial-arm")" = "$TRIAL_TOKEN" ]; then echo TRIAL=armed-once
    else echo TRIAL=not-armed; fi
    if [ -e "$DATA/persistent.disabled" ] || [ -L "$DATA/persistent.disabled" ]; then echo ACTIVATION=blocked-after-failure
    elif regular "$DATA/persistent.enabled" && [ "$(cat "$DATA/persistent.enabled")" = "$PERSISTENT_TOKEN" ]; then echo ACTIVATION=persistent
    else echo ACTIVATION=not-enabled; fi
    if regular "$DATA/persistent.state"; then printf 'START_GUARD_STATE='; dd if="$DATA/persistent.state" bs=256 count=1 2>/dev/null; echo; fi
    pids=$(pidof HmiMain3D 2>/dev/null || :)
    for pid in $pids; do
        case "$pid" in *[!0-9]*|'') continue;; esac
        if [ -r "/proc/$pid/maps" ]; then grep -F /VehicleTelemetry/ "/proc/$pid/maps" || :; fi
    done
    echo MAPPED_DSO_IS_NOT_PROOF_OF_INIT_OR_WORKING_MENU
    if regular "$DATA/trial.log"; then tail -n 20 "$DATA/trial.log"; fi
}
phase() { printf '%s\n' "$1" >"$DATA/.phase-new"; mv "$DATA/.phase-new" "$DATA/phase"; }
restore_ro() {
    [ "$(signature)" = "$original_signature" ] || { echo ROOT_IDENTITY_CHANGED >&2; return 1; }
    if ! remount_root "$original_flags"; then remount_root ro || return 1; fi
    root_ro || return 1
    [ "$(root_flags | tr ',' '\n' | sort)" = "$(printf '%s' "$original_flags" | tr ',' '\n' | sort)" ]
}
