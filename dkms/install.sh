#!/bin/bash
# Install the patched wl driver as a DKMS package so every kernel update rebuilds it.
#   sudo dkms/install.sh [--blob FILE]         install for every kernel that has headers
#   sudo dkms/install.sh --remove              uninstall and hand wl back to the stock DKMS package
#   dkms/install.sh --stage DIR [--blob FILE]  only assemble the DKMS source tree in DIR (no root)
# Never loads or unloads a module: the result takes effect at the next boot.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# name/version come from dkms.conf so there is a single source of truth
NAME=$(sed -n 's/^PACKAGE_NAME="\(.*\)"/\1/p' "$REPO/dkms/dkms.conf")
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$REPO/dkms/dkms.conf")
BLOB_SHA256=352a6e349f74c69b78e76f68c63752c99b8f6b22dc942af531b754211d7f4743
MARK="# wl-wpa3 dkms/install.sh: $NAME owns the wl module"
# tests point SYSROOT at a scratch tree; empty on a real system
SYSROOT=${WL_WPA3_SYSROOT:-}
USRSRC=$SYSROOT/usr/src
DKMSETC=$SYSROOT/etc/dkms
MODPROBE=$SYSROOT/etc/modprobe.d/wl-wpa3.conf
MODULES=$SYSROOT/lib/modules
DKMSTREE=$SYSROOT/var/lib/dkms
LOGDIR=$SYSROOT/var/tmp

die() { echo "ERROR: $*" >&2; exit 1; }

blob="$REPO/wl-src/lib/wlc_hybrid.o_amd64.orig"
mode=install
stage=
while [ $# -gt 0 ]; do
  case $1 in
    --blob) [ $# -ge 2 ] || die "--blob needs a file"; blob=$2; shift 2 ;;
    --stage) [ $# -ge 2 ] || die "--stage needs a directory"; mode=stage; stage=$2; shift 2 ;;
    --remove) mode=remove; shift ;;
    *) die "unknown argument: $1 (see the header of $0)" ;;
  esac
done

# kernels whose headers are installed: the ones DKMS can build for
kernels() {
  local d
  for d in "$MODULES"/*/build/include; do
    if [ -d "$d" ]; then basename "$(dirname "$(dirname "$d")")"; fi
  done
}

# other DKMS source trees that also build a module named wl (stock broadcom-wl / broadcom-sta)
other_wl_packages() {
  local conf
  for conf in "$USRSRC"/*/dkms.conf; do
    [ -f "$conf" ] || continue
    # shellcheck disable=SC1090
    (
      set +u  # dkms.conf files reference ${kernelver} and other unset DKMS variables
      . "$conf"
      [ "${PACKAGE_NAME:-}" != "$NAME" ] && [ "${BUILT_MODULE_NAME[0]:-}" = wl ] &&
        echo "$PACKAGE_NAME $PACKAGE_VERSION"
    ) || true
  done
}

# remove every registered version of a DKMS package (no-op when absent)
dkms_purge() {
  local v
  for v in $(dkms status -m "$1" 2>/dev/null | sed -n "s|^$1/\([^,:]*\)[,:].*|\1|p" | sort -u); do
    dkms remove "$1/$v" --all
  done
}

remove_sources() {
  local d
  for d in "$USRSRC/$NAME"-*; do
    if [ -d "$d" ]; then rm -rf "$d"; fi
  done
}

assemble() {
  local dst=$1
  mkdir -p "$dst/lib" "$dst/patches"
  # never ship objects from a previous ./build.sh (other kernel/flags) into the DKMS tree
  tar -C "$REPO/wl-src" --exclude='*.o' --exclude='.*.cmd' --exclude='*.ko' --exclude='*.mod' \
    --exclude='*.mod.c' --exclude=Module.symvers --exclude=modules.order \
    -cf - Makefile src | tar -C "$dst" -xf -
  cp "$blob" "$dst/lib/wlc_hybrid.o_amd64.orig"
  cp "$REPO/patches/wl_patch.py" "$REPO/patches/wl_reloc.py" "$dst/patches/"
  cp "$REPO/dkms/dkms.conf" "$REPO/dkms/dkms-prebuild.sh" "$dst/"
  chmod +x "$dst/dkms-prebuild.sh"
}

