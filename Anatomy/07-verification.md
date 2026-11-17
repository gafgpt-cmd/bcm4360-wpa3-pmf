# 07. Verification

Deterministic host proofs, and a separate hardware matrix.

## Two classes

| Class | Runs on | Proves | Does not prove |
| --- | --- | --- | --- |
| Host tests | any Linux host, no radio | parsing, crypto contracts, replay, ownership, recovery stubs | kernel integration, firmware behavior, on-air security |
| Hardware tests | the target machine and a real AP | association, key installation, link recovery | other boards, regulatory compliance, sustained throughput unless measured |

No test is described as radio-free when it needs an AP. The hardware matrix is explicit.

## Host test entry point

```sh
bash tests/check.sh
```

The suite compiles and runs the host PMF header, the RX wrapper harness, and the recovery and
install stubs. It uses OpenSSL as the host crypto backend. The kernel module uses the Linux AES-CMAC
library. Host tests do not establish kernel or radio behavior.

## Crypto vectors

Vectors include the key, the message and the expected tag or ciphertext. An expected value is never
derived from the code under test.

| Test | Source | Assertion |
| --- | --- | --- |
| AES-CMAC empty message | RFC 4493 | key `2b7e...4f3c`, empty message, tag `bb1d6929e95937287fa37d129b756746` |
| AES-CMAC multi-block | RFC 4493 | each published key, message and tag matches |
| AES-CCM | RFC 3610 | key, nonce, AAD, plaintext and expected ciphertext plus MIC match |
| BIP sign/verify | independent construction | verify accepts, then replay rejects |
| BIP tamper | flip each body bit | verify rejects |
| CCMP encrypt/decrypt | independent construction | round trip, then replay rejects |
| CCMP tamper | flip each body bit | decrypt or MIC fails |

## Frame location

| Case | Input | Expected |
| --- | --- | --- |
| Base | len 128, offset 38, pad clear | `44` |
| Pad | `rx[0x10] = 4` | `46` |
| Short | len 19 | `EINVAL` |
| No room | len 45, offset 38 | `EINVAL` |
| Offset huge | offset `0xffffffff` | `EINVAL` |
| Offset above len | offset > len | `EINVAL` |
| Exact fit | len = offset + 6 + pad + 4 | locator accepts |

The exact-fit case proves the locator's bounds arithmetic only. It does not prove that a complete
frame parses; full-frame parser acceptance is a separate case with a real header, body and FCS.

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

Replay advancement is tested after MIC success, not before it.

## Key install

| Case | Expected |
| --- | --- |
| ID `4`, 16 bytes | accept |
| ID `5`, 16 bytes | accept |
| ID `6` | reject |
| Length `15` | reject |
| Sequence length `5` | reject |
| Sequence absent but length `6` | reject |
| Same key, higher IPN | accept, floor raised |
| Same key, lower IPN | accept, floor unchanged |
| Different key | accept, floor replaced |
| Different key, then old-frame verify | reject |

## BIP frame checks

| Mutation | Expected |
| --- | --- |
| Any body bit | reject |
| MMIE key ID | reject if not `4`/`5` |
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
| Epoch changed between decrypt and verify | reject |

## RX path

| Case | Expected |
| --- | --- |
| Non-linear skb | drop, no parse |
| `skb->len < 20` | drop |
| Bad version bits | drop |
| Bad offset | drop |
| Frame shorter than 24 after FCS | drop |
| Trailing FCS | excluded once from parser/userspace lengths; retained in object RX buffer |
| SAE auth before START | consumed, no userspace event |
| SAE auth after START | forwarded, then freed |
| SAE auth passed to the object | never |
| Unprotected deauth | dropped |
| Unprotected deauth reason 6/7 | SA Query started |
| Protected deauth valid | applied |
| Protected deauth bad MIC | dropped |
| Group robust frame bad BIP | dropped |
| Block Ack after valid CCMP | Protected bit cleared; prefix + header + plaintext + original FCS re-injected |
| Block Ack without valid CCMP | dropped |
| Data frame | passed through |
| Version 1/2/3 on every handled subtype | dropped without normalization |

