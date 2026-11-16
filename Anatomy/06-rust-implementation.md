# 06. Rust implementation plan

Module structure, kernel interfaces, ownership and acceptance for a Rust host driver.

## Crate shape

```text
bcm4360/
  src/lib.rs            module entry, PCI driver, lifecycle
  src/device.rs         BAR mapping, reset, IRQ, DMA rings
  src/rx.rs             frame location, validation, dispatch
  src/tx.rs             management frame build and queue
  src/sae.rs            SAE state machine and epochs
  src/pmf.rs            BIP, CCMP, replay, SA Query
  src/keys.rs           key install/delete, zeroization
  src/abi.rs            Mode A seam accessors and extern calls
  src/shim.c            optional C glue (cfg80211, blob calls)
```

- `#![no_std]` kernel module.
- Use the kernel crate for allocation, locks, workqueues, PCI, DMA, MMIO and IRQ.
- No userspace libraries. No `std`. No global allocator outside the kernel one.
- Edition and MSRV follow the target kernel. Do not pin newer than the kernel supports.

## Two builds

| Build | Feature | Contains |
| --- | --- | --- |
| A. Compat | `compat-abi` | `abi.rs`, blob calls, hash gate |
| B. Native | default | `device.rs` native MAC/PHY path |

Both share `sae.rs`, `pmf.rs`, `keys.rs`, `rx.rs`, `tx.rs` contracts.

The compat build is not the target. It exists to validate the host contracts against a working MAC.

## Kernel interfaces

| Need | Preferred | Fallback |
| --- | --- | --- |
| Allocation | kernel `KBox`, `KVec`, `KVVec` | `alloc` with kernel allocator |
| Mutex | `kernel::sync::Mutex` | C shim mutex |
| Spinlock | `kernel::sync::SpinLock` | C shim spinlock |
| Workqueue | `kernel::workqueue` | C shim `work_struct` |
| Delayed work | `kernel::workqueue::DelayedWork` | C shim `delayed_work` |
| PCI | `kernel::pci::Device`, `Bar` | C shim probe |
| MMIO | `kernel::io::Io` | `readl`/`writel` shim |
| DMA | `kernel::dma` | C shim `dma_alloc_coherent` |
| IRQ | `kernel::irq::Registration` | C shim `request_irq` |
| cfg80211 | C shim ops | none in Rust yet |
| crypto | C shim `crypto_aead` / `crypto_shash` | pure Rust AES-CMAC and CCM |

Use a C shim only where the kernel Rust API does not yet expose the subsystem. Keep the shim thin: register, call one Rust entry point, return.

Do not reimplement AES or CMAC in Rust unless the kernel crypto API is unavailable. If reimplemented, it must pass the vectors in file 07 and be constant time.

## Ownership model

| Object | Owner | Lifetime |
| --- | --- | --- |
| Adapter | PCI driver | probe to remove |
| BAR mapping | adapter | probe to remove |
| DMA rings | adapter | up to down |
| Link session | adapter, replaced per connect | connect to disconnect |
| Keys | session | install to delete |
| Epoch | adapter, atomic | monotonic |
| RX skb | host until consumed or handed off | one path |
| TX skb | host until queued | one path |
| Userspace SAE state | userspace | never in kernel |

Rules:

- One `LinkSession` per attempt. A new attempt replaces it; the old one is dropped and zeroized.
- Keys live in the session, not globally.
- Captured blob pointers live only in a session and are cleared on teardown.
- Packets are owned by exactly one component at a time.
- No `Arc` cycle between adapter and session.

## Concurrency

### Lock order

Acquire top to bottom. Never reverse.

| Order | Lock | Type | Protects |
| --- | --- | --- | --- |
| 1 | `cfg_lock` | mutex | cfg80211/rtnl-facing state |
| 2 | `dev_lock` | mutex | reset, queues, blob calls |
| 3 | `state_lock` | spinlock | session state, epoch |
| 4 | `key_lock` | spinlock | IGTK, PTK, TX PN |
| 5 | `replay_lock` | spinlock | RX replay floors |

Rules:

- Never take a mutex while holding a spinlock.
- Never sleep under a spinlock.
- Free packets and cancel work outside all locks.
- Document any new lock in this table before adding it.

### Epochs

```text
epoch: AtomicU64
```

