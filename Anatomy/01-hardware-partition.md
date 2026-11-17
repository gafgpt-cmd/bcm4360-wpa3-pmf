# 01. Hardware partition

Who owns what, and which inputs each mode needs.

## Device identity

| Field | Value | Note |
| --- | --- | --- |
| Vendor | `0x14e4` | Broadcom |
| Device | `0x43a0` | BCM4360 |
| Class | `0x028000` | Network other |
| Bus | PCIe | One function |

Match `14e4:43a0` exactly, plus any explicitly listed sibling IDs. Do not match on class alone.

Cores relevant to a driver:

| Core | Role | Public revision evidence |
| --- | --- | --- |
| ChipCommon | clocks, resets, OTP/SROM access, GPIO | board-specific |
| 802.11 MAC | TX/RX engines, queues, DMA, TSF, keys | core rev 40 / 42 |
| AC-PHY | radio and baseband | radio 2069 class |
| PCIe | bus interface, DMA index/status | board-specific |

Register bases, offsets and feature bits come from core and board data. This file defines no
register map.

## Three execution domains

Do not collapse these into one "firmware" owner.

| Domain | Runs | Owns |
| --- | --- | --- |
| Host object, Mode A | vendor `wlc_hybrid.o_amd64` linked into `wl.ko` | MAC state machine, rate control, aggregation, TX/RX queues, key plumbing |
| D11 microcode | device CPU | PHY/D11 timing, channel and calibration execution, low-level TX/RX |
| Host native, Mode B | an open driver | MAC, PHY programming, DMA rings, interrupts, calibration |

In Mode A the host object performs the MAC work; the device does not run `wlc_hybrid`. In Mode B
the host replaces that object entirely. The same jobs are never owned by both modes at once.

## Function split

| Function | Owner | Contract |
| --- | --- | --- |
| SAE commit/confirm math | userspace | kernel never sees password, PWE, scalar or SAE state |
| RSN parsing, 4-way, key derivation | userspace | kernel receives keys and policy only |
| PMF MIC, replay, SA Query, protected teardown | host | implemented in host software; file 05 |
| Management frame body construction | host | auth, assoc and action bodies built on host |
| MAC TX/RX, aggregation, retries, rate control | Mode A host object / Mode B host | host submits frames and descriptors |
| Data-frame CCMP | device, when offloaded | host must not depend on it for management |
| BIP for group robust management | host | device does not protect management frames |
| Channel, bandwidth, TX power | split | host policy, device calibration |
| Microcode execution | device | host downloads and starts it |
| Board calibration | device consumes | host supplies SROM/OTP/NVRAM contents |

Hard rule: management-frame protection is host software. A data-path crypto offload never covers
the management path.

## Mode A: compatibility object

Host supplies:

- exact artifact identity (hash-gated; file 02),
- PCI probe and mapping,
- IRQ and deferred-work plumbing,
- packet free callback,
- management TX/RX taps,
- SAE and PMF state.

The object supplies the MAC state machine, rate control, aggregation and queueing. It still needs
the D11 microcode and valid board data; the bridge does not remove those dependencies.

Mode A inherits the object's assumptions about core revisions, board data and kernel ABI. It is a
bridge, not a portable driver.

## Mode B: native MAC

Mode B replaces the host object with an open MAC/PHY/DMA implementation. It must reuse the public
b43 work rather than restart from a blank register map:

| Prerequisite | Public basis |
| --- | --- |
| Core rev 40/42 MAC setup, 64-bit DMA, MAC/PHY fixes | b43 series in the external references |
| AC-PHY and radio 2069 attach, channel and power tables | b43 AC-PHY series |
| AC microcode TX/RX descriptor and header formats | b43 AC microcode series |
| Board SROM/OTP/NVRAM contents and calibration | board data, not a generic table |
| Microcode image matching the MAC/PHY revision | device package, never embedded in the repo |
| Reset, clock, GPIO and PCIe power sequence | core and board data |

Mode B cannot start from this document alone. It starts from the public b43 core rev 40/42 and
AC-PHY implementation, then adds what that implementation does not yet cover.

## Board data and calibration

| Data | Source | Use |
| --- | --- | --- |
| MAC address | SROM/OTP/NVRAM | interface address; never hardcode |
| PHY parameters | SROM/OTP/NVRAM | channel, power, filter tables |
| Regulatory limits | board data plus host policy | channel and power clamp |
| Antenna/gain, PA tables | board data | per-board TX power |
| Crystal/clock | board data | timing |

Calibration is per board. A generic table is a bug unless board data proves the board is the
reference design.

## Power and link state

- D0 is the only specified operating state for Mode B.
- The current Mode A driver inherits upstream suspend support; a native port must re-establish
  the link on resume and must not assume it survives.
- ASPM and power-save states are not specified here. Do not enable them by default in a new port.
- Radio kill maps to down/up, not to a register poke.
- A reset while associated is a link loss, not a silent recovery.

## Partition invariants

1. Userspace owns SAE and RSN; host owns PMF; the mode owner runs MAC/DMA.
2. Mode A and Mode B never own the same MAC job simultaneously.
3. Microcode and calibration are inputs, never constants.
4. Mode B reuses public b43 core rev 40/42 and AC-PHY work before inventing anything.
5. No management protection depends on device crypto.
6. No register access is written from inference.
