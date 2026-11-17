# 02. Frozen binary ABI

Compatibility seams for one artifact. Not a stable interface, not a structure layout.

## Artifact identity

| Field | Value |
| --- | --- |
| Version | `6.30.223.271` |
| Build | `r587334` |
| Format | ELF64 relocatable, x86-64 |
| Symbols | not stripped |
| SHA-256 | `352a6e349f74c69b78e76f68c63752c99b8f6b22dc942af531b754211d7f4743` |

The ABI applies only to this hash. A different build, even with the same version string, may have
different offsets and must be rejected.

Gate:

1. Read image bytes.
2. Compute SHA-256.
3. Compare in constant time.
4. Refuse to load on mismatch; report expected and actual.
5. Never patch or link an image that failed the gate.

## ABI form

- SysV AMD64 calling convention.
- Integer arguments in `RDI, RSI, RDX, RCX, R8, R9`, then stack.
- Return in `RAX`; negative values are errors unless a function documents otherwise.
- Structure fields are reached by byte offset from opaque pointers.
- Offsets are compatibility seams. Never model them as `#[repr(C)]`.

## Opaque pointer roles

| Symbol | Role |
| --- | --- |
| `wlc` | adapter instance |
| `bsscfg` | per-BSS configuration |
| `scb` | station control block; for a station link this is the AP |
| `assoc` | association context owned by `bsscfg` |
| `txq` | transmit queue object |
| `pkt` | packet/skb wrapper owned by the object |

Pointers are borrowed pointers into object state. Non-null plus a known offset proves only that the
access will not fault at that instant. It does not prove allocation bounds, object liveness, or
absence of a concurrent teardown.

## Offset seams

| Address | Meaning | Type |
| --- | --- | --- |
| `wlc + 0x6a8` | default TX queue pointer | pointer |
| `wlc + 0x6bc` | RX frame offset from `skb->data` | host `u32` |
| `scb + 0x18` | BSS pointer for this station | pointer |
| `bsscfg + 0x320` | association context pointer | pointer |
| `assoc + 0x0c` | auth/assoc state | `int` |
| `assoc + 0x30` | association IE buffer pointer | pointer |
| `assoc + 0x38` | association IE length | `u32` |
| `bsscfg + 0x94` | auth algorithm | `u16` |

`assoc + 0x0c` is observed as `7` and `9` during the auth exchange and becomes `10` after a
successful advance. Treat those numbers as artifact constants, not as a portable enum.

`wlc + 0x6bc` is a host object field written by the object, not a little-endian value read from the
device. Read it with `memcpy` into a host `u32`.

## Auth TX

```c
long wlc_sendauth(void *bsscfg, void *da, void *bssid, void *scb,
                  int alg, int seq, int status,
                  void *key, void *ie, unsigned char flags);
```

| # | Register | Role |
| --- | --- | --- |
| 1 | `RDI` | `bsscfg` |
| 2 | `RSI` | destination (AP) |
| 3 | `RDX` | BSSID |
| 4 | `RCX` | `scb` (AP) |
| 5 | `R8D` | auth algorithm |
| 6 | `R9D` | auth sequence |
| 7 | `[rsp+0x8]` | auth status |
| 8 | `[rsp+0x10]` | key / challenge IV |
| 9 | `[rsp+0x18]` | IE buffer |
| 10 | `[rsp+0x20]` | flags (byte) |

Body construction:

- six bytes little-endian `alg`, `seq`, `status`,
- optional IE appended after the body,
- shared-key challenge uses a key IV and protected frame control `0x40b0`,
- SAE commit and confirm bodies are supplied by userspace, not built here.

The callee owns the frame after return. Do not free the passed buffers.

## Auth RX

```c
void wlc_authresp_client(void *bsscfg, long ctx,
                         unsigned short *body, unsigned int len,
                         unsigned char flag);
```

| # | Register | Role |
| --- | --- | --- |
| 1 | `RDI` | `bsscfg` |
| 2 | `RSI` | frame/address context |
| 3 | `RDX` | body, `u16[]` |
| 4 | `ECX` | body length in bytes |
| 5 | `R8B` | context byte |

`body[0]` = algorithm, `body[1]` = sequence, `body[2]` = status. `len` is bytes, not elements.
Guard `len >= 6` before reading status.

## Auth advancer

