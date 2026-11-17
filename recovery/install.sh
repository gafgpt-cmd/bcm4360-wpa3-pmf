#!/usr/bin/env bash
set -euo pipefail
bundle=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$bundle"
acceptance=120

usage() {
  printf 'Usage: bash install.sh [--check] [--connection PROFILE]\n' >&2
  exit 2
}

mode=install
profile=
while (( $# )); do
  case $1 in
    --check)
      [[ $mode == install ]] || usage
      mode=check
      shift
      ;;
    --connection)
      [[ -z $profile && $# -ge 2 && -n $2 ]] || usage
      profile=$2
      shift 2
      ;;
    *) usage ;;
  esac
done

for tool in sha256sum modinfo modprobe rmmod insmod depmod systemd-run systemctl \
  systemd-path flock install findmnt nmcli timeout mktemp mv dirname basename readlink cmp; do
  command -v "$tool" >/dev/null || { printf 'Missing: %s\n' "$tool" >&2; exit 1; }
done

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

approved() {
  local want=$1 line
  while IFS= read -r line; do
    [[ $line =~ ^[0-9a-fA-F]{64}[[:space:]]+\*?(.*)$ ]] || continue
    [[ ${BASH_REMATCH[1]} == "$want" ]] && return 0
  done < READY_FOR_INSTALL
  return 1
}

sha256sum --check --status SHA256SUMS || die 'Bundle integrity check failed.'
[[ -f READY_FOR_INSTALL ]] || die 'Install disabled: READY_FOR_INSTALL approval is missing.'
sha256sum --check --status READY_FOR_INSTALL || die 'Install approval does not match this bundle.'
approved install.sh || die 'Install approval does not cover install.sh.'
approved candidate/wl.ko || die 'Install approval does not cover candidate/wl.ko.'

[[ -s backup/KERNEL ]] || die 'Saved kernel metadata is missing.'
[[ -s backup/SRCVERSION ]] || die 'Saved module identity is missing.'
read -r saved_kernel < backup/KERNEL
read -r saved_version < backup/SRCVERSION
[[ $(uname -r) == "$saved_kernel" ]] || die 'Kernel changed. Rebuild before install.'
candidate_vermagic=$(modinfo -F vermagic candidate/wl.ko) || die 'Cannot read candidate vermagic.'
[[ $candidate_vermagic == "$saved_kernel "* ]] || die 'Candidate module does not match this kernel.'
candidate_srcversion=$(modinfo -F srcversion candidate/wl.ko) || die 'Cannot read candidate identity.'
[[ -n $candidate_srcversion ]] || die 'Candidate module identity is empty.'
backup_vermagic=$(modinfo -F vermagic backup/wl.ko) || die 'Cannot read saved original vermagic.'
[[ $backup_vermagic == "$saved_kernel "* ]] || die 'Saved original module does not match this kernel.'
candidate_hash=$(sha256sum candidate/wl.ko) || die 'Cannot hash candidate module.'
candidate_hash=${candidate_hash%% *}
stock_hash=$(sha256sum backup/wl.ko) || die 'Cannot hash saved original module.'
stock_hash=${stock_hash%% *}

sysroot=$(findmnt --first-only --noheadings --output TARGET --types sysfs) || die 'Cannot locate sysfs.'
[[ -n $sysroot ]] || die 'Cannot locate sysfs.'
loaded=$(cat "$sysroot/module/wl/srcversion" 2>/dev/null || true)

installed=$(modinfo -n wl 2>/dev/null) || die 'Installed wl module not found.'
[[ -n $installed ]] || die 'Installed wl module not found.'
case $installed in
  /*) ;;
  *) die 'Installed module path is not absolute.' ;;
esac
case $installed in
  *"/../"*|*/..) die 'Installed module path is unsafe.' ;;
esac
[[ -f $installed ]] || die 'Installed wl module not found.'
case $installed in
  *.ko) format=ko ;;
  *.ko.zst) format=zst ;;
  *) die 'Unsupported installed module format. Only .ko and .ko.zst are supported.' ;;
esac
if [[ $format == zst ]]; then
  command -v zstd >/dev/null || die 'Missing: zstd'
fi
installed_vermagic=$(modinfo -F vermagic "$installed") || die 'Cannot read installed module vermagic.'
[[ $installed_vermagic == "$saved_kernel "* ]] || die 'Installed module does not belong to the saved kernel.'
case $format in
  ko)
    installed_hash=$(sha256sum "$installed") || die 'Cannot hash installed module.'
    ;;
  zst)
    installed_hash=$(zstd --quiet --decompress --stdout "$installed" | sha256sum) \
      || die 'Cannot decompress installed module.'
    ;;