## SAE transport

| Case | Expected |
| --- | --- |
| Placeholder observed | START after 40 ms |
| Placeholder absent | START after 500 ms |
| Duplicate START | ignored |
| External-auth status 0 | association drive |
| Status 76/77/126/127 | forwarded exactly once, no kernel interpretation |
| Unknown status | forwarded, never mapped to success |
| Association without `sae_ok` | refused |
| Association without SAE AKM | refused |
| MFP required, non-SAE AKM | `EOPNOTSUPP` |
| Device roam during SAE | link dropped |
| Missing scb at advance | abort |
| IE injection failure | abort |
| Stale completion after epoch bump | dropped |
| Disconnect during any state | all work cancelled |

## Association IE ownership

| Case | Expected |
| --- | --- |
| Inject RSN + RSNX | pointer and length set |
| Inject other element | dropped |
| Teardown | pointer and length cleared before free |
| Later non-SAE join | no stale IE |
| Read-only source left installed | refused; clear before teardown |

## Concurrency and ownership

| Case | Expected |
| --- | --- |
| Parallel key install | serialized, one winner |
| Verify during rekey | old or new key, never mixed |
| Verify during teardown | bounded, no use after free |
| Cancel while running | callback tolerates cancellation |
| Concurrent reset and RX | no double free, no index reuse |
| Stress under lockdep | no inversion, no sleep in atomic |
| RX consumed | freed once |
| RX handed to the object | not freed by host |
| TX queued | not freed by host |
| TX never queued | freed with the drop flag |
| Double delivery attempt | rejected by construction |

Fault injection forces allocation failure at each step and confirms unwind.

## Hardware matrix

Run only with the built candidate, the exact kernel and a real AP. Record dates, module hash, kernel
and AP security mode. Never record passwords, keys or full profiles.

| Scenario | Expected | Current evidence |
| --- | --- | --- |
| SAE-only or transition AP | negotiated SAE, `pmf=2`, BIP group cipher (policy and cipher only) | observed |
| Explicit disconnect and reconnect, other band | fresh SAE and DHCP | observed |
| Radio off/on | fresh SAE and DHCP | observed |
| Normal internet over Wi-Fi | DHCP and HTTPS 200 after the USB tether is removed | observed |
| Stock restore | HTTPS 200 after restore | observed |
| Protected Block Ack on a live link | protected ADDBA/DELBA accepted | not tested on air |
| Wrong password | clean failure, no fallback | not tested |
| Protected deauth from AP | verified and applied | not tested on air |
| Replayed management frame | rejected | not tested on air |
| Group-addressed robust frame with bad MIC | dropped | not tested on air |
| Weak signal loss and return | SA Query resolves or link drops | not tested |
| AP group rekey | traffic continues | not tested |
| Sustained data plane | no MIC or replay false positives; measured rate recorded | not measured |
| Suspend/resume | upstream support inherited | not part of this matrix |
| Long soak | no leak or stall | not performed |

Absence of a row is not support. A row marked not tested is an open hardware check.

## Determinism gates

- No network access.
- No wall-clock dependence; use a monotonic test clock.
- No random seed without a fixed value.
- No sleep longer than the bounded timer under test.
- No reliance on host CPU count.
- No secret material in fixtures.
- No test that greps source text and calls that behavior.

## Pass criteria

A change is verified when:

1. Every crypto vector matches an independent key, message and tag.
2. Every malformed input is rejected without a crash.
3. Every replay, rekey and epoch case matches the table.
4. Every SAE status is forwarded or consumed exactly as specified.
5. Every packet is freed exactly once under fault injection.
6. Lockdep is clean under stress.
7. No secret appears in output.
8. The hardware rows claimed as observed were actually run; every other row stays open.

Host tests do not establish kernel, firmware, radio or on-air behavior. The hardware matrix is the
only place a hardware claim is recorded.