```c
void re_auth_advance(void *bsscfg, int code,
                     const unsigned char *mac, int unused1, int unused2);
```

Location: `.text + 0x5b79d`, minted as a symbol during the build.

On `code == 0` it marks the station authenticated, advances association state and sends the
association request. The AP `scb` must already exist. Call only under the driver lock, after the
association IEs are installed.

## Management TX

```c
void *wlc_frame_get_mgmt_ex(void *wlc, unsigned short fc,
                            const void *a1, const void *a2, const void *a3,
                            int body_len, void **body, unsigned int hdr_pad,
                            int tail);

int wlc_queue_80211_frag(void *wlc, void *pkt, void *txq,
                         void *scb, void *bss, char flag,
                         void *key, unsigned int zero);
```

`a1`, `a2`, `a3` are the three MAC header addresses in frame order. `body` receives a writable
buffer of `body_len` bytes. Fill it, then queue with the real `scb` and `bss = *(scb + 0x18)`.

Return handling:

- `wlc_queue_80211_frag` returns an `int` boolean result.
- The host wrapper ignores that result and returns success when a packet and body were produced.
- A successful call proves only that the frame was handed to the queue. It does not prove
  transmission or TX completion.
- Because the result is ignored, the host cannot tell whether the object accepted or rejected the
  packet. Do not free after the call. Treat the call as the ownership transfer point; if the object
  can reject without consuming, the wrapper must be fixed before relying on it.

Queuing with a null `scb` makes the object substitute a default station in the packet tag; the TX
completion path then dereferences it. Always pass the AP `scb`.

## Lookup helpers

```c
void *wlc_bsscfg_find_by_bssid(void *wlc, const unsigned char *mac);
void *wlc_scbfind(void *wlc, void *bsscfg, const unsigned char *mac);
```

Both return borrowed pointers. Do not free. Revalidate after any disconnect, reset or detach.

## Packet ownership

| Path | Ownership |
| --- | --- |
| RX consumed by host | host frees once |
| RX passed to `wlc_recv` | object |
| TX queued via `wlc_queue_80211_frag` | object |
| TX never queued | host frees with the drop flag |
| `wlc_recv`, `wlc_queue_80211_frag` | take ownership; never free again |

Rules:

- Exactly one free per packet.
- Never free after a successful queue.
- Never hand the same packet to two consumers.
- Free outside any lock that the free path can re-enter.

## Locking and lifetime

- Object entry points assume the driver lock is held unless documented otherwise.
- `re_auth_advance`, TX queueing, RX handoff and key changes are serialized by the driver lock.
- Do not hold the driver lock across `kmalloc(GFP_KERNEL)`, `copy_from_user` or any wait.
- Captured pointers are valid only for the current association and only while the object is attached.
- Clear every captured pointer on disconnect, reset and detach. A detach race is a use-after-free,
  not a recoverable error.

## Patch semantics

Two length-preserving edits make the bridge possible.

| Edit | Location | Semantics |
| --- | --- | --- |
| Auth clamp passthrough | file offset `0x47d8d` in `.text` | store the requested auth algorithm verbatim into `bsscfg + 0x94` instead of clamping nonzero to `1` |
| Relocation redirect | `.rela.text` entries | point `wlc_sendauth`, `wlc_authresp_client`, `wlc_recv`, `wlc_sendmgmt` calls at host wrappers |

`.text` virtual address `V` maps to file offset `V + 0x40`.

Both edits validate the original bytes before writing. A mismatch aborts; never write on a stale
offset. The redirect is layout-safe: symbols are appended, section offsets updated, no code moved.

These edits are seams, not source.

## Boundary rules

| Condition | Result |
| --- | --- |
| Null `wlc`, `bsscfg` or `scb` | reject with `-EINVAL`, no call |
| Hash mismatch | refuse load |
| Frame shorter than 24 bytes | reject with `-EINVAL` |
| Allocation failure | `-ENOMEM` |
| Missing AP `scb` at advance | abort, report link failure |
| Association IE buffer not host-owned | refuse injection |

## Unsafe boundary

```text
host state machine
  -> checked seam accessors (offset + width + null check)
  -> extern "C" object calls under the driver lock
  -> opaque object-owned memory
```

The host owns its state, keys, epochs and policy. It never owns object memory. A non-null pointer and
a known offset do not establish that the target is allocated, live or race-free; that provenance
must come from the object's own lifecycle contract.
