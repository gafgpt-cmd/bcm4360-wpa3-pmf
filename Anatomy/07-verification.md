# 07. Verification

Independent proofs that the contracts hold. Behavior, not recorded history.

## Rules

- Vectors come from standards or are generated independently, not copied from the implementation under test.
- Tests assert properties, not a previous run's counts.
- No test needs a network, a real AP or a live radio.
- Every test is deterministic and bounded.
- No secrets in fixtures, logs or failure output.
- A random-looking value in a fixture is synthetic and documented as such.

## Crypto vectors

| Test | Source | Assertion |
| --- | --- | --- |
| AES-CMAC empty message | RFC 4493 | tag equals `bb1d6929e95937287fa37d129b756746` |
| AES-CMAC multi-block | RFC 4493 | each published tag matches |
| AES-CCM | RFC 3610 | ciphertext and MIC match |
| BIP sign/verify | independent construction | verify accepts, then replay rejects |
| BIP tamper | flip each bit | verify rejects |
| CCMP encrypt/decrypt | independent construction | round trip, then replay rejects |
| CCMP tamper | flip each bit | decrypt or MIC fails |

Do not derive an expected value from the code under test.

## Frame location

| Case | Input | Expected |
| --- | --- | --- |
| Base | len 128, offset 38, pad clear | `44` |
| Pad | `rx[0x10] = 4` | `46` |
| Short | len 19 | `EINVAL` |
| No room | len 45, offset 38 | `EINVAL` |
| Offset huge | offset `0xffffffff` | `EINVAL` |
| Offset above len | offset > len | `EINVAL` |
| Exact fit | len = offset + 6 + 4 | accepted |
| One byte short | len = offset + 6 + 3 | rejected |

## Replay

| Sequence | Expected |
| --- | --- |
| `0` | reject |
| `1` | accept, floor 1 |
| `1` again | reject |
| `3` | accept, floor 3 |
| `2` | reject |
| `0x1000000000000` | reject |
| `0xffffffffffff` | accept |
| `0` after `0xffffffffffff` | reject |

## Key install

| Case | Expected |
| --- | --- |
| ID `4`, 16 bytes | accept |
| ID `5`, 16 bytes | accept |
| ID `6` | reject |
| Length `15` | reject |
| Sequence length `5` | reject |
| Sequence absent, length `6` | reject |
| Same key, higher IPN | accept, floor raised |
| Same key, lower IPN | accept, floor unchanged |
| Different key | accept, floor replaced |
| Different key, then old-frame verify | reject |

## BIP frame checks

| Mutation | Expected |
| --- | --- |
| Any body bit | reject |
| MMIE key ID | reject if not `4/5` |
| MMIE element ID | reject |
| MMIE length | reject |
| MIC bit | reject |
| Destination not group | reject |
| Unsupported subtype | reject |
| Fragment number nonzero | reject |
| Length below minimum | reject |
| Length above maximum | reject |
| Every truncation | reject |
| Reserved frame-control bits set | reject |
| Bits cleared by the AAD mask | accept |

## CCMP frame checks

| Mutation | Expected |
| --- | --- |
| PN `0` | reject |
| PN above 48-bit | reject |
| PN below floor | reject |
| Wrong A2 in nonce | reject |
| Wrong AAD | reject |
| Wrong MIC | reject |
| Rekey then old PN | reject |
| Rekey then new PN | accept |

## RX path

| Case | Expected |
| --- | --- |
| Non-linear skb | drop, no parse |
| `skb->len < 20` | drop |
| `skb->len > 4096` | drop |
| Bad version bits | drop |
| Bad offset | drop |
| Frame shorter than 24 after FCS | drop |
| FCS present in body | stripped once |
| SAE auth before START | consumed |
| SAE auth after START | forwarded |
| SAE auth passed to blob | never |
| Unprotected deauth | dropped |
| Unprotected deauth reason 6/7 | SA Query started |
| Protected deauth valid | applied |
| Protected deauth bad MIC | dropped |
| Group robust frame bad BIP | dropped |
| Data frame | passed through |

## SAE state

| Case | Expected |
| --- | --- |
| Placeholder observed | START after 40 ms |
| Placeholder absent | START after 500 ms |
| Duplicate START | ignored |
| Status 0 | association drive |
| Status 76/77/126/127 | forwarded, attempt fails |
| Unknown status | forwarded, never success |
| Association without `sae_ok` | refused |
| Association without SAE AKM | refused |
| MFP required, non-SAE AKM | `EOPNOTSUPP` |
| Firmware roam during SAE | link dropped |
| Missing scb at advance | abort |
| IE injection failure | abort |
| Stale completion after epoch bump | dropped |
| Disconnect during any state | all work cancelled |

## Assoc IE ownership

| Case | Expected |
| --- | --- |
| Inject RSN + RSNX | pointer and length set |
| Inject other element | dropped |
| Teardown | pointer and length cleared before free |
| Later non-SAE join | no stale IE |
| Read-only source | refused |

## Concurrency

| Case | Expected |
| --- | --- |
| Parallel key install | serialized, one winner |
| Verify during rekey | old or new, never mixed |
| Verify during teardown | bounded, no use after free |
| Cancel while running | callback tolerates cancellation |
| Concurrent reset and RX | no double free, no index reuse |
| Stress under lockdep | no inversion, no sleep in atomic |

## Live behavior

Run on real hardware only when the build is otherwise green.

| Scenario | Expected |
| --- | --- |
| Connect to SAE AP | association with SAE AKM and MFP |
| Disconnect and reconnect same BSS | bounded quiet gap, no stale IE |
| Band switch | new attempt, no silent roam |
| Radio off/on | link loss reported, clean down/up |
| Password change | old keys cleared, new attempt |
| Weak signal | SA Query resolves or link drops |
| Bulk traffic | no MIC failures, no replay false positives |
| PMF AP sends unprotected deauth | not obeyed |
| PMF AP sends protected deauth | applied |
| Rekey | traffic continues, PN resets per contract |
| Suspend/resume | not supported; must report link loss if attempted |

Suspend is listed to prove it fails safely, not to claim support.

## Ownership tests

| Case | Expected |
| --- | --- |
| RX consumed | freed once |
| RX handed to blob | not freed by host |
| TX queued | not freed by host |
| TX dropped before queue | freed with `send = true` |
| Double delivery attempt | rejected by construction |
| Free under re-entrant lock | forbidden; test detects |

Fault injection: force allocation failure at each step and confirm unwind.

## Determinism gates

- No network access.
- No wall-clock dependence; use a monotonic test clock.
- No random seeds without a fixed value.
- No sleeps longer than the bounded timer under test.
- No reliance on host CPU count.
- No secret material in fixtures.

## Pass criteria

A change is verified when:

1. Every crypto vector matches an independent source.
2. Every malformed input is rejected without a crash.
3. Every replay and rekey case matches the table.
4. Every SAE transition is legal or explicitly fails.
5. Every packet is freed exactly once under fault injection.
6. Lockdep is clean under stress.
7. No secret appears in output.
8. Live behavior matches the table, or the deviation is documented as a limit.

## What verification does not prove

- It does not prove the device is fully understood.
- It does not prove Mode A works on another artifact.
- It does not prove regulatory compliance.
- It does not prove suspend, power save or hardware crypto.
- It does not replace board-specific calibration testing.
