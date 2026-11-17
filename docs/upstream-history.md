# Upstream engineering history

Provenance: this page preserves the engineering notes and test history from the upstream project
this fork is based on (`linux-7.3-wpa3-transition-mode`, base commit `4d49a9b`). The text is
historical. This fork later added software PMF, changed the RX path and replaced the validation
record. Current behavior and evidence are in [TESTING.md](../TESTING.md); current changes are in
[FORK.md](../FORK.md).

Nothing here is a current guarantee. Values reported below were measured by the upstream author on
the hardware and software of that time.

## Strategy

The vendor object is a non-stripped, relocatable ELF `.o`, not a final firmware image. The upstream
approach was to link new C against it and redirect a few call sites rather than rewrite it:

- `wpa_supplicant` runs SAE and the 4-way handshake, because the object's crypto is SHA-1-only and
  SAE needs SHA-256.
- A cfg80211 glue bridges auth, association and keys.
- Three build steps make the object hookable:
  1. a byte patch that un-clamps the `auth` iovar so it accepts SAE (`alg=3`),
  2. symbol globalization for the object's internal call targets,
  3. relocation rewriting so calls to `wlc_recv`, `wlc_sendauth`, `wlc_authresp_client` and
     `wlc_sendmgmt` land in host wrappers.

The upstream notes described starting from the cfg80211 entry points in the open glue and following
iovars into the object: the AKM gate, the join/auth state machine, the auth-to-assoc advancer, RX
dispatch, and the PMK/key path. Every synthetic symbol address was checked against disassembly
before use.

## Porting notes from upstream

1. Confirm the object is a non-stripped relocatable `.o`. A stripped firmware image will not work
   this way.
2. Locate the auth-algorithm clamp, the join/auth/assoc state machine and its success advancer, RX
   dispatch, and PMK/key insertion.
3. Put their addresses in the build symbol list and the relocation redirect targets.
4. Fix the structure offsets used by the glue (`wlc + 0x6a8` TX queue, `bsscfg + 0x320` association,
   `scb + 0x18` BSS). These differ per object version.
5. Iterate on air with a packet capture. Firmware faults are kernel panics, so keep an out-of-band
   console available.

## Failures that shaped the design

- **Hard lock during SAE with no backtrace.** A synthetic symbol address was off by one hex digit,
  landing mid-instruction. Every synthetic symbol must be a real function start. Newer kernels no
  longer run objtool over the object during the final link, so the old build-time warning is gone.
- **No panic capture on the test laptop.** Streaming the kernel log over SSH to a second machine
  flushed the oops before reboot.
- **Association rejected with status 31, MFP policy.** The object built its own PSK/no-MFP RSN
  element. Capturing the supplicant's association IEs and pointing the object's association IE
  buffer at them fixed it, and also satisfied the 4-way RSN check.
- **Key install failed with `-22` for the IGTK.** The hardware key engine has no BIP slot. The IGTK
  was accepted in software; PTK/GTK installed normally.
- **The object never decrypted management frames.** SA Query arrived as ciphertext. The upstream fix
  captured the PTK and used AES-CCM in software via the kernel `ccm(aes)` transform.

## Earlier throughput observation

With 802.11w, the AP protects its Block Ack action frames and drops unprotected ones. Without a
Block Ack agreement there is no aggregation, and the AP keeps resending ADDBA. The upstream notes
reported SAE at about 14 Mbit/s with 17% TX retries where WPA2-PSK reached about 190 Mbit/s. After
decrypting the AP's Block Ack frames in software and protecting the object's own, the same link was
reported at 150-210 Mbit/s down with 0% retries.

Those figures belong to the upstream build and test setup. This fork does not claim them. Measure
your own link before publishing a rate.

Module parameter `re_pmf_ba`: `2` (default) full handling, `1` decline the AP's ADDBA, `0` off.

## Earlier stalled-link observation

Under load, a PMF link occasionally went bad without a deauth: after a scan the object's rate control
ran to its top rate and almost nothing was acknowledged, while the stack still showed connected.
The upstream watchdog counted retries in 12-second windows, ignored windows containing a scan, and
probed with SA Query when retries reached 70% of frames sent. A missing answer dropped the link.
Three consecutive bad windows (about 36 seconds) also dropped it, but only in opt-in mode, because
an idle 2.4 GHz link produced false alarms overnight.

Module parameter `re_pmf_stall`: `1` probe only (default), `0` off, `2` probe and drop, `3` test
mode.

The cause of the rate runaway was not understood. It was seen four times in about 60 scans with
Block Ack handling and never in about 150 scans without it.

## Transition-mode AP behavior

An existing WPA3-SAE profile selects WPA3; a WPA2-PSK profile connects with WPA2 by design. On a
consumer WPA2/WPA3 mixed-mode AP the upstream notes reported stable operation with periodic AP
group-key rekeys, repeated disconnect/reconnect cycles and suspend/resume.

The fixes that got there:

- **Association request was malformed.** SAE connects carry WPA3 in `wpa_versions`, which the glue
  did not know, so the object ran with WPA off and added a legacy-WPA IE. The glue also pasted the
  supplicant's whole IE buffer next to the object's own, duplicating Extended Capabilities. The AP
  accepted the station but failed every group rekey and deauthenticated it. The fix injected only
  RSN/RSNX and set `wpa_auth` correctly from the start.
- **The AP dropping the station was invisible.** Its management frames are CCMP-protected and
  unprotected deauths are filtered. The fix verified protected deauth/disassoc in software and
  honoured it; an unprotected class 2/3 deauth triggered a software SA Query; association was probed
  after resume.
- **Two races in the glue.** The host SAE exchange could start before the object's stub auth frame,
  which made the AP drop the join. The cfg80211 BSS lookup used pre-4.1 arguments, so a connect could
  be silently discarded when the scan entry expired.
- **Bounded reconnect.** A failed join was retried silently up to three times; an association with no
  4-way handshake was dropped after 3.5 seconds instead of 10; the protected deauth on disconnect was
  retransmitted if unacknowledged; at least 2 seconds separated disconnect and the next join.

Reported reconnect medians at the time: WPA2-PSK 1.7 seconds, maximum 5.4 seconds; SAE 0.3-1.1
seconds in 16-cycle stress tests. Rejoining the BSS just left was the AP's weak spot and accounted
for an occasional roughly 5-second reconnect.

## Upstream test history

| Kernel | Method | Reported result |
| --- | --- | --- |
| 7.3.0-9 (Ubuntu 26.10) | loaded and used on a BCM4360 laptop with a WPA2/WPA3 mixed-mode AP, 2.4 and 5 GHz | 45-minute SAE soak with periodic AP rekeys, repeated reconnects, clean module reload, wrong password failure, suspend/resume reconnect, no kernel warnings |
| 6.12, 6.14, 6.17, 7.0, 7.1, 7.2 | build only against mainline PPA headers | builds |
| 6.12 (Debian 13) | loaded and used on a second BCM4360 laptop with a WPA3-only AP, previous release | worked |

These are upstream results, not reproductions. This fork's current record is shorter and is kept in
[TESTING.md](../TESTING.md).
