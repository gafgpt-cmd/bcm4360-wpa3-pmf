# Local patches

Base: linux-7.3-wpa3-transition-mode, 4d49a9b.
Source-only fork. Validation and publication status: [TESTING.md](TESTING.md).

| Upstream file | Change |
| --- | --- |
| wl-src/src/wl/sys/wl_cfg80211_hybrid.c | BIP-CMAC key lifecycle, ordered RX verification/effects, teardown priority, worker cancellation; forward SAE statuses only after external-auth START; reject unsupported RX versions; failed join reports no BSSID (stock blamed the previous AP) |
| wl-src/src/wl/sys/wl_linux.c | Packet-free bridge carries TX/RX ownership |
| wl-src/src/wl/sys/wl_linux.h | Matching packet-free declaration |

Own files: wl_pmf.h, tests/, recovery/, local check configuration.
Crypto: Linux AES-CMAC library. No custom AES.
Framing reference: hostap wlantest/bip.c; Linux ieee80211 robust-frame classification.
