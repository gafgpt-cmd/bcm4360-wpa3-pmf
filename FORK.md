# Local patches

Base: linux-7.3-wpa3-transition-mode, 4d49a9b.
Source-only fork. Validation and publication status: [TESTING.md](TESTING.md).

| Upstream file | Change |
| --- | --- |
| wl-src/src/wl/sys/wl_cfg80211_hybrid.c | BIP-CMAC key lifecycle, ordered RX verification/effects, teardown priority, worker cancellation; forward SAE statuses only after external-auth START; reject unsupported RX versions; failed join reports no BSSID (stock blamed the previous AP); A-MSDU parts bypass PMF frame parsing; SA-Query response needs a station entry |
| wl-src/src/wl/sys/wl_linux.c | Packet-free bridge carries TX/RX ownership |
| wl-src/src/wl/sys/wl_linux.h | Matching packet-free declaration |
| dkms/dkms.conf | Version 1.2, no deprecated CLEAN, x86_64 only |
| dkms/dkms-prebuild.sh | Refuse a blob whose SHA-256 differs from the pinned one |
| dkms/install.sh | Arch rewrite: build all kernels before touching stock, exclude stock via /etc/dkms override, --remove, --stage, no service |
| dkms/README.md | Arch usage, stock coexistence, rollback |

Own files: wl_pmf.h, tests/, recovery/, local check configuration.
Crypto: Linux AES-CMAC library. No custom AES.
Framing reference: hostap wlantest/bip.c; Linux ieee80211 robust-frame classification.
