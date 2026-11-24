#!/bin/sh
# DKMS PRE_BUILD: build the hooked blob in the DKMS build dir (cwd = build dir).
# Mirrors ../build.sh exactly. The patch scripts take the blob path as $1 -- pass the
# build-dir-relative path so they touch THIS build's blob (not some absolute checkout).
set -e
ORIG=lib/wlc_hybrid.o_amd64.orig
BLOB=lib/wlc_hybrid.o_amd64
# offsets in patches/ are valid for this exact blob only
SHA256=352a6e349f74c69b78e76f68c63752c99b8f6b22dc942af531b754211d7f4743
[ -f "$ORIG" ] || { echo "missing $ORIG (pristine Broadcom blob)"; exit 1; }
echo "$SHA256  $ORIG" | sha256sum -c --quiet - || { echo "$ORIG is not the pristine 6.30.223.271 blob"; exit 1; }

cp "$ORIG" "$BLOB"
python3 patches/wl_patch.py "$BLOB" --apply
objcopy \
  --globalize-symbol=wlc_sendauth --globalize-symbol=wlc_sendmgmt \
  --globalize-symbol=wlc_queue_80211_frag --globalize-symbol=wlc_frame_get_mgmt \
  --globalize-symbol=wlc_frame_get_mgmt_ex --globalize-symbol=wlc_sup_set_pmk \
  --globalize-symbol=wlc_authresp_client --globalize-symbol=wlc_recv \
  --globalize-symbol=wlc_scbfind --globalize-symbol=wlc_bsscfg_find_by_bssid \
  --add-symbol re_auth_advance=.text:0x5b79d,global,function \
  "$BLOB" "$BLOB.g"
mv "$BLOB.g" "$BLOB"
python3 patches/wl_reloc.py "$BLOB"
