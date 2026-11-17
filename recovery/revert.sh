#!/usr/bin/env bash
set -euo pipefail
bundle=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ $(id -u) != 0 ]]; then
  printf 'Run: sudo bash %q\n' "$bundle/revert.sh" >&2
  exit 1
fi
cd "$bundle/backup"
sha256sum --check --status SHA256SUMS
read -r saved_kernel < KERNEL
read -r saved_version < SRCVERSION

restore_installed_file() {
  local state=$bundle/backup/installed install_path install_format
  local state_kernel state_version target_dir tmp
  [[ -e $state ]] || return 0
  [[ -f $state/SHA256SUMS ]] || {
    printf 'Installed-module backup is incomplete.\n' >&2
    return 1
  }
  [[ -f $state/module && -f $state/INSTALL_PATH && -f $state/FORMAT ]] || {
    printf 'Installed-module backup is incomplete.\n' >&2
    return 1
  }
  [[ -f $state/KERNEL && -f $state/SRCVERSION ]] || {
    printf 'Installed-module backup is incomplete.\n' >&2
    return 1
  }
  (cd "$state" && sha256sum --check --status SHA256SUMS) || {
    printf 'Installed-module backup failed its checksum.\n' >&2
    return 1
  }
  read -r install_path < "$state/INSTALL_PATH"
  read -r install_format < "$state/FORMAT"
  read -r state_kernel < "$state/KERNEL"
  read -r state_version < "$state/SRCVERSION"
  [[ $state_kernel == "$saved_kernel" && $state_version == "$saved_version" ]] || {
    printf 'Installed-module backup metadata is inconsistent.\n' >&2
    return 1
  }
  case $install_path in
    /*) ;;
    *)
      printf 'Saved installed-module path is not absolute.\n' >&2
      return 1
      ;;
  esac
  case $install_path in
    *"/../"*|*/..)
      printf 'Saved installed-module path is unsafe.\n' >&2
      return 1
      ;;
  esac
  case $install_format:$install_path in
    ko:*.ko|zst:*.ko.zst) ;;
    *)
      printf 'Saved installed-module path and format disagree.\n' >&2
      return 1
      ;;
  esac
  if [[ $(uname -r) == "$saved_kernel" ]]; then
    current=$(modinfo -n wl 2>/dev/null || true)
    if [[ $current != "$install_path" ]]; then
      printf 'Installed module path changed. Refusing to overwrite it.\n' >&2
      return 1
    fi
  fi
  target_dir=$(dirname "$install_path")
  if [[ ! -d $target_dir ]]; then
    printf 'Saved kernel module directory is gone; disk restore skipped.\n' >&2
    return 0
  fi
  tmp=$(mktemp "$target_dir/.wl-wpa3-restore.XXXXXX")
  install -m 644 "$state/module" "$tmp"
  mv -f "$tmp" "$install_path"
  depmod "$saved_kernel"
  printf 'Installed module file restored.\n'
}

restore_installed_file || {
  printf 'Rollback could not restore the installed module file.\n' >&2
  exit 1
}

if [[ $(uname -r) != "$saved_kernel" ]]; then
  modprobe wl
  printf 'Kernel changed. Loaded its installed stock driver. Saved module was not loaded.\n'
  exit 0
fi

sysroot=$(findmnt --first-only --noheadings --output TARGET --types sysfs)
loaded=$(cat "$sysroot/module/wl/srcversion" 2>/dev/null || true)
if [[ $loaded == "$saved_version" ]]; then
  printf 'Original driver already loaded.\n'
  exit 0
fi
if [[ -n $loaded ]]; then
  if ! rmmod wl; then
    printf 'Driver is busy and cannot unload. Reboot to restore the installed stock driver.\n' >&2
    exit 1
  fi
fi
depends=$(modinfo -F depends "$bundle/backup/wl.ko")
IFS=, read -r -a dependencies <<< "$depends"
for dependency in "${dependencies[@]}"; do
  modprobe "$dependency"
done
insmod "$bundle/backup/wl.ko"
loaded=$(cat "$sysroot/module/wl/srcversion" 2>/dev/null || true)
if [[ $loaded != "$saved_version" ]]; then
  printf 'Stock driver identity differs. Reboot to restore the installed driver.\n' >&2
  exit 1
fi
printf 'Original driver restored. NetworkManager can reconnect.\n'
