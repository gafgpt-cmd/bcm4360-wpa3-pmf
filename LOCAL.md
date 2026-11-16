# Local PMF patch

Base: upstream WPA3 bridge + kernel compatibility revision.
Scope: BIP-CMAC-128, replay checks, RX framing, temporary recovery.
No persistent installation. No upstream PR until hardware tests pass.

## Check

Needs: C compiler, OpenSSL development files, pkg-config, Python 3, ShellCheck, Gitleaks.

```bash
bash tests/check.sh
shellcheck recovery/*.sh tests/check.sh
gitleaks dir --redact --no-banner .
```

Tests execute the PMF header with OpenSSL as the host crypto backend.
Kernel module uses Linux AES-CMAC. No custom AES implementation.
Host tests do not prove kernel integration or radio behavior.
Build requirements and commands: [README.md](README.md#build).

## Temporary trial bundle

Keep the prepared bundle in a home-folder directory outside Git, with:

- trial.sh, revert.sh, SHA256SUMS
- READY_FOR_TRIAL: candidate checksum, created only after required review/gate approvals
- candidate/wl.ko
- backup/wl.ko, KERNEL, SRCVERSION, SHA256SUMS

KERNEL and SRCVERSION are one-line values for the saved stock module.
Hash manifests use `sha256sum` format: backup/SHA256SUMS lists paths relative to
backup/; the bundle's SHA256SUMS lists scripts, candidate and backup files relative
to the bundle. READY_FOR_TRIAL contains the approved hash for candidate/wl.ko.

```bash
bash trial.sh --check
sudo bash trial.sh
sudo bash trial.sh --provisional
sudo bash revert.sh
```

Default trial: 120 seconds, no argument.
Provisional trial: 3600 seconds, `--provisional`.
Provisional is still temporary: no installation, no profile change, no service restart.
Reboot loads stock in both modes.
Manual rollback: `sudo bash revert.sh`, or any reboot.
Trial verifies hashes, kernel, loaded stock identity and signature policy.
Copies recovery files into a root-owned runtime directory.
Arms the timer before unloading stock: 120 seconds by default, 3600 provisional.
Trial and recovery use `rmmod wl` to preserve cfg80211 and its netlink family.
Both load module dependencies before `insmod`.
The timer invokes stock restoration; failed loading invokes it immediately.
On the saved kernel, recovery loads the saved module directly and verifies stock identity.
If the kernel changed, recovery loads that kernel's installed stock driver instead.
Installed module files and saved Wi-Fi profiles stay untouched.
Reboot loads stock. A kernel hang can defeat the timer; reboot may be necessary.
Recovery prints reboot instructions if the candidate is busy; it never force-unloads a module.
Missing READY_FOR_TRIAL disables loading; read-only --check still works.

Required gates and their current status: [TESTING.md](TESTING.md#required-gates).
