# 02. Frozen binary ABI

Compatibility seams for the one artifact this mode supports.

## Artifact identity

| Field | Value |
| --- | --- |
| Version | `6.30.223.271` |
| Build | `r587334` |
| Format | ELF64 relocatable, x86-64 |
| Symbols | not stripped |
| SHA-256 | `352a6e349f74c69b78e76f68c63752c99b8f6b22dc942af531b754211d7f4743` |

The ABI below applies only to this hash. A different build, even with the same version string, may have different offsets and must be rejected.

Gate:

1. Read image bytes.
2. Compute SHA-256.
3. Compare in constant time.
4. Refuse to load on mismatch; report expected and actual.

Do not patch an image in memory that failed the gate.

## What the ABI is

- SysV AMD64 calling convention.
- Integer args in `RDI, RSI, RDX, RCX, R8, R9`, then stack.
- Return in `RAX`; negative values are errors unless a function documents otherwise.
- Struct fields are reached by byte offset from opaque pointers.
- Offsets are compatibility seams, not Rust layouts. Never model them as `#[repr(C)]`.

## Opaque pointer roles

| Symbol | Role |
| --- | --- |
| `wlc` | adapter instance |
| `bsscfg` | per-BSS configuration |
| `scb` | station control block; for a station link this is the AP |
| `assoc` | association context owned by `bsscfg` |
| `txq` | transmit queue object |
| `pkt` | packet/skb wrapper owned by the blob |

## Offset seams

| Address | Meaning | Confidence |
| --- | --- | --- |
| `wlc + 0x6a8` | default TX queue pointer | confirmed in call sites |
| `wlc + 0x6bc` | RX frame offset (`u32`) from `skb->data` | confirmed in call sites |
| `scb + 0x18` | BSS pointer for this station | confirmed in call sites |
| `bsscfg + 0x320` | association context pointer | confirmed in call sites |
| `assoc + 0x0c` | auth/assoc state (`int`) | confirmed |
| `assoc + 0x30` | association IE buffer pointer | confirmed |
| `assoc + 0x38` | association IE length (`u32`) | confirmed |
| `bsscfg + 0x94` | auth algorithm (`u16`) | confirmed |

Observed `assoc + 0x0c` values: `7` and `9` during the auth exchange; `10` after a successful auth advance. Treat the numbers as artifact constants, not an enum for other builds.

## Auth TX ABI

`wlc_sendauth` sends an authentication frame.

```c
long wlc_sendauth(void *bsscfg, void *da, void *bssid, void *scb,
                  int alg, int seq, int status,
                  void *key, void *ie, unsigned char flags);
```

| # | Register | Inferred role | Confidence |
| --- | --- | --- | --- |
| 1 | `RDI` | `bsscfg` | high |
| 2 | `RSI` | destination address (AP) | high |
| 3 | `RDX` | BSSID | high |
| 4 | `RCX` | `scb` (AP) | high |
| 5 | `R8D` | auth algorithm | high |
| 6 | `R9D` | auth sequence | high |
| 7 | `[rsp+0x8]` | auth status | medium |
| 8 | `[rsp+0x10]` | key / challenge IV | medium |
| 9 | `[rsp+0x18]` | IE buffer | medium |
| 10 | `[rsp+0x20]` | flags (byte) | low |

Body construction observed:

- six bytes little-endian `alg`, `seq`, `status`,
- optional IE appended after the body,
- shared-key challenge uses a key IV and protected frame control `0x40b0`,
- SAE commit/confirm bodies are supplied by userspace, not built here.

The function owns the frame after return. Do not free the passed buffers.

## Auth RX ABI

`wlc_authresp_client` consumes a received authentication body.

```c
void wlc_authresp_client(void *bsscfg, long ctx,
                         unsigned short *body, unsigned int len,
                         unsigned char p5);
```

| # | Register | Inferred role | Confidence |
| --- | --- | --- | --- |
| 1 | `RDI` | `bsscfg` | high |
| 2 | `RSI` | frame/address context | low |
| 3 | `RDX` | body, `u16[]` | high |
| 4 | `ECX` | body length in bytes | high |
| 5 | `R8B` | context byte | low |

`body[0]` = algorithm, `body[1]` = sequence, `body[2]` = status. `len` is bytes, not elements. Guard `len >= 6` before reading status.

## Auth advancer

```c
void re_auth_advance(void *bsscfg, int code,
                     const unsigned char *mac, int p4, int p5);
```

