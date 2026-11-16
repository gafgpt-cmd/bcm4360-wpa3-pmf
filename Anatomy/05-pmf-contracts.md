# 05. PMF contracts

Protected management frames: crypto, replay, key lifetime, SA Query, teardown.

## Scope

Host software implements PMF. The device may offload data-frame CCMP; it must not be trusted for management protection.

Two mechanisms:

| Mechanism | Protects | Cipher |
| --- | --- | --- |
| BIP | group-addressed robust management | AES-CMAC |
| CCMP | individual robust management | AES-CCM |

## Keys

| Key | ID | Size | Index |
| --- | --- | --- | --- |
| IGTK | `4` | 16 bytes | `0` |
| IGTK | `5` | 16 bytes | `1` |
| PTK (TK) | n/a | 16 bytes | n/a |

IGTK install rules:

- Accept only IDs `4` and `5`.
- Key must be exactly 16 bytes.
- Optional sequence is 6 bytes, little-endian IPN.
- Same key bytes: keep the higher IPN, ignore a lower one.
- Different key bytes: replace, set IPN to the supplied value.
- Reject any other ID or length before touching state.
- Serialize install, verify and delete.

PTK install rules:

- 16 bytes.
- Key change resets TX PN to `1`.
- First install after association starts TX PN at `0x1000000`.
- Key delete clears the ready flag before freeing any transform.

## BIP

### MMIE layout

| Offset | Size | Field |
| --- | --- | --- |
| `0` | 1 | element ID `76` |
| `1` | 1 | length `16` |
| `2` | 2 | key ID, little-endian |
| `4` | 6 | IPN, little-endian |
| `10` | 6 | reserved |
| `16` | 8 | MIC |

The MMIE is the last 18 bytes of the frame.

### BIP input

```text
AAD  = FC[0] || (FC[1] & ~0x38) || A1 || A2 || A3     (20 bytes)
data = frame[24 .. len-8]                               (body, MMIE included)
pad  = 8 zero bytes
MIC  = AES-CMAC(IGTK, AAD || data || pad)[0..8]
```

| Field | Bytes |
| --- | --- |
| AAD | 20 |
| Body | `len - 24 - 8` |
| Pad | 8 |
| MIC | 8 |

Frame acceptance before crypto:

| Check | Reject when |
| --- | --- |
| Length | `< 44` or `> 4096` |
| Destination | address 1 is not group-addressed |
| Frame control | reserved or unsupported bits set |
| Fragment | fragment number nonzero |
| Subtype | not deauth, disassoc or action |
| MMIE | element ID not `76` or length not `16` |
| Key ID | not `4` or `5` |

Then:

1. Read IPN.
2. Require key valid.
3. Require `IPN > last`.
4. Compute MIC over the AAD and body.
5. Compare 8 bytes in constant time.
6. Advance IPN only on success.

Replay:

- Reject IPN `0`.
- Reject IPN `> 0xffffffffffff`.
- Reject IPN `<= last`.
- Advance after MIC success, before dispatch.

## CCMP

### Nonce

13 bytes:

| Offset | Size | Value |
| --- | --- | --- |
| `0` | 1 | `0x10` (management, priority 0) |
| `1` | 6 | address 2 (transmitter) |
| `7` | 6 | PN bytes `5,4,3,2,1,0` |

### AAD

22 bytes:

| Offset | Size | Value |
| --- | --- | --- |
| `0` | 2 | frame control, protected bit set, retry/power/more-data cleared |
| `2` | 18 | address 1, address 2, address 3 |
| `20` | 2 | sequence control, fragment bits only |

### CCMP header and PN

- 8-byte CCMP header precedes the encrypted body.
- PN is 48 bits, assembled from header bytes `0,1,4,5,6,7`:

```text
PN = h[0] | h[1]<<8 | h[4]<<16 | h[5]<<24 | h[6]<<32 | h[7]<<40
```

- MIC is 8 bytes, appended after the ciphertext.
- AES-CCM with 13-byte nonce, 22-byte AAD, 8-byte MIC.

Replay:

- Reject PN `0`.
- Reject PN `> 0xffffffffffff`.
- Reject PN `<= last`.
- Advance before dispatch.

