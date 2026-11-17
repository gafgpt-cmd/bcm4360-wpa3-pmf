# 03. RX/TX framing

Byte layout, validation and ownership on the 802.11 path.

## RX entry contract

At the per-frame entry point:

- `skb->data` points at the D11 receive header, not at the 802.11 frame.
- `skb->len` is the total buffer length.
- The frame starts at:

```text
frame_off = host_u32_at(wlc + 0x6bc) + 6 /* PHY bytes */
frame_off += (rx[0x10] & 0x04) ? 2 : 0   /* optional pad */
```

| Component | Size | Meaning |
| --- | --- | --- |
| D11 header prefix | `frame_off - 6` | device metadata |
| PHY bytes | `6` | always skipped |
| pad | `0` or `2` | present when `rx[0x10]` bit 2 is set |
| 802.11 frame | variable | starts at `frame_off` |
| FCS | `4` | trailing, stripped before parsing |

The offset field is a host `u32` inside the object, not a little-endian wire value. Read it with
`memcpy`.

## Validation pipeline

Apply in order. Any failure drops the packet.

| # | Check | Reject when |
| --- | --- | --- |
| 1 | Pointer | `skb == NULL` |
| 2 | Linearity | `skb_headlen(skb) != skb->len` |
| 3 | Minimum | `skb->len < 20` |
| 4 | Offset bound | `frame_off > 4096` or `frame_off > skb->len` |
| 5 | Room | `skb->len - frame_off < 6 + pad + 4` |
| 6 | Version | `skb->data[frame_off] & 0x03 != 0` |

The minimum-24-byte frame check applies after the offset is located and FCS is removed, in the
per-family handlers, not to every packet at the tap.

Rules:

- Do not linearize in the hot path. Drop non-linear frames and count the event.
- Do not trust `frame_off` before the bounds checks.
- Do not read past `skb->len`.
- All 802.11 multi-byte fields are little-endian.
- The frame may be unaligned; use `memcpy`, not a cast.

## Header fields

Non-QoS three-address management header, 24 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| `0` | 2 | frame control |
| `2` | 2 | duration |
| `4` | 6 | address 1 (DA / RA) |
| `10` | 6 | address 2 (SA / TA) |
| `16` | 6 | address 3 (BSSID) |
| `22` | 2 | sequence control |

Frame control, little-endian `u16` view:

| Mask | Meaning | Use |
| --- | --- | --- |
| `0x0003` | protocol version | must be `0` |
| `0x000c` | type | `0` management |
| `0x00f0` | subtype | `0xb0` auth, `0xa0` disassoc, `0xc0` deauth, `0xd0` action |
| `0x4000` | protected frame | set on CCMP-protected unicast management |
| `0x8000` | order | not expected on management |

Byte view of the same flags:

| Byte and mask | Meaning |
| --- | --- |
| `f[0] & 0x03` | protocol version |
| `f[0] & 0x0c` | type |
| `f[0] & 0xf0` | subtype |
| `f[1] & 0x40` | protected frame |
| `f[1] & 0x80` | order |

Never compare a byte against a little-endian 16-bit mask. `0x4000` is `f[1] & 0x40`, not
`f[0] & 0x40`.

Authentication body at `+24`:

| Offset | Size | Field |
| --- | --- | --- |
| `+24` | 2 | algorithm |
| `+26` | 2 | transaction sequence |
| `+28` | 2 | status |
| `+30` | variable | challenge / SAE / FILS element |

## FCS handling

- The trailing 4 bytes are the FCS.
- Strip exactly once, after locating the frame and before parsing body lengths.
- Never include FCS in MIC or AAD input.
- Never hand a frame with FCS to userspace or to the object.

## RX decision matrix

The tap runs when SAE is armed or PMF is active.

| Frame class | Condition | Action |
| --- | --- | --- |
| Authentication, alg 3 (SAE) | before external-auth START | consume and free; do not notify userspace; never pass to the object |
| Authentication, alg 3 (SAE) | after START | forward to userspace, consume, free; never pass to the object |
| Authentication, other alg | any | pass to the object |
| Group robust management | DA group, SA = AP, BSSID = AP | verify BIP; on failure drop; on success apply teardown or drop after handling |
| Individual robust management | Protected bit set, DA = us, SA = AP, BSSID = AP | CCMP verify and replay; on failure drop |
| Protected action after verify | valid | clear the Protected bit, then either re-inject Block Ack or pass |
| Robust action, unprotected, to us | no Protected bit | drop |
| Deauth / disassoc, unprotected, from AP | any reason | drop; reason 6 or 7 schedules SA Query |
| Data | any | pass through; the object owns it |

