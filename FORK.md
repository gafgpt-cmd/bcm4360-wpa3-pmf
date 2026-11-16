# Local patches

Base: linux-7.3-wpa3-transition-mode, 4d49a9b.
Local work only. No remote publication.

| Upstream file | Change |
| --- | --- |
| wl-src/src/wl/sys/wl_cfg80211_hybrid.c | BIP-CMAC key lifecycle, ordered unicast RX verification and worker cancellation |
| wl-src/src/wl/sys/wl_linux.c | Packet-free bridge carries TX/RX ownership |
| wl-src/src/wl/sys/wl_linux.h | Matching packet-free declaration |

Own files: wl_pmf.h, tests/, recovery/, local check configuration.
Crypto: Linux AES-CMAC library. No custom AES.
Framing reference: hostap wlantest/bip.c; Linux ieee80211 robust-frame classification.
