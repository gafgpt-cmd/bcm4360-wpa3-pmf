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

The ABI applies only to this pristine hash. A different build, even with the same version string,
may have different offsets and is unsupported.

Current C bridge: manually verify the pristine object's whole-file SHA-256 before running
`build.sh`. The build does not compute this digest or enforce rejection of other artifacts.
Do not build or load a candidate whose pristine input identity has not been verified.

Required automatic gate for a future compatibility implementation, not implemented in the current bridge:

1. Read image bytes.
2. Compute SHA-256.
3. Compare the digest with the expected value. The digest is public, so this is an ordinary
   equality check, not a secret-dependent comparison.
4. Reject the pristine input on mismatch; report expected and actual.
5. Never patch or link an image that failed the gate.

## ABI form

- SysV AMD64 calling convention.
- Integer arguments in `RDI, RSI, RDX, RCX, R8, R9`, then stack.
- Return in `RAX`. The object has no uniform error convention; use the contract for each symbol.
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

Pointers are borrowed values into object state. A non-null value proves only that the pointer is
nonzero. It does not prove that the target is mapped, allocated, live, within bounds, or free of a
concurrent teardown.

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
long wlc_sendauth(void *a1, void *a2, void *a3, void *a4, int alg,
                  int seq, int status, void *a8, void *a9, unsigned char a10);
```

| # | Register | Role |
| --- | --- | --- |
| 1 | `RDI` | `bsscfg`; captured by the host wrapper |
| 2 | `RSI` | destination address; captured as the BSSID fallback |
| 3 | `RDX` | third MAC address argument |
| 4 | `RCX` | AP `scb`; captured by the host wrapper |
| 5 | `R8D` | auth algorithm |
| 6 | `R9D` | auth sequence |
| 7 | `[rsp+0x8]` | auth status |
| 8 | `[rsp+0x10]` | key/IV context, used only for the shared-key challenge |
| 9 | `[rsp+0x18]` | IE buffer; byte 1 is the element length |
| 10 | `[rsp+0x20]` | queue flag byte |

The roles above are observed in the artifact. The extern declaration is the authority for the
types and positions; do not rename the arguments into a different signature.

Body construction:

- three little-endian 16-bit fields `alg`, `seq`, `status`, six bytes total,
- at most one IE appended after the body, copied as element ID, length and payload,
- shared-key challenge only when `alg == 1`, `seq == 3` and `status == 0`; it uses the key/IV
  context and protected frame control `0x40b0`,
- SAE commit and confirm bodies are supplied by userspace, not built here.

Return and ownership:

- A nonzero return is the constructed packet pointer after the queue call.
- The function returns `0` on its early-completion path and when the queue call reports failure.
- Do not infer failure from a negative value; this symbol has no negative-error convention.
- The caller's buffers stay borrowed. The object constructs and owns the frame. Do not free the
  passed buffers or the returned packet.

## Auth RX

```c
void wlc_authresp_client(void *bsscfg, long p2,
                         unsigned short *body, unsigned int len,
                         unsigned char p5);
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
                     const unsigned char *mac, int p4, int p5);
```

Location: `.text + 0x5b79d`, minted as a symbol during the build.

On `code == 0` it marks the station authenticated, advances association state and sends the
association request. The AP `scb` must already exist. Call it from the association-advance path,
which holds `WL_LOCK`, after the association IEs are installed.

## Management TX

```c
void *wlc_frame_get_mgmt_ex(void *wlc, uint16 fc, const void *da,
                            const void *sa, const void *bssid,
                            int body_len, void **body_out,
                            uint hdr_pad, int tail);

int wlc_queue_80211_frag(void *wlc, void *pkt, void *txq,
                         void *scb, void *bss, char flag,
                         void *key, uint32 zero);
