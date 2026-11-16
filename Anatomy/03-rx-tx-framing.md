# 03. RX/TX framing

Byte layout, validation, and packet ownership for the 802.11 path.

## RX entry contract

At the receive entry point:

- `skb->data` points at the d11 receive header, not at the 802.11 frame.
- `skb->len` is the total buffer length.
- The 802.11 frame starts at:

```text
frame_off = *(u32 *)(wlc + 0x6bc) + 6
frame_off += (rx_header[0x10] & 0x04) ? 2 : 0
```

| Component | Size | Meaning |
| --- | --- | --- |
| d11 header prefix | `frame_off - 6` | device metadata |
| PHY bytes | `6` | always skipped |
| pad | `0` or `2` | present when `rx[0x10]` bit 2 is set |
| 802.11 frame | variable | starts at `frame_off` |
| FCS | `4` | trailing, stripped before parsing |

`frame_off` is little-endian `u32` at `wlc + 0x6bc`.

## Validation pipeline

Apply in order. Any failure drops the packet.

| # | Check | Reject when |
| --- | --- | --- |
| 1 | Pointer | `skb == NULL` |
| 2 | Linearity | `skb_headlen(skb) != skb->len` |
| 3 | Minimum | `skb->len < 20` |
| 4 | Maximum | `skb->len > 4096` |
| 5 | Offset sane | `frame_off > 4096` or `frame_off > skb->len` |
| 6 | Room | `skb->len - frame_off < 2 + 4` |
| 7 | Version | `skb->data[frame_off] & 0x03 != 0` |
| 8 | Frame minimum | remaining bytes `< 24` after FCS removal |
| 9 | Header form | unexpected control/extension form for the path |

Rules:

- Do not linearize in the hot path. If a frame is non-linear, drop it and count the event.
- Do not trust `frame_off` before bounds checks.
- Do not read past `skb->len`.
- All multi-byte protocol fields are little-endian.
- The frame may be unaligned; use `memcpy`, not a cast.

## 802.11 header fields

Non-QoS three-address management header, 24 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| `0` | 2 | frame control |
| `2` | 2 | duration |
| `4` | 6 | address 1 (DA / RA) |
| `10` | 6 | address 2 (SA / TA) |
| `16` | 6 | address 3 (BSSID) |
| `22` | 2 | sequence control |

Frame control bits used:

| Bit | Meaning | Use |
| --- | --- | --- |
| `0x0003` | protocol version | must be `0` |
| `0x0040` | protected frame | CCMP-protected management |
| `0x0080` | order | not expected on management |
| `0x0300` | subtype high bits | `0xb0` auth, `0xc0` deauth, `0xa0` disassoc, `0xd0` action |

Authentication body at `+24`:

| Offset | Size | Field |
| --- | --- | --- |
| `+24` | 2 | algorithm |
| `+26` | 2 | transaction sequence |
| `+28` | 2 | status |
| `+30` | variable | challenge / SAE / FILS element |

## FCS handling

- The trailing 4 bytes are the FCS.
- Strip exactly once, after locating the frame, before parsing body lengths.
- Never include FCS in MIC/AAD input.
- Never hand a frame with FCS to userspace or to the blob.

## RX decision matrix

| Frame class | Protected bit | Action |
| --- | --- | --- |
| Authentication, alg 3 (SAE) | either | forward to userspace after START; consume; never pass to blob |
| Authentication, other alg | either | pass to blob unless host policy says otherwise |
| Deauth / disassoc | `0` | drop, do not obey; if reason 6/7, start SA Query |
| Deauth / disassoc | `1` | verify CCMP, then apply reason |
| Action, SA Query | `1` | verify CCMP; respond |
| Action, Block Ack | `1` | verify CCMP; handle or re-inject |
| Robust management, group addressed | `0` | verify BIP, then dispatch |
| Robust management, individual, unprotected | `0` | drop |
| Data | any | pass through |

"Robust" means the management subtype is protected under PMF. Use the kernel helper, not a hand-written subtype list.

Order of checks for protected robust frames:

1. Bounds.
2. Address match: DA = us, BSSID = AP, SA = AP where required.
3. MIC verify and replay advance.
4. Only then side effects or dispatch.

Never dispatch before MIC verify. Never advance replay after dispatch.

## TX contract

Management transmit takes a complete 802.11 frame, header and body.

```text
precondition: flen >= 24
body_len = flen - 24
```

Steps:

1. Reject `flen < 24` or null frame.
2. Build a management frame with `wlc_frame_get_mgmt_ex` using frame control, address 1, address 2, address 3 and `body_len`.
3. Copy the body into the returned buffer.
4. Read `txq` from `wlc + 0x6a8`.
5. Derive `bss` from the AP `scb`: `*(scb + 0x18)`.
6. Queue with `wlc_queue_80211_frag(wlc, pkt, txq, scb, bss, 0, NULL, 0)`.
7. On build or allocation failure, free the packet with `send = true`.

Do not construct the MAC header by hand when the helper exists. Do not queue with a null `scb`.

## Headroom and tailroom

| Buffer | Requirement |
| --- | --- |
| Blob-built management frame | allocation and headroom owned by the helper; do not adjust |
| Host RX skb | read-only in the tap; never pull/push |
| Host TX frame | helper allocates; body copy bounded by `body_len` |
| Protected TX | CCMP header and MIC space must be reserved before encryption |

Never assume the frame is writable. Never write into an RX skb.

## Encryption ordering

TX:

1. Build plaintext frame.
2. If protected, reserve CCMP header (8 bytes) and MIC (8 bytes).
3. Encrypt body, insert PN, append MIC.
4. Queue.

RX:

1. Locate frame and strip FCS.
2. If protected, read PN and CCMP header.
3. Verify and decrypt.
4. Replay-check.
5. Dispatch.

BIP group frames have no Protected bit and no CCMP header. Verify BIP, then dispatch.

## Ownership summary

| Event | Owner after event |
| --- | --- |
| Host consumes RX | host, free once |
| Host passes RX to blob | blob |
| Host queues TX | blob |
| Host drops TX before queue | host, free with `send = true` |
| Duplicate delivery | forbidden |

Free is not allowed under a lock that the free path can re-enter. Exactly one free per packet.

## Counters

Track at least:

- RX: total, dropped non-linear, dropped short, dropped oversize, dropped bad offset, dropped bad version, dropped MIC, dropped replay, forwarded, consumed.
- TX: built, queued, build failure, queue failure, dropped.

Counters are diagnostic. They must not change behavior.

## Invariants

1. `skb->data` is not the 802.11 frame at entry.
2. Offset comes from the device, then is validated.
3. FCS is stripped exactly once.
4. MIC verify precedes side effects.
5. Replay advances before dispatch.
6. Management protection is host software.
7. One free per packet, one owner at a time.
