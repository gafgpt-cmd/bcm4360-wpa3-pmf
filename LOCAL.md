# Local PMF patch

Base: upstream WPA3 bridge + kernel compatibility revision.
Scope: BIP-CMAC-128, replay checks, RX framing, temporary recovery.
No persistent installation. No upstream PR until hardware tests pass.

## Check

Needs: C compiler, OpenSSL development files, Python 3, ShellCheck, Gitleaks.

```bash
bash tests/check.sh
shellcheck recovery/*.sh tests/check.sh
gitleaks dir --redact --no-banner .
```

Tests execute the PMF header with OpenSSL as the host crypto backend.
Kernel module uses Linux AES-CMAC. No custom AES implementation.
Host tests do not prove kernel integration or radio behavior.
Current module requires the kernel AES-CMAC library API.

Build: existing build.sh, pristine blob from installed distribution package, matching kernel headers.
Do not run upstream dkms/install.sh unchanged.

## Temporary trial bundle

Keep beside each other:

- trial.sh, revert.sh, SHA256SUMS
- READY_FOR_TRIAL: candidate checksum, created only after required review/gate approvals
- candidate/wl.ko
- backup/wl.ko, KERNEL, SRCVERSION, SHA256SUMS

```bash
bash trial.sh --check
sudo bash trial.sh
sudo bash revert.sh
```

Trial verifies hashes, kernel, loaded stock identity and signature policy.
Copies recovery files into a root-owned runtime directory.
Arms a 120-second system timer before unloading stock.
Timer always restores stock. Failed loading also restores stock.
Recovery verifies stock identity and falls back to the saved module.
Installed module files and saved Wi-Fi profiles stay untouched.
Reboot loads stock. A kernel hang can defeat the timer; reboot may be necessary.
Busy unload prints reboot instructions; it never force-unloads a module.
Missing READY_FOR_TRIAL disables loading; read-only --check still works.

## Required independent reviews

- GPT Astra: incomplete; provider blocked the review after reporting a rekey race.
- OpenCode Kimi K3.1: pending.
- no-mistakes and forge checks: pending.

Deployment requires the final source revision to pass all gates.