TX:

- Fetch and increment a 64-bit counter under a spinlock.
- Reject `0` or `> 0xffffffffffff`.
- Write PN little-endian into the CCMP header.
- Initial value `0x1000000`.
- After a PTK change, reset to `1`.

## Constant time

- Compare MICs with a constant-time primitive.
- Never branch on key bytes, MIC bytes or secret lengths.
- Never log key material, PN-derived secrets or plaintext bodies.
- Zeroize key buffers on delete and detach.

## Key lifetime

| Event | IGTK | PTK |
| --- | --- | --- |
| Association | install per AP | install after 4-way |
| Rekey | replace on new key bytes | replace, reset TX PN |
| Disconnect | clear | clear |
| Reset | clear | clear |
| Detach | clear and free transforms | clear and free transforms |

Rules:

- A key is unusable until `valid` is set.
- Clear `valid` before freeing a transform.
- A stale frame must not verify against a replaced key.
- Same-key reinstall must not lower the IPN floor.

## SA Query

Purpose: confirm the link when the AP claims we are not associated, instead of obeying a spoofed teardown.

Trigger:

- Unprotected deauth or disassoc with reason `6` or `7`.
- Link check after resume or repeated TX failure.

Frame:

| Field | Value |
| --- | --- |
| Category | `8` (SA Query) |
| Action | `0` request, `1` response |
| TID | transaction identifier, 2 bytes |

Procedure:

1. Send request with a fresh TID.
2. Wait `300 ms`.
3. Retry up to `3` times.
4. Any matching response clears the transaction.
5. No response after the limit: treat the link as lost, disconnect.

Rules:

- Match response by category, action and TID.
- A response for a different TID does not clear the transaction.
- Only one transaction in flight.
- The responder runs in process context; the RX path only queues.
- Do not obey the triggering teardown while the query is unresolved.
- If the link is genuinely gone, the query fails and the disconnect proceeds.

## Authenticated teardown

| Received frame | Protected | Action |
| --- | --- | --- |
| Deauth | yes, valid MIC | apply reason, disconnect |
| Disassoc | yes, valid MIC | apply reason, disconnect |
| Deauth | no | drop; reason 6/7 -> SA Query |
| Disassoc | no | drop; reason 6/7 -> SA Query |
| Deauth | yes, bad MIC | drop |
| Deauth | no, other reason | drop |

Never apply an unprotected teardown as authoritative. A protected teardown is authoritative after MIC and replay checks.

Reason codes that indicate a lost association (`6`, `7`) are the ones that warrant SA Query rather than immediate disconnect.

## Block Ack under PMF

- Block Ack action frames are robust management frames.
- They must be CCMP-protected when PMF is active.
- Verify before handling or re-injecting.
- Do not pass unverified Block Ack frames to the device.

## Group-addressed robust management

- BIP frames normally have no Protected bit.
- Do not require the Protected bit for group-addressed robust frames.
- Verify BIP first; dispatch only on success.
- A group frame with a bad MIC is dropped, not forwarded.

## Protocol vs local policy

| Rule | Kind | Notes |
| --- | --- | --- |
| IGTK IDs `4/5` | protocol | fixed by 802.11 |
| MMIE `76`, length `16` | protocol | fixed |
| BIP MIC `8` | protocol | fixed |
| CCMP MIC `8` | protocol | fixed |
| Nonce and AAD layout | protocol | fixed |
| PN/IPN 48-bit | protocol | fixed |
| Reject PN `0` | local policy | conservative; matches current host |
| Initial TX PN `0x1000000` | local policy | avoids short-session reuse |
| SA Query `300 ms` x `3` | local policy | bounded, tunable |
| Drop unprotected teardown | protocol plus policy | 802.11w defense |
| Only one SA Query in flight | local policy | simplification |

Local policy may change values. Protocol layout may not.

## Invariants

1. Management protection is host software.
2. Verify MIC before any side effect.
3. Advance replay before dispatch.
4. MIC compare is constant time.
5. A replaced key invalidates the old floor.
6. No unprotected teardown is authoritative.
7. Group robust frames use BIP and need no Protected bit.
8. Keys are zeroized on delete and detach.
