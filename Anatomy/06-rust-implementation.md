# 06. Rust implementation plan

Design constraints for a Rust host driver. No Rust driver is implemented.

## Reuse mac80211

A native port is a **mac80211 driver**, not a cfg80211 driver that rebuilds management.

| Layer | Owner | Consequence |
| --- | --- | --- |
| SAE, RSN, 4-way, key derivation | `wpa_supplicant` | kernel never sees the password or SAE state |
| Management frame construction and parsing | mac80211 | do not duplicate auth/assoc/action builders |
| External SAE transport | cfg80211 `external_auth` | forward auth frames; do not interpret status |
| PMF (BIP, CCMP management, SA Query) | mac80211 where the kernel supports it | do not reimplement in Rust |
| Block Ack | mac80211 | do not hand-roll ADDBA state |
| Data crypto | mac80211 and kernel crypto | no device offload dependency |
| Hardware, DMA, interrupts, calibration | the Rust driver | the only new code |

The Mode A C driver reimplements much of this because the frozen object bypasses mac80211. A native
port must not copy that design into Rust.

## Artifact status

The pinned target kernel tree supplies generated Rust metadata (`.rmeta`) but no Rust kernel source.
Therefore this document names no specific Rust kernel API, module or trait. Use subsystem contracts
and thin C bindings where the Rust surface is not present. Any API name asserted without a source
tree is a guess and must not enter code or tests.

## Module shape

A design sketch, not an existing tree:

```text
bcm4360/
  src/lib.rs            module entry, PCI driver, lifecycle
  src/device.rs         BAR mapping, reset, IRQ, DMA rings
  src/rx.rs             descriptor handling, frame handoff to mac80211
  src/tx.rs             descriptor handling, frame handoff from mac80211
  src/cal.rs            board data and calibration inputs
  src/abi.rs            Mode A seam accessors, if compat is built
  src/shim.c            cfg80211/mac80211 registration, crypto and workqueue glue
```

- Kernel module; no `std` and no userspace libraries.
- Allocation, locks, workqueues, PCI, DMA, MMIO and IRQ come from the kernel.
- Edition and MSRV follow the target kernel. Do not pin newer than the kernel accepts.

## Two builds

| Build | Feature | Contains |
| --- | --- | --- |
| A. Compat | `compat-abi` | `abi.rs`, object calls, hash gate |
| B. Native | default | `device.rs` MAC/PHY/DMA path, mac80211 registration |

Both share the SAE transport and PMF policy contracts in files 04 and 05. The compat build is a
validation bridge; it is not the target.

## Kernel interfaces

| Need | Use |
| --- | --- |
| Allocation | kernel allocator |
| Mutex, spinlock | kernel lock primitives |
| Workqueue, delayed work | kernel workqueue |
| PCI, BAR mapping | kernel PCI API |
| MMIO | kernel I/O accessors |
| DMA | kernel DMA API |
| IRQ | kernel IRQ registration |
| cfg80211, mac80211 | C shim registration and callbacks |
| AES-CMAC, AES-CCM | kernel crypto API; C shim only when Rust bindings are absent |

Do not implement AES or CMAC in Rust. The current C driver relies on the kernel AES-CMAC library and
a synchronous `ccm(aes)` transform. A native port uses the same kernel crypto contracts. There is no
pure-Rust crypto fallback: if the kernel API is unavailable, the feature is unavailable.

## Ownership model

| Object | Owner | Lifetime |
| --- | --- | --- |
| Adapter | PCI driver | probe to remove |
| BAR mapping | adapter | probe to remove |
| DMA rings | adapter | up to down |
| Link session | adapter, replaced per connect | connect to disconnect |
| Keys | mac80211 or session | install to delete |
| Epoch | adapter, atomic | monotonic |
| RX skb | mac80211 after handoff | one path |
| TX skb | device after queue | one path |
| SAE state | userspace | never in kernel |

Rules:

- One link session per attempt. A new attempt replaces it; the old one is dropped and zeroized.
- Keys live in the session or in mac80211, never in two places.
- Captured object pointers live only in a compat session and are cleared on teardown.
- Packets have exactly one owner at a time.
- No reference cycle between adapter and session.

## Concurrency

### Lock order

Acquire top to bottom. Never reverse.

| Order | Lock | Type | Protects |
| --- | --- | --- | --- |
| 1 | cfg lock | mutex | cfg80211/rtnl-facing state |
| 2 | device lock | mutex | reset, queues, object calls |
| 3 | state lock | spinlock | session state, epoch |
| 4 | key lock | spinlock | IGTK, PTK, TX PN |
| 5 | replay lock | spinlock | RX replay floors |

Rules:

- Never take a mutex while holding a spinlock.
- Never sleep under a spinlock.
- Free packets and cancel work outside all locks.
- Document any new lock in this table before adding it.

