# Test record

## Required gates

- GPT Astra: INCOMPLETE. Provider blocked further review after identifying the TX rekey race;
  the race was fixed, but the required independent review remains open.
- OpenCode Kimi K3.1: pending independent review.
- no-mistakes: pending completion of the current run.
- Gitleaks: prior local scan and synthetic-credential detection control passed.
- GitHub CI: pending; no self-hosted runner is available for the new repository.
  A full local mirror of the configured test and lint commands may satisfy the CI gate;
  it must pass on the final source revision and does not replace Astra or Kimi.
- Live validation: pending; see the protocol and results below.

No deployment while required pre-deployment gates remain open. Local checks are not
independent-review approval or hardware evidence. No upstream PR before genuine live validation.

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
  automatic/manual saved-module restore, changed installed version, dependency
  removal/reload, dependency/metadata failure, wrong saved identity, tamper, kernel mismatch.
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

## Review regression verification

Focused commands: `python3 tests/rx.py`, `python3 tests/recovery.py`, and
`make -C /lib/modules/"$(uname -r)"/build M="$PWD/wl-src" src/wl/sys/wl_cfg80211_hybrid.o`.
The RX harness compiles the actual RX entrypoint and workers, using OpenSSL CCM
and kernel-boundary stubs. Its oracle is the 802.11 CCMP PN ordering contract:
SA-Query PN 10 takes effect at RX before Block Ack PN 11 arrives; authenticated
teardown PN 10 remains effective when its worker runs after Block Ack PN 11.
Replayed and unauthenticated frames must not be processed.
It also exercises ordinary requests, responses, Block Ack
and PMF-inactive pass-through. Recovery stubs model kmod removing dependencies
and insmod requiring them; both candidate and saved-module insertion must work.
These commands are local evidence only. The configured test command invokes
both regression harnesses; the existing push/pull-request workflow invokes that
same command without path filters or job conditions. No remote CI was run here.
Current approval status is tracked in [Required gates](#required-gates).

Results: RX and recovery regressions passed; the Gitleaks synthetic credential
control passed; the changed driver object compiled against 7.2.15-1-omarchy.
The pre-review revision `a1f2655` fails the delayed-SA-Query regression and the recovery
dependency-removal regression. The kernel object build reports four existing
missing-prototype warnings on Broadcom wrapper entrypoints. Full module linking
was not attempted because the proprietary blob is absent from this worktree.

## Worker-contention regression verification

Focused verification: `python3 tests/rx.py` and compilation of the changed
`wl_cfg80211_hybrid.o` with the kernel-object command above. The executable
harness runs the actual RX wrapper, authentication boundary, SA-Query handlers
and association-loss function. OpenSSL authenticates unicast CCMP and group BIP;
only kernel scheduling, radio transmission and cfg80211 boundaries are stubbed.
The oracle is 802.11 SA-Query transaction matching and authenticated teardown:
worker occupancy must not cause a matching response to time out or suppress an
authenticated teardown. Cases cover queued/running request and probe workers,
unmatched responses, both teardown subtypes and delivery forms, first teardown
reason, reason-code limits, forged frames, key replacement and session shutdown.
The existing test command and unconditional push/pull-request workflow invoke
this harness through `tests/check.sh`; local results do not establish remote CI.
No live radio operations are part of this verification.

Results: the expanded RX harness passed with ASan/UBSan, and the changed kernel
object compiled successfully. The pre-contention-fix revision `e94e952` reproduces
all three contention failures independently: matching response, unicast teardown
and group teardown.
Scheduling hooks exercise deterministic interleavings; this is not live kernel
concurrency or hardware evidence. No complete suite, remote CI or deployment ran
in this review phase.
