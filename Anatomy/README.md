# BCM4360 host driver: reconstruction anatomy

Implementable contracts for an open BCM4360 (`14e4:43a0`) host driver.

Status: specification only. No working Rust driver exists. No complete recoverability is claimed.
Audience: an implementer who needs exact seams, ownership, state machines and test gates.

## Read order

| # | File | Answers |
| --- | --- | --- |
| 1 | [01-hardware-partition.md](01-hardware-partition.md) | Host object, D11 microcode and native MAC split; what each mode must supply |
| 2 | [02-binary-abi.md](02-binary-abi.md) | Artifact identity, exact declarations, offset seams, ownership, hazards |
| 3 | [03-rx-tx-framing.md](03-rx-tx-framing.md) | D11 prefix, frame offset, validation, FCS, protected Block Ack mutation |
| 4 | [04-sae-state-machine.md](04-sae-state-machine.md) | Per-attempt SAE lifecycle, status forwarding, association injection, timers |
| 5 | [05-pmf-contracts.md](05-pmf-contracts.md) | BIP, MMIE, CCMP, PN/IPN, key epochs, SA Query, teardown |
| 6 | [06-rust-implementation.md](06-rust-implementation.md) | mac80211 reuse, module shape, locks, epochs, acceptance |
| 7 | [07-verification.md](07-verification.md) | Host tests, hardware matrix, vectors, ownership and concurrency cases |
| 8 | [08-limits-prerequisites.md](08-limits-prerequisites.md) | Required inputs, compat versus native limits, kernel and board constraints |

## Two implementation modes

| Mode | Description | Blob required | State |
| --- | --- | --- | --- |
| A. Compat ABI | Host code drives the frozen vendor object through the seams in file 02 | Yes, exact artifact only | Implementable now, board-dependent |
| B. Native MAC | Host implements MAC/PHY/DMA directly, b43-style | No | Research path; prerequisites in file 08 |

Mode A is a bridge. Mode B is the target. Both share the SAE transport and PMF contracts.

The current driver in this repository is Mode A in C. It works on 2.4 GHz and 5 GHz and inherits
upstream suspend support. Mode B is a specification, not an implemented port, and may reach feature
parity later or never.

## Invariants

- SAE mathematics runs in userspace. The kernel never sees a password, PWE, scalar or SAE state.
- PMF for management frames is host software. Device crypto is used for data only, if at all.
- No downgrade: a PMF-required non-SAE join is refused; an SAE join that associates without SAE is refused.
- MIC verification precedes every side effect; replay advances only after MIC success.
- MIC comparison is constant time.
- Every consumed RX packet is freed exactly once; every queued TX packet transfers ownership exactly once.
- ABI offsets are compatibility seams, not structure layouts or proof of allocation bounds.
- Board calibration and microcode are inputs, not constants.

## Source map

| Area | Relative source |
| --- | --- |
| SAE bridge, PMF, SA Query, key epochs | `wl-src/src/wl/sys/wl_cfg80211_hybrid.c` |
| BIP, replay, IGTK install | `wl-src/src/wl/sys/wl_pmf.h` |
| ABI declarations, packet free, management TX | `wl-src/src/wl/sys/wl_linux.c`, `wl_linux.h` |
| 802.11 protocol constants | `wl-src/src/common/include/proto/802.11.h` |
| Binary patch seams | `patches/wl_patch.py`, `patches/wl_reloc.py` |
| Host vectors | `tests/pmf.c`, `tests/rx.c`, `tests/rx.py` |

## External references

- Linux Rust kernel policy: https://docs.kernel.org/rust/general-information.html
- BCM4360 MAC/DMA fixes: https://lists.openwall.net/linux-kernel/2026/10/14/1949
- BCM4360 core revision 40/42 MAC setup: https://lkml.iu.edu/hypermail/linux/kernel/2611.1/16345.html
- BCM4360 AC microcode TX/RX formats: https://www.mail-archive.com/b43-dev%40lists.infradead.org/msg05065.html
- BCM4360 AC-PHY support: https://lists.openwall.net/linux-kernel/2026/11/12/3436
- IEEE 802.11-2020: clauses 12 (CCMP), 13 (BIP), 11.13 (SA Query)
- RFC 4493: AES-CMAC
- RFC 3610: CCM

## Non-goals

- Reconstructing vendor source or copying vendor bytes.
- Reimplementing wpa_supplicant, SAE, the 4-way handshake or RSN parsing.
- Defining a register map not backed by core and board data.
- Claiming the current driver lacks 5 GHz or suspend, which it has.
- Claiming a native port, hardware crypto, power save or TX calibration works.
