# 04. SAE state machine

Per-attempt lifecycle for WPA3-SAE, with PMF fail-closed rules.

## Principle

SAE mathematics runs in userspace. The kernel transports frames, owns policy, and drives the device
into association only after userspace reports success through external-auth completion.

The device does not understand SAE. It still emits one authentication frame so it enters its own
authenticating state. That frame is a placeholder. It is not the SAE exchange.

## States

| State | Meaning | Entry action | Exit action |
| --- | --- | --- | --- |
| `IDLE` | no connect | none | none |
| `JOIN_DEFERRED` | waiting out rejoin quiet time | schedule join work | issue join |
| `JOIN_SENT` | join accepted | arm RX tap | wait for stub |
| `STUB_WAIT` | waiting for the placeholder auth | start grace timer | start external auth |
| `EXT_AUTH` | userspace owns the exchange | forward auth frames | external-auth result |
| `ASSOC_DRIVE` | auth done, injecting IEs | install IEs, advance | wait for association |
| `ASSOC_WAIT` | association in flight | arm watchdog | link up or failure |
| `RUN` | associated, keys installed | normal data path | disconnect |
| `FAILED` | attempt failed | abort join | cleanup |
| `CANCELLED` | superseded or disconnected | cancel work | cleanup |

## Transition table

| From | Event | Guard | To | Action |
| --- | --- | --- | --- | --- |
| `IDLE` | connect | SAE AKM | `JOIN_DEFERRED` or `JOIN_SENT` | save request, arm |
| `JOIN_DEFERRED` | quiet timer | still desired | `JOIN_SENT` | issue saved join |
| `JOIN_SENT` | placeholder auth alg 3 seen | armed | `STUB_WAIT` | start 40 ms grace |
| `STUB_WAIT` | grace expired | armed | `EXT_AUTH` | request external auth |
| `STUB_WAIT` | 500 ms fallback | armed | `EXT_AUTH` | request external auth |
| `EXT_AUTH` | external-auth status 0 | `bsscfg` valid | `ASSOC_DRIVE` | inject IEs, advance |
| `EXT_AUTH` | external-auth status nonzero | any | `FAILED` | abort join, report |
| `ASSOC_DRIVE` | advance returned | `scb` valid | `ASSOC_WAIT` | arm 3000 ms watchdog |
| `ASSOC_WAIT` | link up event | success | `RUN` | start 4-way watchdog |
| `ASSOC_WAIT` | watchdog | timeout | `FAILED` | abort, report |
| `RUN` | 4-way complete | keys installed | `RUN` | clear watchdog |
| `RUN` | 4-way timeout | 3500 ms | `FAILED` | disconnect |
| any | disconnect | any | `CANCELLED` | cancel all work |
| any | new connect | any | `CANCELLED` then new | bump epoch |

Any transition not listed is illegal. An illegal transition cancels the attempt and reports failure;
it does not guess.

## Placeholder and external START

The placeholder frame:

- is sent for every SAE join,
- is required so the device enters its authenticating state,
- is rejected by the AP,
- must precede the real SAE commit.

If the real commit goes first, the later placeholder makes the AP discard its SAE state and the join
collapses.

Therefore:

1. Wait for the placeholder to be observed, then wait `40 ms`.
2. If the placeholder is not observed, start after `500 ms`.
3. Before START, consume SAE authentication frames; do not forward them.
4. After START, forward every SAE authentication frame to userspace.
5. Consume SAE authentication frames in the kernel; never pass them to the device.

START is idempotent per attempt. A second START without a new attempt is ignored.

## Status forwarding

After external-auth START, forward every SAE authentication status unchanged. The kernel does not
rewrite, normalize or interpret it. Userspace decides.

| Status | Meaning | Kernel action |
| --- | --- | --- |
| `0` | success | forward |
| `76` | anti-clogging token required | forward |
| `77` | unsupported finite cyclic group | forward |
| `126` | hash-to-element required | forward |
| `127` | SAE-PK negotiation | forward |
| other | as received | forward |

Rules:

- A pre-START frame is consumed, including its status.
- A post-START frame is always forwarded, including every nonzero status.
- Only the external-auth completion drives association or abort:
  status `0` drives association; any nonzero status aborts the pending join.
- Never map an unknown status to success.
- Never retry SAE in the kernel. Userspace decides.

## Association IE injection