esac
installed_hash=${installed_hash%% *}

installed_is_candidate=false
[[ $installed_hash == "$candidate_hash" ]] && installed_is_candidate=true
loaded_is_candidate=false
[[ $loaded == "$candidate_srcversion" ]] && loaded_is_candidate=true
loaded_is_stock=false
[[ $loaded == "$saved_version" ]] && loaded_is_stock=true

state=$bundle/backup/installed
state_present=false

validate_state() {
  local state_hash
  [[ -f $state/SHA256SUMS ]] || return 1
  [[ -f $state/module && -f $state/INSTALL_PATH && -f $state/FORMAT ]] || return 1
  [[ -f $state/KERNEL && -f $state/SRCVERSION ]] || return 1
  (cd "$state" && sha256sum --check --status SHA256SUMS) || return 1
  read -r state_path < "$state/INSTALL_PATH"
  read -r state_format < "$state/FORMAT"
  read -r state_kernel < "$state/KERNEL"
  read -r state_version < "$state/SRCVERSION"
  [[ $state_kernel == "$saved_kernel" && $state_version == "$saved_version" ]] || return 1
  [[ $state_path == "$installed" && $state_format == "$format" ]] || return 1
  case $state_format:$state_path in
    ko:*.ko|zst:*.ko.zst) ;;
    *) return 1 ;;
  esac
  case $state_format in
    ko) state_hash=$(sha256sum "$state/module") || return 1 ;;
    zst) state_hash=$(zstd --quiet --decompress --stdout "$state/module" | sha256sum) \
      || return 1 ;;
  esac
  state_hash=${state_hash%% *}
  [[ $state_hash == "$stock_hash" ]] || return 1
}

if [[ -e $state ]]; then
  state_present=true
  validate_state || die 'Existing installed-module backup is incomplete or corrupt. Refusing to overwrite it.'
fi

if [[ $installed_hash != "$stock_hash" && $installed_is_candidate == false ]]; then
  die 'Installed module identity is unexpected. Refusing to replace it.'
fi
if [[ $state_present == false && $installed_is_candidate == true ]]; then
  die 'Candidate is installed but the original module backup is missing. Refusing.'
fi
if [[ $loaded_is_stock == false && $loaded_is_candidate == false ]]; then
  die 'Original driver is not loaded. Run revert.sh first.'
fi

if [[ -r $sysroot/kernel/security/lockdown ]] && ! grep -q '\[none\]' "$sysroot/kernel/security/lockdown"; then
  die 'Kernel lockdown active. Unsigned install refused.'
fi
if [[ -r $sysroot/module/module/parameters/sig_enforce ]] \
  && grep -qi '^y' "$sysroot/module/module/parameters/sig_enforce"; then
  die 'Module signatures enforced. Unsigned install refused.'
fi

if [[ $state_present == true && $installed_is_candidate == true && $loaded_is_candidate == true ]]; then
  printf 'Candidate driver is already installed and loaded. No changes made.\n'
  printf 'Confirm actual Wi-Fi internet from this laptop before unplugging the phone.\n'
  exit 0
fi

