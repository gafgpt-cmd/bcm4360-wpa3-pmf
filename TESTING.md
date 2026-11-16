# Test record

## Local evidence

- PMF host tests: passed with ASan and UBSan.
- AES-CMAC: RFC 4493 empty-message known answer.
- BIP: signed broadcast frame accepted; byte/bit mutations rejected.
- Replay: repeated/lower PN, wraparound and forged high PN rejected.
- Keys: slots 4/5 independent; same-key reinstall keeps replay floor.
- Invalid keys: index, length and sequence rejected without state mutation.
- Frames: short, missing/altered MIC, oversized, wrong flags rejected.
- RX prefix: metadata + 6-byte PHY header; optional 2-byte pad covered.
- Recovery: isolated command stubs; no real module operations.
- Recovery cases: dry run, timer failure, load failure, timer-before-unload,
  automatic/manual restore, stock-load fallback, tamper, kernel mismatch.
- Exact-kernel module build: passed before final review.
- Hardware behavior: not tested yet.

## Hardware test protocol

Record date, source commit, module SHA-256, kernel, chipset and AP security mode.
Never record passwords, keys or full connection profiles.
Use the prepared temporary bundle. Keep the original module backup.
Run from a local terminal. Save work; expect a brief Wi-Fi interruption.

1. Record stock baseline: link state, security mode, gateway reachability.
2. Verify bundle with trial.sh --check.
3. Run elevated trial; confirm timer is active before swap.
4. Join a WPA3-only or transition AP; confirm negotiated SAE, not WPA2 fallback.
5. Check gateway and internet reachability throughout the trial.
6. Confirm automatic stock restoration; repeat manual restore as a separate test.
7. Reboot; confirm installed stock module and reconnect.
8. After the short trial passes, design a separately approved longer test:
   AP group rekey, reconnect, wrong password, sustained traffic, suspend/resume.
9. Verify actual protected traffic with an independent monitor-mode capture device.
   Check valid group teardown, forged MIC rejection and replay rejection on-air.

No on-air injection against unrelated networks.
No upstream PR while any required live test remains unverified.

## Live results

Pending privileged trial and an identified WPA3 AP.
Do not replace this line with a pass based on host tests or module compilation.
