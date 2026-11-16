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
if [[ $(uname -r) != "$saved_kernel" ]]; then
  modprobe wl
  printf 'Kernel changed. Loaded its installed stock driver.\n'
  exit 0
fi
sysroot=$(findmnt --first-only --noheadings --output TARGET --types sysfs)
loaded=$(cat "$sysroot/module/wl/srcversion" 2>/dev/null || true)
if [[ $loaded == "$saved_version" ]]; then
  printf 'Original driver already loaded.\n'
  exit 0
fi
if [[ -n $loaded ]]; then
  if ! modprobe -r wl; then
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
