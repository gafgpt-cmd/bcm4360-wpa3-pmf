# DKMS install (auto-rebuild on kernel updates)

Persistent path: the patched `wl` is rebuilt by DKMS for every installed and every new kernel.
Arch / Omarchy layout. Never loads or unloads a module: takes effect at next boot.

```sh
sudo bash dkms/install.sh --blob /usr/lib/broadcom-wl-dkms/wlc_hybrid.o_shipped
sudo bash dkms/install.sh --remove          # back to stock broadcom-wl-dkms
bash dkms/install.sh --stage DIR --blob F   # assemble the DKMS source tree only, no root
```

- `--blob`: pristine 6.30.223.271 x86_64 blob, SHA-256 checked (default `wl-src/lib/wlc_hybrid.o_amd64.orig`).
  `broadcom-wl-dkms` ships it as `wlc_hybrid.o_shipped`.
- Use `--remove` to undo. Not `recovery/revert.sh`: that restores a hand-installed file only.

## What install does

1. Copy sources + blob + patch scripts to `/usr/src/wl-wpa3-<version>`, `dkms add`.
2. `dkms build` for every kernel with headers. Any failure: own package removed, stock untouched, `make.log` copied to `/var/tmp`.
3. Stock `broadcom-wl` DKMS: removed from all kernels, then excluded via `/etc/dkms/broadcom-wl.conf` (`BUILD_EXCLUSIVE_KERNEL="^$"`).
4. `/etc/modprobe.d/wl-wpa3.conf`: blacklist in-tree Broadcom drivers.
5. `dkms install --force` for every kernel. `--force` replaces a hand-installed `wl.ko`; DKMS archives it and restores it on remove.

## Coexistence with stock broadcom-wl-dkms

| Fact | Effect |
| --- | --- |
| Both packages build `updates/dkms/wl.ko.zst` | Last one built wins; pacman hook builds both per kernel |
| `/etc/dkms/<pkg>.conf` overrides the package's `dkms.conf` | Stock gets `BUILD_EXCLUSIVE_KERNEL="^$"`: never built |
| Override is not owned by the package | Survives stock upgrades and reinstalls |

Choice: keep the stock package installed, excluded. Not `pacman -R`:
- a later reinstall (e.g. Omarchy hardware fix script) would race ours again
- rollback would need the package from cache or network

Expected noise on each kernel update: `dkms install broadcom-wl ... exited 77` (skipped). Harmless.
`--remove` deletes only overrides whose first line is this script's marker, then rebuilds stock.
A foreign `/etc/dkms/broadcom-wl.conf` makes install refuse before any change.

## The one thing that bites you

`dkms.conf` **must** run `PRE_BUILD="dkms-prebuild.sh"`: byte patch + symbol globalize + reloc redirect
on the blob in the DKMS build dir. Stock `dkms.conf` has no such step: DKMS compiles the unpatched blob,
a driver that scans but never completes WPA3-SAE (0 `__wrap_*` hooks, 0 P1 bytes in the `.ko`).

## Files

- `dkms.conf`: package definition, `PRE_BUILD`, x86_64 only.
- `dkms-prebuild.sh`: checks blob SHA-256, then mirrors `../build.sh`.
- `install.sh`: install / remove / stage.
- Tests: `python3 tests/dkms.py` (stubbed DKMS, scratch sysroot).
