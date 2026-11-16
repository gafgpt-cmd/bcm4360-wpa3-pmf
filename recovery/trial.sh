#!/usr/bin/env bash
set -euo pipefail
bundle=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$bundle"
case ${1:-} in
  ''|--check) ;;
  *) printf 'Usage: bash trial.sh [--check]\n' >&2; exit 2 ;;
esac
if (( $# > 1 )); then exit 2; fi
for tool in sha256sum modinfo modprobe insmod systemd-run systemctl systemd-path flock install findmnt; do
  command -v "$tool" >/dev/null || { printf 'Missing: %s\n' "$tool" >&2; exit 1; }
done
sha256sum --check --status SHA256SUMS
read -r saved_kernel < backup/KERNEL
read -r saved_version < backup/SRCVERSION
[[ $(uname -r) == "$saved_kernel" ]] || { printf 'Kernel changed. Rebuild before trial.\n' >&2; exit 1; }
[[ $(modinfo -F vermagic candidate/wl.ko) == "$saved_kernel "* ]] || exit 1
sysroot=$(findmnt --first-only --noheadings --output TARGET --types sysfs)
loaded=$(cat "$sysroot/module/wl/srcversion" 2>/dev/null || true)
[[ $loaded == "$saved_version" ]] || { printf 'Original driver is not loaded. Run revert.sh first.\n' >&2; exit 1; }
if [[ -r $sysroot/kernel/security/lockdown ]] && ! grep -q '\[none\]' "$sysroot/kernel/security/lockdown"; then
  printf 'Kernel lockdown active. Unsigned trial refused.\n' >&2; exit 1
fi
if [[ -r $sysroot/module/module/parameters/sig_enforce ]] && grep -qi '^y' "$sysroot/module/module/parameters/sig_enforce"; then
  printf 'Module signatures enforced. Unsigned trial refused.\n' >&2; exit 1
fi
if [[ ${1:-} == --check ]]; then
  printf 'Files, kernel and original driver verified. No changes made.\n'
  exit 0
fi
if [[ ! -f READY_FOR_TRIAL ]]; then
  printf 'Trial disabled: required independent reviews and validation are incomplete.\n' >&2
  exit 1
fi
sha256sum --check --status READY_FOR_TRIAL
if [[ $(id -u) != 0 ]]; then
  printf 'Run: sudo bash %q\n' "$bundle/trial.sh" >&2
  exit 1
fi
runtime_root=$(systemd-path system-runtime)
exec {lock_fd}>"$runtime_root/wl-wpa3-trial.lock"
flock -n "$lock_fd" || { printf 'A trial is already starting.\n' >&2; exit 1; }
if systemctl is-active --quiet wl-wpa3-restore.timer || systemctl is-active --quiet wl-wpa3-restore.service; then
  printf 'Recovery is still armed. Let it finish before another trial.\n' >&2; exit 1
fi
umask 077
stage=$(mktemp -d "$runtime_root/wl-wpa3-trial.XXXXXX")
install -d -m 700 "$stage/backup" "$stage/candidate"
install -m 600 trial.sh revert.sh SHA256SUMS READY_FOR_TRIAL "$stage/"
install -m 600 backup/wl.ko backup/KERNEL backup/SRCVERSION backup/SHA256SUMS "$stage/backup/"
install -m 600 candidate/wl.ko "$stage/candidate/"
cd "$stage"
sha256sum --check --status SHA256SUMS
bash_bin=$(command -v bash)
systemd-run --unit=wl-wpa3-restore --collect --on-active=120s \
  --timer-property=AccuracySec=1s --timer-property=RemainAfterElapse=no \
  --property=Type=oneshot --property=TimeoutStartSec=45s "$bash_bin" "$stage/revert.sh"
systemctl is-active --quiet wl-wpa3-restore.timer
printf 'Automatic stock-driver restore armed for 120 seconds.\n'
printf 'Manual restore: sudo bash %q\n' "$bundle/revert.sh"
printf 'A reboot also restores stock. Installed driver files are untouched.\n'
trap 'bash "$stage/revert.sh"; exit 1' ERR HUP INT TERM
modprobe -r wl
insmod "$stage/candidate/wl.ko"
trap - ERR HUP INT TERM
printf 'Temporary candidate loaded. Connect through the Wi-Fi menu.\n'
printf 'The trial ALWAYS reverts automatically. No keep/install action exists.\n'
