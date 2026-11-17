# 05. PMF contracts

Protected management frames: crypto, replay, key lifetime, SA Query, teardown.

## Scope

Host software implements PMF. The device may offload data-frame CCMP; it is never trusted for
management protection.

| Mechanism | Protects | Cipher |
| --- | --- | --- |
| BIP | group-addressed robust management | AES-CMAC |
| CCMP | individual robust management | AES-CCM |

## Keys

| Key | ID | Size | Slot |
| --- | --- | --- | --- |
| IGTK | `4` | 16 bytes | `0` |
| IGTK | `5` | 16 bytes | `1` |
| PTK (TK) | n/a | 16 bytes | n/a |

IGTK install rules:

- Accept only IDs `4` and `5`.
- Key must be exactly 16 bytes.
- Optional sequence is 6 bytes, little-endian IPN.
- Same key bytes: keep the higher IPN, ignore a lower one.
- Different key bytes: replace and set IPN to the supplied value.
- Reject any other ID, length or sequence length before touching state.
- Serialize install, verify and delete.

PTK install rules:

- 16 bytes, CCMP only.
- A first or changed key sets the software TX PN to `1`.
- A same-key reinstall leaves the TX PN and the RX floor unchanged.
- Install brackets the epoch: increment before publishing the key, increment again after, so a
  stable key always has an even epoch.
- Delete clears the key and replay floor before freeing the transform.

## BIP

### MMIE layout

The MMIE is the last 18 bytes of the frame.

| Offset | Size | Field |
| --- | --- | --- |
| `0` | 1 | element ID `76` |
| `1` | 1 | length `16` |
| `2` | 2 | key ID, little-endian |
| `4` | 6 | IPN, little-endian |
| `10` | 8 | MIC |

There is no reserved field. The total is 18 bytes: 10 bytes of header and IPN, then the MIC.

### BIP input

```text
AAD  = FC[0] || (FC[1] & ~0x38) || A1 || A2 || A3     (20 bytes)
data = frame[24 .. len-8]                               (body and MMIE header + IPN)
mic  = 8 zero bytes                                     (MMIE MIC field, zeroed)
MIC  = AES-CMAC(IGTK, AAD || data || mic)[0..8]
```

| Field | Bytes |
| --- | --- |
| AAD | 20 |
| Body | `len - 24 - 8` |
| MMIE MIC field, zeroed | 8 |
| MIC | 8 |

The final eight zero bytes are the MMIE MIC field, which the BIP construction treats as zero for
authentication. They are not CMAC padding and do not extend the length. AES-CMAC applies its own
block padding internally.

Frame acceptance before crypto:

| Check | Reject when |
| --- | --- |
| Length | `< 44` or `> 4096` |
| Destination | address 1 is not group-addressed (`f[4] & 1` clear) |
| Frame control | any of `f[1] & 0xc7` set (toDS, fromDS, more fragments, protected, order) |
| Fragment | `f[22] & 0x0f` nonzero |
| Subtype | not deauth (`0xc0`), disassoc (`0xa0`) or action (`0xd0`) |
| MMIE | element ID not `76` or length not `16` |
| Key ID | not `4` or `5` |

Then:

1. Read IPN.
2. Require the key to be valid.
3. Compare IPN against the floor; reject `0`, above 48 bits, or not greater than the floor.
4. Compute the MIC over AAD, body and the zeroed MMIE MIC field.
5. Compare 8 bytes in constant time.
6. Advance the IPN only after the MIC succeeds.

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
| `20` | 2 | sequence control, fragment bits only (`& 0x000f`) |

The frame-control value is built as `(fc & ~0x3800) | 0x4000`. That clears retry, power management
and more-data, and sets the Protected bit.

### CCMP header and PN

An 8-byte CCMP header precedes the encrypted body.

| Offset | Size | Field |
| --- | --- | --- |
| `0` | 1 | PN0 |
| `1` | 1 | PN1 |
| `2` | 1 | reserved, `0` |
| `3` | 1 | ExtIV and key ID: `0x20 | (key_id << 6)` |
| `4` | 1 | PN2 |
| `5` | 1 | PN3 |
| `6` | 1 | PN4 |
| `7` | 1 | PN5 |

Key ID `0` therefore writes `0x20` at byte `3`. Byte `2` is reserved and stays zero.
PN is 48 bits, assembled from header bytes `0,1,4,5,6,7`:

```text
PN = h[0] | h[1]<<8 | h[4]<<16 | h[5]<<24 | h[6]<<32 | h[7]<<40
```

- MIC is 8 bytes, appended after the ciphertext.
- AES-CCM with a 13-byte nonce, 22-byte AAD and 8-byte MIC.

Replay:

- Reject PN `0`.
- Reject PN above `0xffffffffffff`.
- Reject PN not greater than the floor.
- Advance the floor after MIC success, before dispatch.

TX:

- Fetch and increment a 64-bit counter under a spinlock.
- Reject `0` or a value above 48 bits.
- Write PN little-endian into the CCMP header.
- Initial value after a successful association is `0x1000000`.
- A first or changed PTK resets it to `1`.
- A same-key reinstall does not reset it.

## Constant time

- Compare MICs and tags with a constant-time primitive.
- Never branch on key bytes, MIC bytes or secret lengths.
- Equality checks on public values, such as the artifact hash, need not be constant time.
- Never log key material, PN-derived secrets or plaintext bodies.
- Zeroize key buffers on delete and detach.

## Key lifetime

| Event | IGTK | PTK |
| --- | --- | --- |
| Association | install per AP | install after 4-way |
| Rekey | replace on new key bytes | replace, reset software TX PN and RX floor |
| Same-key reinstall | keep the higher IPN | leave TX PN and RX floor |
| Disconnect | clear | clear |
| Reset | clear | clear |
| Detach | clear and free transforms | clear and free transforms |

Rules:

- A key is unusable until its valid flag is set.
- Clear the valid flag before freeing a transform.
- A frame must not verify against a replaced key.
- Every verify checks the epoch captured before decryption against the current even epoch.
- A stale frame must not verify against a replaced key or a deleted key.

## SA Query

Purpose: confirm the link when the AP claims we are not associated, instead of obeying a spoofed
teardown.

Trigger:

- Unprotected deauth or disassoc with reason `6` or `7`.
- Link probe after a stalled-link window, when enabled.

Frame:

| Field | Value |
| --- | --- |
| Category | `8` (SA Query) |
| Action | `0` request, `1` response |
| TID | transaction identifier, 2 bytes |

Procedure:

1. Send a request with a fresh TID.
2. After `300 ms` without a matching response, send the next request.
3. At most three requests are sent in total, including the first.
4. Any matching response clears the transaction.
5. If the third request also times out, treat the link as lost and disconnect.

Rules:

- Match by category, action and TID.
- A response for a different TID does not clear the transaction.
- Only one transaction in flight.
- The responder runs in process context; the RX path only queues.
- Do not obey the triggering teardown while the query is unresolved.
- If the link is genuinely gone, the query fails and the disconnect proceeds.

## Authenticated teardown

| Received frame | Protected | Action |
| --- | --- | --- |
| Deauth | valid MIC | apply reason, disconnect |
| Disassoc | valid MIC | apply reason, disconnect |
| Deauth | unprotected | drop; reason 6 or 7 starts SA Query |
| Disassoc | unprotected | drop; reason 6 or 7 starts SA Query |
| Deauth | bad MIC | drop |
| Deauth | unprotected, other reason | drop |

Never apply an unprotected teardown as authoritative. A protected teardown is authoritative only
after MIC and replay checks.

Reason codes `6` and `7` indicate a lost association and warrant SA Query instead of an immediate
disconnect.

## Block Ack under PMF

- Block Ack action frames are robust management frames.
- They must be CCMP-protected when PMF is active.
- Verify before handling or re-injecting.
- Never pass an unverified Block Ack frame to the device.
- On TX, protect the device's own ADDBA/DELBA frames before they are queued.

## Group-addressed robust management

- BIP frames normally have no Protected bit.
- Do not require the Protected bit for group-addressed robust frames.
- Verify BIP first; dispatch only on success.
- A group frame with a bad MIC is dropped, not forwarded.

## Protocol versus local policy

| Rule | Kind | Note |
| --- | --- | --- |
| IGTK IDs `4`/`5` | protocol | fixed by 802.11 |
| MMIE ID `76`, length `16`, total `18` | protocol | fixed |
| BIP MIC `8` | protocol | fixed |
| CCMP MIC `8` | protocol | fixed |
| Nonce and AAD layout | protocol | fixed |
| PN/IPN 48-bit | protocol | fixed |
| Reject PN `0` | local policy | conservative |
| Initial TX PN `0x1000000` | local policy | keeps management PN above short data sessions |
| SA Query: three sends, `300 ms` apart | local policy | bounded and tunable |
| Drop unprotected teardown | protocol plus policy | 802.11w defense |
| Only one SA Query in flight | local policy | simplification |

Local policy may change values. Protocol layout may not.

## Invariants

1. Management protection is host software.
2. MIC verification precedes every side effect.
3. Replay advances only after MIC success.
4. MIC comparison is constant time.
5. A replaced or deleted key invalidates the old floor.
6. No unprotected teardown is authoritative.
7. Group robust frames use BIP and need no Protected bit.
8. Keys are zeroized on delete and detach.