resolve_connection() {
  local path driver entry
  local -a wl_interfaces=() active_list=() selected=()
  for path in "$sysroot"/class/net/*; do
    [[ -e $path/device/driver ]] || continue
    driver=$(basename "$(readlink -f "$path/device/driver")")
    [[ $driver == wl ]] && wl_interfaces+=("$(basename "$path")")
  done
  if (( ${#wl_interfaces[@]} != 1 )); then
    die 'Refusing: cannot identify exactly one wl Wi-Fi interface. Check the driver state, then retry.'
  fi
  iface=${wl_interfaces[0]}
  if [[ -n $profile ]]; then
    profile_type=$(nmcli -g connection.type connection show "$profile") \
      || die 'Cannot read the selected connection.'
    [[ $profile_type == 802-11-wireless ]] || die 'The selected connection is not a saved Wi-Fi profile.'
    profile_uuid=$(nmcli -g connection.uuid connection show "$profile") || die 'Cannot read the selected profile.'
    [[ -n $profile_uuid ]] || die 'The selected profile has no UUID.'
  else
    mapfile -t active_list < <(nmcli -g UUID,DEVICE connection show --active)
    for entry in "${active_list[@]}"; do
      [[ $entry == *":$iface" ]] && selected+=("${entry%%:*}")
    done
    if (( ${#selected[@]} != 1 )); then
      printf 'Refusing: cannot determine the selected Wi-Fi profile for %s.\n' "$iface" >&2
      printf 'Run: sudo bash %q --connection <profile>\n' "$bundle/install.sh" >&2
      exit 1
    fi
    profile_uuid=${selected[0]}
    profile_type=$(nmcli -g connection.type connection show "$profile_uuid") \
      || die 'Cannot read the selected connection.'
    [[ $profile_type == 802-11-wireless ]] || die 'The selected connection is not a saved Wi-Fi profile.'
  fi
}

resolve_connection

if [[ $mode == check ]]; then
  printf 'Bundle, approval, kernel, module format and recovery state verified.\n'
  printf 'Target interface and saved Wi-Fi profile resolved. No changes made.\n'
  exit 0
fi

if [[ $(id -u) != 0 ]]; then
  if [[ -n $profile ]]; then
    printf 'Run: sudo bash %q --connection %q\n' "$bundle/install.sh" "$profile" >&2
  else
    printf 'Run: sudo bash %q\n' "$bundle/install.sh" >&2
  fi
  exit 1
fi

runtime_root=$(systemd-path system-runtime) || die 'Cannot locate the system runtime directory.'
[[ -d $runtime_root ]] || die 'Cannot locate the system runtime directory.'
exec {trial_lock_fd}>"$runtime_root/wl-wpa3-trial.lock"
flock -n "$trial_lock_fd" || die 'A trial is already starting.'
exec {install_lock_fd}>"$runtime_root/wl-wpa3-install.lock"
flock -n "$install_lock_fd" || die 'Another installation is in progress.'
for unit in wl-wpa3-restore.timer wl-wpa3-restore.service \
  wl-wpa3-install-restore.timer wl-wpa3-install-restore.service; do
  if systemctl is-active --quiet "$unit"; then
    die 'Recovery is still armed. Let it finish before another installation.'
  fi
done

if [[ $state_present == true && $installed_is_candidate == true && $loaded_is_candidate == true ]]; then
  printf 'Candidate driver is already installed and loaded. No changes made.\n'
  printf 'Confirm actual Wi-Fi internet from this laptop before unplugging the phone.\n'
  exit 0
fi

umask 077
if [[ $state_present == false ]]; then
  tmp_state=$(mktemp -d "$bundle/backup/.installed.XXXXXX")
  install -m 600 "$installed" "$tmp_state/module"
  printf '%s\n' "$installed" > "$tmp_state/INSTALL_PATH"
  printf '%s\n' "$format" > "$tmp_state/FORMAT"
  printf '%s\n' "$saved_kernel" > "$tmp_state/KERNEL"
  printf '%s\n' "$saved_version" > "$tmp_state/SRCVERSION"
  (cd "$tmp_state" && sha256sum module INSTALL_PATH FORMAT KERNEL SRCVERSION > SHA256SUMS)
  chmod 600 "$tmp_state"/*
  mv "$tmp_state" "$state"
  validate_state || die 'Saved original module backup failed validation.'
fi

stage=$(mktemp -d "$runtime_root/wl-wpa3-install.XXXXXX")
install -d -m 700 "$stage/backup/installed" "$stage/candidate"
install -m 600 revert.sh "$stage/"
install -m 600 backup/wl.ko backup/KERNEL backup/SRCVERSION backup/SHA256SUMS "$stage/backup/"
install -m 600 candidate/wl.ko "$stage/candidate/"
install -m 600 "$state/module" "$state/INSTALL_PATH" "$state/FORMAT" \
  "$state/KERNEL" "$state/SRCVERSION" "$state/SHA256SUMS" "$stage/backup/installed/"
(cd "$stage/backup" && sha256sum --check --status SHA256SUMS) \
  || die 'Root recovery copy failed its checksum.'
(cd "$stage/backup/installed" && sha256sum --check --status SHA256SUMS) \
  || die 'Root installed-module backup failed its checksum.'

bash_bin=$(command -v bash)
systemd-run --unit=wl-wpa3-install-restore --collect --on-active="${acceptance}s" \
  --timer-property=AccuracySec=1s --timer-property=RemainAfterElapse=no \
  --property=Type=oneshot --property=TimeoutStartSec=45s "$bash_bin" "$stage/revert.sh" \
  || die 'Could not arm the automatic rollback timer. No changes made.'
if ! systemctl is-active --quiet wl-wpa3-install-restore.timer; then
  die 'Automatic rollback timer is not active. No changes made.'
fi

rolled_back=0
tmp_file=
rollback() {
  (( rolled_back == 0 )) || return 0
  rolled_back=1
  trap - ERR HUP INT TERM
  if [[ -n $tmp_file && -e $tmp_file ]]; then
    rm -f -- "$tmp_file"
  fi
  if bash "$stage/revert.sh"; then
    systemctl stop wl-wpa3-install-restore.timer wl-wpa3-install-restore.service \
      2>/dev/null || true
    printf 'Original installed module restored.\n'
  else
    printf 'Automatic restore failed; disk restoration is not confirmed.\n' >&2
    printf 'Retry manual restore: sudo bash %q\n' "$stage/revert.sh" >&2
    return 1
  fi
}
on_error() {
  rc=$?
  printf 'Installation failed. Restoring the original installed module.\n' >&2
  rollback || true
  exit $(( rc != 0 ? rc : 1 ))
}
abort() {
  printf '%s\n' "$*" >&2
  printf 'Installation failed. Restoring the original installed module.\n' >&2
  rollback || true
  exit 1
}
trap on_error ERR HUP INT TERM

target_dir=$(dirname "$installed")
tmp_file=$(mktemp "$target_dir/.wl-wpa3-install.XXXXXX")
case $format in
  ko) install -m 644 candidate/wl.ko "$tmp_file" ;;
  zst)
    zstd --quiet -f candidate/wl.ko -o "$tmp_file"
    chmod 644 "$tmp_file"
    ;;
esac
mv -f "$tmp_file" "$installed"
tmp_file=
depmod "$saved_kernel" || abort 'depmod failed after replacing the installed module.'

rmmod wl || abort 'wl is busy and cannot unload safely.'
depends=$(modinfo -F depends "$stage/candidate/wl.ko") || abort 'Cannot read candidate dependencies.'
IFS=, read -r -a dependencies <<< "$depends"
for dependency in "${dependencies[@]}"; do
  [[ -n $dependency ]] || continue
  modprobe "$dependency" || abort "Cannot load candidate dependency: $dependency"
done
insmod "$stage/candidate/wl.ko" || abort 'Candidate module failed to load.'

loaded=$(cat "$sysroot/module/wl/srcversion" 2>/dev/null || true)
[[ $loaded == "$candidate_srcversion" ]] || abort 'Loaded module identity does not match the candidate.'

if ! timeout 45 nmcli connection up "$profile_uuid" ifname "$iface"; then
  abort 'Saved Wi-Fi profile failed to reconnect.'
fi

connected=false
mapfile -t active_list < <(nmcli -g UUID,DEVICE connection show --active)
for entry in "${active_list[@]}"; do
  if [[ $entry == "$profile_uuid:$iface" ]]; then
    connected=true
  fi
done
[[ $connected == true ]] || abort 'Target interface is not connected to the selected profile.'

systemctl stop wl-wpa3-install-restore.timer wl-wpa3-install-restore.service \
  2>/dev/null || true
if systemctl is-active --quiet wl-wpa3-install-restore.timer; then
  abort 'Could not cancel the automatic rollback timer.'
fi
loaded=$(cat "$sysroot/module/wl/srcversion" 2>/dev/null || true)
[[ $loaded == "$candidate_srcversion" ]] || abort 'Installed module identity changed before acceptance.'
connected=false
mapfile -t active_list < <(nmcli -g UUID,DEVICE connection show --active)
for entry in "${active_list[@]}"; do
  if [[ $entry == "$profile_uuid:$iface" ]]; then
    connected=true
  fi
done
[[ $connected == true ]] || abort 'Wi-Fi connection changed before acceptance.'
case $format in
  ko)
    accepted_hash=$(sha256sum "$installed") || abort 'Cannot hash installed module before acceptance.'
    ;;
  zst)
    accepted_hash=$(zstd --quiet --decompress --stdout "$installed" | sha256sum) \
      || abort 'Cannot read installed module before acceptance.'
    ;;
esac
accepted_hash=${accepted_hash%% *}
[[ $accepted_hash == "$candidate_hash" ]] || abort 'Installed module file changed before acceptance.'
trap - ERR HUP INT TERM

printf 'Installed candidate driver for kernel %s.\n' "$saved_kernel"
printf 'Original installed module saved under backup/installed.\n'
printf 'Manual restore: sudo bash %q\n' "$bundle/revert.sh"
printf 'Confirm actual Wi-Fi internet from this laptop before unplugging the phone.\n'
printf 'USB tethering does not prove Wi-Fi connectivity.\n'
printf 'Kernel or package updates may replace this module with the stock driver.\n'
printf 'Rebuild for the exact kernel and reinstall; DKMS is not part of this install.\n'