### Epochs

- Increment on connect, disconnect, reset and abort.
- Key install brackets the epoch: even means stable, odd means installing.
- Every completion captures the epoch and rechecks before applying.
- A stale completion is dropped.
- Use `fetch_add`; wrap is not a practical concern.

### Cancellation

- Cancel delayed work synchronously on disconnect and before detach.
- Cancellation must tolerate an already-running callback.
- Do not hold a lock the callback needs while cancelling.

## Device layer

### Probe

1. Match `14e4:43a0`.
2. Enable the device, set bus master.
3. Map BARs.
4. Read core revisions; reject unknown combinations.
5. Load board data.
6. Load and start microcode.
7. Attach the MAC, then the PHY.
8. Register the IRQ.
9. Register with mac80211/cfg80211.

Any failure unwinds in reverse. Partial init must not leave DMA armed.

### Reset and up/down

- `down`: stop TX, mask IRQ, drain rings, clear session.
- `up`: reinitialise rings, enable IRQ, start queues.
- `reset`: down, core reset, up.
- Reset reports link loss; it does not silently restore association.
- Suspend/resume is a link loss unless the port explicitly re-establishes it. Do not claim resume.

### IRQ

- Top half acknowledges and schedules deferred work.
- Deferred work processes completions and RX.
- No sleeping in the top half.
- A spurious IRQ returns the not-mine result and changes nothing.

## RX layer

- Locate the frame from the native descriptor, not from `wlc + 0x6bc`; the offset field is a Mode A
  compatibility seam.
- Validate length, offset and version before parsing.
- Hand management and data frames to mac80211; do not build a second dispatch tree.
- Free or hand off exactly once.

## TX layer

- Accept frames from mac80211.
- Do not build the MAC header or management body in the driver.
- Reserve crypto space only when mac80211 has not already done so.
- Queue with the station context supplied by mac80211.
- On failure, return the frame to mac80211 for freeing.

## SAE layer

mac80211 and cfg80211 already provide external-auth transport. The driver only moves frames.

Design pseudocode:

```text
on auth frame:
    if external_auth_started:
        cfg80211_rx_mgmt(frame)
    else:
        drop
on external_auth(status):
    if status == 0:
        mac80211 proceeds to association
    else:
        mac80211 aborts and reports failure
```

Do not reimplement SAE timers, status interpretation or association injection. Those exist in the
Mode A C bridge only because the object bypasses mac80211.

## PMF layer

Use mac80211's PMF support for management protection, BIP, replay and SA Query. The driver does not
own a second CCMP implementation.

If a kernel lacks a required PMF capability, the correct result is a refused WPA3 connection, not a
private crypto stack.

## Key layer

- Keys are owned by mac80211 or by the device key table.
- The driver never logs key bytes and never exposes them through debugfs or sysfs.
- Zeroize any driver-local copy on delete and detach.

## Error model

| Error | Meaning |
| --- | --- |
| `EINVAL` | malformed input or null context |
| `ENOMEM` | allocation failure |
| `ENOKEY` | key not installed |
| `EOPNOTSUPP` | policy forbids the request |
| `EIO` | device or DMA failure |
| `ETIMEDOUT` | bounded wait expired |
| `EAGAIN` | retryable, attempt superseded |

Map to kernel errno at the boundary. Do not invent positive error codes.

## Unsafe boundary

- Raw offset access exists only in `abi.rs`, only for the compat build.
- Each accessor checks null, documented offset and width.
- `unsafe` outside `abi.rs` is limited to atomics and zeroization.
- Every `unsafe` block names the invariant that makes it sound.
- A non-null pointer plus a known offset is not proof of allocation bounds or liveness.

## Acceptance criteria

| # | Criterion | Proof |
| --- | --- | --- |
| 1 | Builds against the target kernel | module build |
| 2 | Hash gate rejects a wrong artifact | unit test |
| 3 | RX validation rejects malformed frames | unit tests |
| 4 | PMF vectors pass | unit tests |
| 5 | Replay, rekey and epoch behavior correct | unit tests |
| 6 | SAE transport forwards all post-START statuses | transport test |
| 7 | No secrets in logs or files | secret scan |
| 8 | Lock order holds under stress | lockdep |
| 9 | No double free | fault injection |
| 10 | Down/up and reset idempotent | repeated runs |
| 11 | Link loss is reported, never silent | hardware test |

## What this plan does not do

- Does not claim a working Rust driver.
- Does not name Rust kernel APIs that the pinned tree does not supply.
- Does not define a register map.
- Does not implement SAE, RSN parsing, the 4-way handshake, PMF or Block Ack.
- Does not enable power save, suspend or hardware crypto by default.
- Does not make Mode A portable to another artifact.
