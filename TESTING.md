# Test record

## Required gates

- GPT Astra: WAIVED by owner on 2026-11-16. Earlier review found the
  RX protocol-version defect, fixed in `bdf1fac`; final recheck was refused.
- OpenCode Kimi K3.1: WAIVED by owner on 2026-11-16. Quota stopped its review.
  Neither waiver counts as model approval. No further named-model review required.
- no-mistakes: run `01M7JZQCDAHF5868KVDGD7DG0S` completed through source push
  at `88fae2f`; no source review findings. Passed-with-override solely for
  inconclusive live validation; hardware validation remains pending.
- Gitleaks: directory and 25-commit history scans passed; detection control passed.
- CI evidence: [Remote source checks](#remote-source-checks).
- Live validation: pending; see the protocol and results below.

The owner waived only Astra/Kimi. Keep the remaining checks and live-test requirements.
Offline checks do not establish hardware behavior. No upstream PR before genuine live validation.
Prepare READY_FOR_TRIAL only for an agreed temporary trial with verified recovery files.
Rebuild the phase-guard candidate and refresh bundle hashes after source gates pass.
Trials are user-run; keep the verified USB tether connected throughout.
The source pipeline must not operate the radio, use root/sudo, restart services,
load/unload modules, or change network profiles or passwords.
Publication scope: feature-branch source only; no main push, merge, upstream PR,
proprietary blob or generated module publication.

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
  automatic/manual saved-module restore, changed installed version, shared-stack
  preservation, dependency loading/failure, metadata failure, wrong saved identity,
  tamper, kernel mismatch, busy-unload refusal, default 120-second and provisional
  3600-second durations, provisional timer ordering/failure cleanup and invalid arguments.
- Exact-kernel module build: `bdf1fac` passed against 7.2.15-1-omarchy,
  using a source archive and unchanged distribution blob outside Git.
  Module SHA-256: `5a47ba7af2f4ad44196035a49105af7d4948ad98e79b034f3e77e9e28bfb9da8`.
  Rebuild after any driver or build-source change.
- Hardware trials: initial SAE connection succeeded; reconnect failed. See [Live results](#live-results).
  Low-signal stalls and GrapheneOS Pixel 9 Pro hotspot failures are reported;
  hardware causes remain hypotheses. Neither has an established fix.

## Hardware test protocol

Record date, source commit, module SHA-256, kernel, chipset and AP security mode.
Never record passwords, keys or full connection profiles.
Use the prepared temporary bundle. Keep the original module backup.
Run from a local terminal. Save work; expect a brief Wi-Fi interruption.
Trial authorization and uplink requirement: [Required gates](#required-gates).

1. Record stock baseline: link state, security mode, gateway reachability.
2. Verify bundle with trial.sh --check.
3. Run elevated trial; confirm timer is active before swap.
4. Join a WPA3-only or transition AP; confirm negotiated SAE, not WPA2 fallback.
5. Check gateway and internet reachability throughout the trial.
6. Confirm automatic stock restoration; repeat manual restore as a separate test.
7. Reboot; confirm installed stock module and reconnect.
8. After the short trial passes, design a separately approved longer test:
   AP group rekey, reconnect, wrong password, sustained traffic, suspend/resume;
   weak-signal loss/return and recovery without reboot.
   Test the phone hotspot separately: open, WPA2, WPA3, transition; 2.4/5 GHz.
9. Verify actual protected traffic with an independent monitor-mode capture device.
   Check valid group teardown, forged MIC rejection and replay rejection on-air.

No on-air injection against unrelated networks.
No upstream PR while any required live test remains unverified.

## Live results

First timed trial: candidate module loaded; association NOT reached.
Radio enabled/unblocked, but supplicant could not initialize the interface.
The 120-second service restored the original module identity successfully.
Wi-Fi remained unavailable after restore. Restarting the supplicant and cycling
NetworkManager management of the Wi-Fi device restored stock association and DHCP.
USB tethering kept the agent online; ordinary HTTPS returned200.
No WPA3/PMF, phone-hotspot or weak-signal result established by this trial.

Corrected helpers: secured normal-network authentication and DHCP succeeded.
Automatic stock restore succeeded; no shared cfg80211 reload observed.
Two later hotspot trials completed SAE, association, CCMP key negotiation and DHCP.
Both failed after an explicit disconnect/reconnect; repeated status77 rejections.
Debug trace: the placeholder response reached supplicant before external-auth START,
triggering failure callbacks and aborting the join. A later START returned EINVAL.
Stock restored automatically and reconnected; USB tether remained available.
PMF status was not captured while connected. VPN blocked a Wi-Fi gateway probe.
No throughput, wrong/right password, weak-signal or full transition-matrix pass.
The new phase guard requires another gated build and hardware trial.

First-trial logs showed cfg80211 reinitialization during both swaps with the old
recursive `modprobe -r` helpers. No control-family ID mismatch was measured.
Reference: [kmod removal semantics](https://github.com/kmod-project/kmod/blob/master/man/modprobe.8.scd).
Current lifecycle: [Temporary trial bundle](LOCAL.md#temporary-trial-bundle).
Corrected-helper live results are recorded above; the recovery regression covers
dependency preservation during both swaps.

## Review regression verification

Focused commands: `python3 tests/rx.py`, `python3 tests/recovery.py`, and
`make -C /lib/modules/"$(uname -r)"/build M="$PWD/wl-src" src/wl/sys/wl_cfg80211_hybrid.o`.
The RX harness compiles the actual RX entrypoint and workers, using OpenSSL CCM
and kernel-boundary stubs. Its oracle is the 802.11 CCMP PN ordering contract:
SA-Query PN 10 takes effect at RX before Block Ack PN 11 arrives; authenticated
teardown PN 10 remains effective when its worker runs after Block Ack PN 11.
Replayed and unauthenticated frames must not be processed.
It also exercises ordinary requests, responses, Block Ack
and PMF-inactive pass-through. Recovery stubs model recursive kmod removal,
single-module removal and insmod dependencies. Both insertion paths must work,
with no shared-stack teardown during candidate loading or stock restoration.
These commands are local evidence only. The configured test command invokes
both regression harnesses; the existing push/pull-request workflow invokes that
same command without path filters or job conditions. No remote CI was run here.
Current approval status is tracked in [Required gates](#required-gates).

Results: RX and recovery regressions passed; the Gitleaks synthetic credential
control passed; the changed driver object compiled against 7.2.15-1-omarchy.
The pre-review revision `a1f2655` fails the delayed-SA-Query regression and the recovery
dependency-removal regression. The kernel object build reports four existing
missing-prototype warnings on Broadcom wrapper entrypoints. Full module linking
was not attempted in that review because the proprietary blob was absent.
Later full-module evidence: [Local evidence](#local-evidence).

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

## SAE status regression

`python3 tests/rx.py` executes the actual RX wrapper.
After external-auth START, SAE statuses 0, 76, 77, 126 and 127 must reach cfg80211 exactly once;
the proprietary blob must receive none. Oracle: supplicant's
[SAE state machine](https://android.googlesource.com/platform/external/wpa_supplicant_8/+/6d755ae3182b871a753223d842aeed5e5f1002b8/wpa_supplicant/sme.c).
The status-zero filter dropped anti-clogging, group negotiation, H2E and SAE-PK.
Before the fix: only status 0 delivered. After: all five delivered.
Full configured test and lint commands passed locally after this change.
This demonstrates frame delivery, not successful hotspot association.

Reconnect regression: consume pre-START replies without notifying supplicant.
The RX executable fails before this phase guard and passes afterward.
It still delivers every listed status after START. The actual START worker is
also executed: disarmed/missing device sends nothing; request failure closes RX delivery.

## Astra RX version regression

The actual RX wrapper accepted unsupported protocol-version bits for deauth,
disassoc, robust Action and SAE Authentication. The proprietary parser masks
those bits; exact-byte wrapper comparisons missed the same subtypes.
Reject versions 1/2/3 before either SAE or PMF dispatch. Never normalize them.
The regression executes all four families with versions 0/1/2/3.
Malformed frames must be freed, with no blob delivery or supplicant event.
Version-zero SAE remains delivered; unprotected robust frames remain dropped.
The test classifier follows Linux's subtype mask and robust-category exclusions.
The new regression fails before the guard. On-air ingress remains untested.

## Remote source checks

Revisions `ded5741` and `bdf1fac`: full test and lint commands passed on the connected CI host.
For `bdf1fac`, the caller also ran both commands locally; all passed.
Remote transcript: `omar-bdf1fac.log`, retained on the originating machine.
The isolated image lacked OpenSSL headers, pkg-config and Gitleaks;
checks ran in fresh host scratch with installed tools. No radio operations.
Follow-up commits require their own remote checks.
Published `88fae2f`: full configured test/lint command mirror passed on the connected host.
External `ci/offline-source` status was green for those commands only.
Repository has no assigned Actions runner. Fleet Actions admission is private-only;
the documented local/remote command mirror does not claim an Actions run.
Actions had no eligible runner; its unstarted run was cancelled.
Do not register a public runner or change admission policy.