After SAE success, userspace supplies the association request IEs. The device's association builder
copies an RSN element verbatim when present.

| Field | Value |
| --- | --- |
| Allowed element IDs | `48` (RSN), `244` (RSNX) |
| Storage | `assoc + 0x30` pointer, `assoc + 0x38` length |
| Content | RSN + RSNX exactly as userspace supplied |
| Buffer | writable, host-owned allocation or static host buffer |

Rules:

- Copy into a buffer the host owns for the life of the injection.
- Never point at read-only module data without clearing the pointer before teardown.
- Inject only RSN and RSNX. Drop other elements.
- Length must equal the copied bytes.
- The device may free the buffer on BSS teardown. Clear `+0x30` and `+0x38` and release the host
  buffer before any teardown.
- Stale IEs must not survive into a later non-SAE join.
- Release on every connect, disconnect and detach.

If the association request would not carry the SAE AKM and MFPC bits, fail closed.

## Fail-closed rules

| Condition | Result |
| --- | --- |
| MFP required, AKM is not SAE | reject connect with `-EOPNOTSUPP` |
| SAE connect, no userspace SAE | reject |
| Association without `sae_ok` | refuse to advance |
| Association request without SAE AKM | refuse to send |
| Device-initiated roam during SAE | drop link, report disconnect |
| Missing AP `scb` at advance | abort, report failure |
| IE injection fails | abort, report failure |

No silent downgrade. No Open/PSK fallback for a WPA3-SAE request.

## Timings

| Timer | Value | Purpose |
| --- | --- | --- |
| Stub grace | `40 ms` | let the placeholder frame clear |
| Start fallback | `500 ms` | start SAE if no placeholder observed |
| Association timeout | `3000 ms` | auth done but no association |
| Join retry delay | `1000 ms` | before retrying a failed join |
| Join retry limit | `3` | then report failure |
| Rejoin quiet gap | `2000 ms` | after disconnect |
| 4-way timeout | `3500 ms` | associated but no keys |
| Deauth settle | `40 ms` | observe whether the protected deauth was ACKed |
| SA Query send interval | `300 ms` | between requests |
| SA Query send limit | `3` | total requests, then treat the link as lost |

Timers are per attempt. A new attempt cancels all timers from the previous attempt.

## Join retry

A device-reported join failure, other than "no networks", is retried silently:

1. Wait `1000 ms`.
2. Reissue the saved join for the same BSS.
3. At most `3` retries.
4. Then report the failure.

Do not retry on "no networks". Do not scan-hop in the kernel.

## Cancellation and epochs

Every attempt carries a generation number.

- Increment on connect, disconnect and abort.
- Every asynchronous completion checks its generation before touching state.
- Stale completions are dropped, not applied.
- On cancel: stop the RX tap, cancel all delayed work synchronously, release IEs, clear captured
  pointers.

Work items that must be cancelled:

- start and fallback,
- association watchdog,
- 4-way watchdog,
- join retry,
- deferred join,
- SA Query retry,
- protected teardown worker.

Cancellation must be safe when the work is already running.

## Rejoin quiet time

After a disconnect, wait `2000 ms` before a new join to the same BSS.

- `.connect` runs under the wireless mutex and must not sleep.
- If the quiet time has not elapsed, defer the join to a work item.
- A new connect during the quiet time supersedes the old attempt.

## Roaming

| Event | Action |
| --- | --- |
| Device roam before SAE success | drop link |
| Device roam after `RUN` | userspace-driven reconnect only |
| Band switch during SAE | fail the attempt |
| BSSID change without a new connect | fail the attempt |

The kernel does not silently roam during SAE. A new BSS is a new attempt.

## Link loss

| Cause | Report |
| --- | --- |
| Association timeout | failure, retryable |
| 4-way timeout | disconnect |
| SA Query exhausted | disconnect |
| Unprotected deauth/disassoc | do not obey; SA Query if reason 6 or 7 |
| Protected deauth/disassoc | apply reason |
| Reset or down | disconnect |

## Invariants

1. Userspace owns SAE; the kernel owns transport and policy.
2. The placeholder precedes the real commit.
3. Pre-START frames are consumed.
4. Every post-START status is forwarded unchanged.
5. Association happens only after external-auth status `0`.
6. Injection is RSN/RSNX only, host-owned, released before teardown.
7. Stale completions are dropped by generation.
8. No downgrade, ever.