- Increment on connect, disconnect, reset and abort.
- Every completion captures the epoch and rechecks before applying.
- A stale completion is dropped.
- Epoch wrap is not a practical concern; use `fetch_add`.

### Cancellation

- Cancel delayed work synchronously on disconnect and before detach.
- Cancellation must tolerate an already-running callback.
- Do not hold a lock the callback needs while cancelling.

## Device layer

### Probe

1. Match `14e4:43a0`.
2. Enable device, set bus master.
3. Map BARs.
4. Read core revisions and reject unknown.
5. Load board data.
6. Load microcode.
7. Attach MAC, then PHY.
8. Register IRQ.
9. Register network/cfg80211 interface.

Any failure unwinds in reverse.

### Reset and up/down

- `down`: stop TX, mask IRQ, drain rings, clear session.
- `up`: reinit rings, enable IRQ, start queues.
- `reset`: down, core reset, up.
- Reset reports link loss; it does not silently restore association.

### IRQ

- Top half acknowledges and schedules deferred work.
- Deferred work processes completions and RX.
- No sleeping in the top half.
- Spurious IRQ returns `IRQ_NONE` and changes nothing.

## RX layer

- Locate frame from `wlc + 0x6bc` in compat mode; from the native descriptor in native mode.
- Validate per file 03 before any parse.
- Reject non-linear skbs.
- Strip FCS once.
- Route:

| Frame | Route |
| --- | --- |
| SAE auth after START | userspace |
| SAE auth before START | consume |
| protected robust mgmt | verify CCMP then handle |
| group robust mgmt | verify BIP then handle |
| unprotected teardown | drop, maybe SA Query |
| data | device/host data path |

- Free exactly once.

## TX layer

- Build management frames through the device interface.
- Never hand-build the MAC header if the helper exists.
- Reserve CCMP header and MIC space before encryption.
- Queue with the real station context.
- On failure, free with the correct ownership flag.

## SAE layer

State machine from file 04. Timers and generations. No crypto.

Public surface:

```rust
trait SaeTransport {
    fn start(&self, epoch: u64) -> Result;
    fn forward(&self, epoch: u64, frame: &[u8]) -> Result;
    fn cancel(&self, epoch: u64);
}
```

The kernel calls `start` after the placeholder grace, forwards post-START frames, and cancels on teardown.

## PMF layer

Public surface:

```rust
trait Pmf {
    fn install_igtk(&self, id: u8, key: &[u8; 16], seq: Option<&[u8; 6]>) -> Result;
    fn verify_bip(&self, frame: &[u8]) -> Result<()>;
    fn verify_ccmp(&self, frame: &[u8]) -> Result<Vec<u8>>;
    fn install_ptk(&self, tk: &[u8; 16]) -> Result;
}
```

- `verify_*` advances replay only on success.
- `verify_ccmp` returns plaintext or an error; never partial output.
- Key install is serialized.
- All comparisons constant time.

## Key layer

- Fixed-size arrays for keys, not heap buffers.
- `valid` flag before use.
- Zeroize on delete and detach with a volatile write or the kernel's secure erase.
- Never log key bytes.
- Never expose keys through debugfs or sysfs.

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

- All blob calls and all raw offset access live in `abi.rs`.
- Each accessor validates pointer non-null, offset within the documented table, and type width.
- No `unsafe` in `sae.rs`, `pmf.rs`, `keys.rs` except for zeroization and atomics.
- Every `unsafe` block has a safety comment naming the invariant.

## Acceptance criteria

| # | Criterion | Proof |
| --- | --- | --- |
| 1 | Builds against target kernel | module build |
| 2 | Hash gate rejects a wrong artifact | unit test |
| 3 | RX validation rejects malformed frames | unit tests |
| 4 | PMF vectors pass | unit tests |
| 5 | Replay and rekey behavior correct | unit tests |
| 6 | SAE transitions legal only | state tests |
| 7 | No secrets in logs or files | manual audit plus secret scan |
| 8 | Lock order holds under stress | lockdep clean |
| 9 | No double free | fault injection |
| 10 | Down/up and reset idempotent | repeated runs |
| 11 | Link loss reported, never silent | live test |
| 12 | No management protection depends on device crypto | code audit |

## What this plan does not do

- Does not claim a working driver.
- Does not define a register map.
- Does not implement SAE, RSN parsing or the 4-way handshake.
- Does not enable power save, suspend or hardware crypto by default.
- Does not make Mode A portable to another artifact.