Order for protected individual frames:

1. Bounds and FCS.
2. Address match: DA = us, SA = AP, BSSID = AP.
3. CCMP decrypt and MIC verify.
4. Epoch check, then replay advance.
5. Only then side effects or dispatch.

Never dispatch before MIC verification. Never advance replay before MIC success.

### Protected Block Ack mutation

Block Ack is a robust action frame. Under PMF the AP protects its ADDBA/DELBA frames, and the
object cannot decrypt management frames.

RX:

1. Verify CCMP and replay.
2. Clear the Protected bit in the frame-control byte so the object's parser accepts the now-plaintext
   action frame.
3. Trim to the plaintext length.
4. Re-inject the frame to the object for Block Ack handling.

TX:

1. Locate the object's unprotected Block Ack action frame.
2. Reserve 16 bytes of tailroom (8-byte CCMP header + 8-byte MIC).
3. Encrypt the body with the current PTK and a fresh PN.
4. Set the Protected bit and write the CCMP header and MIC.
5. Extend the skb and return it to the object.

This mutation is the only place where a frame is rewritten in place. It is bounded, atomic with
respect to the TX path, and never applied to a frame that failed verification.

## TX contract

Management transmit takes a complete 802.11 frame, header and body.

```text
precondition: flen >= 24
body_len = flen - 24
```

Steps:

1. Reject `flen < 24` or a null frame.
2. Build with `wlc_frame_get_mgmt_ex` using frame control, address 1, address 2, address 3 and
   `body_len`.
3. Copy the body into the returned buffer.
4. Read `txq` from `wlc + 0x6a8`.
5. Derive `bss` from the AP `scb`: `*(scb + 0x18)`.
6. Queue with `wlc_queue_80211_frag(wlc, pkt, txq, scb, bss, 0, NULL, 0)`.
7. Treat the queue result as acceptance, not transmission. Never free after the call.

Do not build the MAC header by hand when the helper exists. Do not queue with a null `scb`.

## Headroom and tailroom

| Buffer | Requirement |
| --- | --- |
| Object-built management frame | allocation and headroom owned by the helper; do not adjust |
| Host RX skb | read-only at the tap; never pull or push except the verified Block Ack rewrite |
| Host TX frame | helper allocates; body copy bounded by `body_len` |
| Protected TX | CCMP header and MIC space reserved before encryption |

Never write into an RX skb that has not passed MIC verification.

## Encryption ordering

TX:

1. Build plaintext frame.
2. If protected, reserve CCMP header (8 bytes) and MIC (8 bytes).
3. Encrypt body, insert PN, append MIC.
4. Queue.

RX:

1. Locate frame and remove FCS.
2. If protected, read PN and CCMP header.
3. Verify and decrypt.
4. Epoch check, then replay advance.
5. Dispatch.

BIP group frames have no Protected bit and no CCMP header. Verify BIP, then dispatch.

## Ownership summary

| Event | Owner after event |
| --- | --- |
| Host consumes RX | host, free once |
| Host passes RX to object | object |
| Host queues TX | object |
| Host never queues TX | host, free with the drop flag |
| Duplicate delivery | forbidden |

Free is not allowed under a lock that the free path can re-enter. Exactly one free per packet.

## Counters

Track at least:

- RX: total, non-linear, short, oversize, bad offset, bad version, bad MIC, replay, forwarded,
  consumed.
- TX: built, queued, build failure, never queued, dropped.

Counters are diagnostic. They must not change behavior.

## Invariants

1. `skb->data` is not the 802.11 frame at entry.
2. The offset is a host field, then is validated.
3. FCS is stripped exactly once.
4. MIC verification precedes side effects.
5. Replay advances only after MIC success.
6. Management protection is host software.
7. One free per packet, one owner at a time.