```

`da`, `sa`, `bssid` are the three MAC header addresses in frame order. `body_out` receives a
writable buffer of `body_len` bytes. Fill it, then queue with the real `scb` and
`bss = *(scb + 0x18)`.

Return handling:

- The host wrapper returns `0` when a packet and body buffer were produced.
- It calls `wlc_queue_80211_frag` and ignores that call's boolean result.
- It does not free on a failed queue call. The packet is still handed to the object once.
- A `0` return proves allocation and the dispatch call only. It does not prove that the object
  accepted the frame, transmitted it, or completed TX.
- Do not free the packet after the call.

Queuing with a null `scb` makes the object substitute a default station in the packet tag; the TX
completion path then dereferences it. Always pass the AP `scb`.

## Lookup helpers

```c
void *wlc_bsscfg_find_by_bssid(void *wlc, const void *bssid);
void *wlc_scbfind(void *wlc, void *bsscfg, const unsigned char *mac);
```

Both return borrowed pointers. Do not free. Revalidate after any disconnect, reset or detach.

## Packet ownership

| Path | Ownership |
| --- | --- |
| RX consumed by host | host frees once |
| RX passed to `wlc_recv` | object |
| Constructed TX packet passed to `wlc_queue_80211_frag` | object |
| TX never queued | host frees with the drop flag |
| Caller buffers passed to any seam | stay borrowed |

Rules:

- Exactly one free per packet.
- Never free the constructed packet after the queue call, even if the wrapper reports failure.
- Never hand the same packet to two consumers.
- Free outside any lock that the free path can re-enter.

## Locking and lifetime

- The host management TX path takes `WL_LOCK` around `wlc_frame_get_mgmt_ex` and
  `wlc_queue_80211_frag`. The association-advance path takes `WL_LOCK` around
  `re_auth_advance`.
- `WL_LOCK` is `down/up` in all-passive mode and `spin_lock_bh` otherwise. Do not add a second
  host lock around these seams.
- Captured pointers are valid only for the current association and only while the object is attached.
- Clear every captured pointer on disconnect, reset and detach. A detach race is a use-after-free,
  not a recoverable error.

## Patch semantics

Two edits make the bridge possible; the instruction patch preserves code length.

| Edit | Location | Semantics |
| --- | --- | --- |
| Auth clamp passthrough | file offset `0x47d8d` in `.text` | store the requested auth algorithm verbatim into `bsscfg + 0x94` instead of clamping nonzero to `1` |
| Relocation redirect | `.rela.text` entries | point `wlc_sendauth`, `wlc_authresp_client`, `wlc_recv`, `wlc_sendmgmt` calls at host wrappers |

`.text` virtual address `V` maps to file offset `V + 0x40`.

`wl_patch.py` checks the expected instruction bytes, also accepting the already-patched bytes.
On mismatch it exits without writing. This is a local byte check, not whole-object identification.
`wl_reloc.py` checks ELF64 identification, reads section metadata and requires the redirect symbols
on an unpatched input. It does not validate original instruction bytes or a whole-object digest.
It appends symbol/string tables and updates section offsets without moving code.

`build.sh` pipes both helpers through `sed` without `pipefail`; helper failure can be masked.
Build success alone proves neither pristine identity nor successful patching.
For the current bridge, manually verify pristine identity, helper results and the resulting edits
before loading. Automatic hash rejection and propagation of helper failures are prerequisites
for a future compatibility implementation.

These edits are seams, not source.

## Boundary rules

| Condition | Result |
| --- | --- |
| Null `wlc` or `wl->wlc` | reject with `-EINVAL`, no object call |
| Null `bsscfg` at advance | return without calling the object |
| Pristine hash mismatch | unsupported; operator must stop before build/load; no automatic current-bridge gate |
| Frame shorter than 24 bytes | reject with `-EINVAL` |
| Allocation failure | `-ENOMEM` |
| Missing AP `scb` at advance | return, report link failure |
| Association IE buffer | must stay valid until `wl_re_release_assoc_ie`; the object frees it at teardown |

## Unsafe boundary

```text
host state machine
  -> checked seam accessors (offset + width + null check)
  -> extern "C" object calls through the locked host seams
  -> opaque object-owned memory
```

The host owns its state, keys, epochs and policy. It never owns object memory. A non-null pointer and
a known offset do not establish that the target is mapped, allocated, live or race-free; that
provenance must come from the object's own lifecycle contract.