| # | Register | Role |
| --- | --- | --- |
| 1 | `RDI` | `bsscfg` |
| 2 | `ESI` | result code; `0` = auth succeeded |
| 3 | `RDX` | AP MAC |
| 4 | `ECX` | unused, pass `0` |
| 5 | `R8D` | unused, pass `0` |

Location: `.text + 0x5b79d`.

Effect on `code == 0`: marks the station authenticated, advances association state, sends the association request. The AP `scb` must already exist; a null `scb` aborts or crashes. Call only under the driver lock, after the association IEs are installed.

## Management TX helpers

```c
void *wlc_frame_get_mgmt_ex(void *wlc, unsigned short fc,
                            const void *a1, const void *a2, const void *a3,
                            int body_len, void **body, int p8, int p9);

void wlc_queue_80211_frag(void *wlc, void *pkt, void *txq,
                          void *scb, void *bss, int p6,
                          void *p7, int p8);
```

`a1`, `a2`, `a3` are the three MAC header addresses in frame order. `body` receives a writable buffer of `body_len` bytes. Fill it, then queue with the real `scb` and `bss = *(scb + 0x18)`.

Queuing with a null `scb` makes the blob substitute a default station in the packet tag; TX completion then dereferences it. Always pass the AP `scb`.

## Lookup helpers

```c
void *wlc_bsscfg_find_by_bssid(void *wlc, const unsigned char *mac);
void *wlc_scbfind(void *wlc, void *bsscfg, const unsigned char *mac);
```

Both return borrowed pointers. Do not free. Revalidate after any disconnect or reset.

## Packet ownership

| Path | Ownership |
| --- | --- |
| RX consumed by host | host frees with `wl_re_pktfree(dev, pkt, false)` |
| RX passed to `wlc_recv` | ownership transfers to blob |
| TX queued via `wlc_queue_80211_frag` | ownership transfers to blob |
| TX dropped before queue | free with `send = true` |
| `wlc_recv`, `wlc_queue_80211_frag` | take ownership; never free again |

Rules:

- Exactly one free per packet.
- Never free after a successful queue.
- Never hand the same packet to two consumers.
- Free outside any lock that the free path can re-enter.

## Locking

- Blob entry points assume the driver lock is held unless documented otherwise.
- `re_auth_advance`, TX queueing, RX handoff and key changes are serialized by the driver lock.
- Do not hold the driver lock across `kmalloc(GFP_KERNEL)`, `copy_from_user`, or any wait.
- Auth wrappers may capture pointers during the call; they must not retain them past disconnect without revalidation.

## Patch semantics

Two length-preserving edits make the bridge possible.

| Edit | Location | Semantics |
| --- | --- | --- |
| Auth clamp passthrough | file offset `0x47d8d` in `.text` | store requested auth algorithm verbatim into `bsscfg + 0x94` instead of clamping nonzero to `1` |
| Relocation redirect | `.rela.text` entries | point `wlc_sendauth`, `wlc_authresp_client`, `wlc_recv`, `wlc_sendmgmt` calls at host wrappers |

`.text` virtual address `V` maps to file offset `V + 0x40`.

Both edits must validate the original bytes before writing. A mismatch aborts; never write on a stale offset. The redirect is layout-safe: symbols are appended, section offsets updated, no code moved.

Do not present these edits as source. They are seams.

## Error rules

| Condition | Result |
| --- | --- |
| Null `wlc` / `bsscfg` / `scb` | `-EINVAL`, no call |
| Hash mismatch | refuse load |
| Frame shorter than 24 bytes | `-EINVAL` |
| Allocation failure | `-ENOMEM` |
| Missing AP `scb` | abort advance, report link failure |
| Association IE buffer not owned | refuse injection |

## Applicability and hazards

- One hash, one build, one architecture.
- Offsets are not stable across builds, configs or compilers.
- Any Rust struct that mirrors these offsets is wrong by construction; use raw byte access behind a checked accessor.
- Any pointer captured from the blob is invalid after detach, reset or disconnect.
- Any call made without the driver lock is a data race, even if it appears read-only.
- The bridge does not make the blob correct; it only exposes seams.

## Minimal safe boundary

```text
Rust state machine
  -> checked seam accessors (offset + type + bound)
  -> extern "C" blob calls
  -> blob-owned objects (opaque)
```

The Rust side never owns blob objects. It owns host state, keys, epochs and policy. The boundary is `unsafe` and small.