if [ "$mode" != remove ]; then
  [ -f "$blob" ] || die "pristine blob not found: $blob (see README.md#build or pass --blob)"
  echo "$BLOB_SHA256  $blob" | sha256sum -c --quiet - || die "$blob is not the pristine 6.30.223.271 blob"
fi

if [ "$mode" = stage ]; then
  [ ! -e "$stage" ] || die "$stage already exists"
  assemble "$stage"
  echo "staged $NAME/$VER source tree at $stage"
  exit 0
fi

[ "$(id -u)" = 0 ] || die "run with sudo"
command -v dkms >/dev/null || die "dkms is not installed"
mapfile -t ks < <(kernels)
[ ${#ks[@]} -gt 0 ] || die "no kernel headers under $MODULES/*/build"

if [ "$mode" = remove ]; then
  echo "== remove $NAME =="
  dkms_purge "$NAME"
  remove_sources
  rm -f "$MODPROBE"
  echo "== let the stock wl package build again =="
  for f in "$DKMSETC"/*.conf; do
    if [ -f "$f" ] && [ "$(head -n 1 "$f")" = "$MARK" ]; then rm -f "$f"; echo "   removed $f"; fi
  done
  mapfile -t others < <(other_wl_packages)
  [ ${#others[@]} -gt 0 ] || echo "   NOTE: no stock wl DKMS package in $USRSRC; install broadcom-wl-dkms for Wi-Fi"
  for o in "${others[@]}"; do
    for k in "${ks[@]}"; do dkms install "${o% *}/${o#* }" -k "$k" --force; done
  done
  dkms status
  echo "Done. The stock wl is used from the next boot."
  exit 0
fi

mapfile -t others < <(other_wl_packages)
for o in "${others[@]}"; do
  override=$DKMSETC/${o% *}.conf
  if [ -e "$override" ] && [ "$(head -n 1 "$override")" != "$MARK" ]; then
    die "$override exists and was not written by this script; merge it by hand"
  fi
done

echo "== replace any previous $NAME source tree =="
dkms_purge "$NAME"
remove_sources
assemble "$USRSRC/$NAME-$VER"
dkms add "$NAME/$VER"

echo "== build for every kernel with headers (stock driver untouched so far) =="
for k in "${ks[@]}"; do
  if ! dkms build "$NAME/$VER" -k "$k"; then
    log=$LOGDIR/$NAME-$VER-$k-make.log
    cp "$DKMSTREE/$NAME/$VER/build/make.log" "$log" 2>/dev/null || log="(no make.log)"
    dkms_purge "$NAME"
    remove_sources
    die "build failed for $k; stock driver not touched by this run. Log: $log"
  fi
done

echo "== stop other DKMS packages from building wl =="
# Arch's pacman hook builds every /usr/src/*/dkms.conf for each new kernel. Two packages that both
# install updates/dkms/wl.ko would race, so the stock one is excluded from every kernel instead.
# Its package stays installed: upgrades of it keep this override, and --remove undoes it.
mkdir -p "$DKMSETC"
for o in "${others[@]}"; do
  dkms_purge "${o% *}"
  printf '%s\n%s\n%s\n' "$MARK" "# Delete this file (or run dkms/install.sh --remove) to build it again." \
    'BUILD_EXCLUSIVE_KERNEL="^$"' > "$DKMSETC/${o% *}.conf"
  echo "   wrote $DKMSETC/${o% *}.conf"
done

echo "== keep conflicting in-tree drivers off the card =="
mkdir -p "$(dirname "$MODPROBE")"
cat > "$MODPROBE" <<'BL'
blacklist b43
blacklist b43legacy
blacklist bcm43xx
blacklist bcma
blacklist brcm80211
blacklist brcmfmac
blacklist brcmsmac
blacklist ssb
BL

echo "== install for every kernel =="
for k in "${ks[@]}"; do
  # --force: replace a wl.ko installed by hand (recovery/install.sh); DKMS archives it for rollback
  dkms install "$NAME/$VER" -k "$k" --force
done

echo "== result =="
dkms status
for k in "${ks[@]}"; do
  printf '%s: %s srcversion=%s\n' "$k" "$(modinfo -k "$k" -F filename wl)" "$(modinfo -k "$k" -F srcversion wl)"
done
echo "Not loaded now. The patched wl is used from the next boot."
