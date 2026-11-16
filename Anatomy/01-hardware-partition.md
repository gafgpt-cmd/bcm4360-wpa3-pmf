# 01. Hardware partition

What the host owns, what the device owns, and the inputs each needs.

## Device identity

| Field | Value | Note |
| --- | --- | --- |
| Vendor | `0x14e4` | Broadcom |
| Device | `0x43a0` | BCM4360 |
| Class | `0x028000` | Network other |
| Bus | PCIe | One function |
| `BAR0` | register window | size from PCI config |
| `BAR2` | mapped window | optional; used by the compatibility path |
| IRQ | shared PCIe | level, shared, one line |

Driver match must not rely on class alone. Match `14e4:43a0` exactly, plus any explicitly listed sibling IDs. Unknown siblings are rejected, not coerced.

Cores present (reference architecture; confirm from board data before writing any register access):

| Core | Role | Revisions seen in public work |
| --- | --- | --- |
| ChipCommon | clocks, resets, OTP/SROM access, GPIO | board-specific |
| 802.11 MAC | TX/RX engines, queues, DMA, TSF, keys | 40 / 42 |
| PHY | AC-PHY, radio | radio 2069 class |
| PCIe | bus interface, DMA index/status | board-specific |

Do not write a register map from this file. Revisions, register bases and offsets come from board data, not from inference.

## Host/device split

| Function | Owner | Contract |
| --- | --- | --- |
| SAE commit/confirm math | userspace | kernel never sees password, PWE, scalar, SAE state |
| RSN parsing, 4-way, key derivation | userspace | kernel receives only keys and policy |
| PMF MIC, replay, SA Query, protected teardown | host | implemented in host software; see file 05 |
| 802.11 management frame body construction | host | auth, assoc, action bodies built on host |
| MAC TX/RX, aggregation, retries, rate control | device | host submits frames and descriptors |
| CCMP for data frames | device (assumed) | host must not assume device crypto for management frames |
| BIP for group management frames | host | device does not protect management frames |
| Channel, bandwidth, TX power | split | host policy, device calibration |
| Microcode execution | device | host downloads and starts it |
| Board calibration | device consumes | host supplies SROM/OTP/NVRAM contents |

Hard rule: management-frame protection is host software. Data-frame crypto may be device offload, but no management path may depend on it.

## Lifecycle

Order is a contract. Skipping or reordering a stage produces silent DMA corruption or a dead radio.

| # | Stage | Host action | Exit condition |
| --- | --- | --- | --- |
| 1 | PCI enable | enable device, set bus master, map BARs | config space readable |
| 2 | Power | bring device to D0, release reset | core clock alive |
| 3 | Core probe | enumerate cores, read revisions | expected MAC/PHY present |
| 4 | Board data | load SROM/OTP/NVRAM image | MAC address and PHY params available |
| 5 | Microcode | download image, release core | microcode idle or running |
| 6 | MAC attach | init queues, DMA rings, filters, TSF | rings armed, no pending error |
| 7 | PHY attach | init radio, load tables, calibrate | calibration complete |
| 8 | Up | enable TX/RX, start interrupts | first beacon/probe visible |
| 9 | Down | stop queues, mask IRQ, drain | no in-flight DMA |
| 10 | Reset | core reset, clear state | repeatable from stage 2 |

Failure at any stage unwinds in reverse. Partial init must not leave DMA armed.

### Reset

- Reset is a core-level operation, not a PCI function reset.
- Quiesce TX before reset: stop queueing, wait for completion or timeout, then reset.
- Clear driver-side ring indices after reset. Device-side indices are meaningless across reset.
- Reset must be idempotent: two consecutive resets leave the same state.
- Reset while associated is a link loss, not a silent recovery. Report disconnect.

### MMIO

- All register access through one mapped window, `readl`/`writel` or ordered equivalents.
- No cached shadow of device state that can diverge. Re-read on recovery.
- Barriers: use the kernel's MMIO accessors; do not reorder index writes past doorbells.
- Registers are 32-bit unless board data says otherwise. No 64-bit write without a documented pair order.

### DMA

| Item | Requirement |
| --- | --- |
| Addressing | 64-bit capable; descriptor and status addresses must be bus addresses |
| Descriptor rings | TX and RX; count and alignment from board data |
| Index/status | host and device indices in the documented layout; no invented stride |
| Ownership | host writes descriptors, then doorbell; device writes status, then IRQ |
| Ordering | descriptor contents visible before doorbell; status read before reuse |
| Coherence | use dma_map/unmap or coherent allocation consistently |
| Bounds | every descriptor length validated before use |
| Drain | on down, wait for in-flight descriptors or reset the engine |

Do not mix coherent and streaming mappings on the same buffer. Do not reuse a descriptor before its status is observed.

### Interrupts

- One shared IRQ line. Handler must tolerate `IRQ_NONE` for other devices.
- Top half: acknowledge device, mask if needed, schedule deferred work.
- Bottom half: process RX ring, TX completions, errors.
- No sleeping in the handler. No allocation that can sleep.
- Disable the line before teardown, then drain deferred work.
- Spurious interrupt must not advance any index.

### Microcode

| Input | Contract |
| --- | --- |
| Image | board-specific binary, supplied as a file or firmware blob |
| Version | must match the MAC/PHY revision; reject mismatches |
| Size | exact; no truncation or padding guess |
| Load | through the documented core interface |
| Verify | read back a known status or revision before use |

Microcode is an input, not a constant. Do not embed an image in the repository. Do not assume a version works because another board works.

### Calibration and board data

| Data | Source | Use |
| --- | --- | --- |
| MAC address | SROM/OTP/NVRAM | interface address; never hardcode |
| PHY parameters | SROM/OTP/NVRAM | channel, power, filter tables |
| Regulatory limits | board data plus host policy | channel and power clamp |
| Antenna/gain | board data | per-board, not per-driver |
| PA tables | board data | TX power |
| Crystal/clock | board data | timing |

Calibration is per board. A generic table is a bug unless board data proves the board is the reference design.

### Power

- D0 is the only supported operating state.
- Suspend/resume is not covered by this specification. If implemented later, it must re-run stages 2-8 and treat the link as lost.
- ASPM and power-save states are not specified. Do not enable them by default.
- Radio kill (rfkill) maps to down/up, not to a register poke.

## Mode A: compatibility path

Host Rust drives the frozen vendor object through the seams in file 02.

Host supplies:

- exact artifact identity (hash-gated),
- PCI probe and mapping,
- IRQ and tasklet/work plumbing,
- packet free callback,
- management TX/RX taps,
- SAE and PMF state.

Device supplies:

- MAC/PHY/DMA/microcode,
- data-frame crypto (assumed),
- rate control and aggregation.

This mode is a bridge, not a target. It inherits the artifact's assumptions about core revisions and board data.

## Mode B: native MAC

Host Rust implements MAC/PHY/DMA directly.

Required before any code:

| Prerequisite | Why |
| --- | --- |
| Core revisions for the exact board | register bases and feature bits differ |
| Microcode image and load protocol | MAC does not run without it |
| SROM/OTP layout for the board | calibration and MAC address |
| DMA descriptor layout and index/status addresses | TX/RX cannot work otherwise |
| PHY/radio init tables | radio stays deaf without them |
| TX calibration and power tables | transmit is undefined otherwise |
| Regulatory channel/power mapping | legal and functional |

Mode B is a research path. File 08 lists what remains open.

## Unknowns that block a register map

- Exact core base addresses for this board.
- Exact DMA ring sizes, alignments and index/status addresses.
- Microcode load register sequence and status encoding.
- PHY table format and calibration completion signal.
- GPIO/clock/reset bit assignments.
- OTP/SROM revision and section offsets.

Until these are resolved from board data, no register map is defined here. A guessed map is worse than a declared gap.

## Partition invariants

1. Userspace owns SAE and RSN; host owns PMF; device owns MAC/DMA.
2. Microcode and calibration are inputs, never constants.
3. Reset is idempotent and drains DMA.
4. Interrupt handling never sleeps and never advances an index on a spurious IRQ.
5. No management protection depends on device crypto.
6. Mode A is hash-gated to one artifact; Mode B is gated on board data.
7. No register access is written from inference.
