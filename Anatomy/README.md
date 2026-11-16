# BCM4360 host driver: reconstruction anatomy

Scope: implementable contracts for an open BCM4360 (`14e4:43a0`) host driver in Rust.
Status: specification only. No working Rust driver is claimed. No complete recoverability is claimed.
Audience: implementer who needs exact seams, ownership rules, state machines and test gates.

## Read order

| # | File | Answers |
| --- | --- | --- |
| 1 | [01-hardware-partition.md](01-hardware-partition.md) | What runs on host, what runs on device microcode, reset/init/DMA/IRQ/calibration boundaries |
| 2 | [02-binary-abi.md](02-binary-abi.md) | Frozen blob ABI, exact call signatures, offset seams, ownership and error rules |
| 3 | [03-rx-tx-framing.md](03-rx-tx-framing.md) | d11 prefix, PHY bytes, padding, FCS, validation, skb ownership |
| 4 | [04-sae-state-machine.md](04-sae-state-machine.md) | Per-attempt SAE lifecycle, statuses, assoc IEs, retries, cancellation, roaming |
| 5 | [05-pmf-contracts.md](05-pmf-contracts.md) | BIP/CMAC, MMIE, CCMP, PN/IPN, key lifetimes, SA Query, teardown |
| 6 | [06-rust-implementation.md](06-rust-implementation.md) | no_std layout, kernel interfaces, locks, epochs, zeroization, acceptance |
| 7 | [07-verification.md](07-verification.md) | Vectors, malformed inputs, replay/rekey/concurrency, live behavior |
| 8 | [08-limits-prerequisites.md](08-limits-prerequisites.md) | Microcode, board data, unsupported variants, inferred types, kernel ABI |

## Two implementation modes

| Mode | Description | Blob required | State |
| --- | --- | --- | --- |
| A. Compat ABI | Host Rust drives the frozen vendor object through the seams in file 02 | Yes, exact artifact only | Implementable now, board-dependent |
| B. Native MAC | Host Rust implements MAC/PHY/DMA directly, b43-style | No | Research path; prerequisites in file 08 |

Mode A is a bridge. Mode B is the target. Both use the same PMF and SAE transport contracts.

## Invariants

- SAE mathematics runs in userspace. The kernel never sees a password, PWE, scalar or SAE state.
- PMF is host software. Do not assume device crypto for management frames.
- No downgrade: a PMF-required non-SAE join is refused; an SAE join that associates without SAE is refused.
- Replay state advances before any side effect.
- MIC comparison is constant time.
- Every consumed RX packet is freed exactly once. Every queued TX packet transfers ownership exactly once.
- ABI offsets are compatibility seams, not Rust structure layouts.
- Board calibration and microcode are inputs, not constants.

## Source map

| Area | Relative source |
| --- | --- |
| SAE bridge, PMF, SA Query | `wl-src/src/wl/sys/wl_cfg80211_hybrid.c` |
| PMF primitives | `wl-src/src/wl/sys/wl_pmf.h` |
| Packet-free bridge, PCI, IRQ, TX/RX plumbing | `wl-src/src/wl/sys/wl_linux.c`, `wl_linux.h` |
| 802.11 protocol constants | `wl-src/src/common/include/proto/802.11.h` |
| Binary patch seams | `patches/wl_patch.py`, `patches/wl_reloc.py` |
| Host vectors | `tests/pmf.c`, `tests/rx.c`, `tests/rx.py` |
| Trial portability constraints | `recovery/`, `dkms/` |

## External references

- Linux Rust kernel policy: https://docs.kernel.org/rust/general-information.html
- BCM4360 MAC/DMA fixes: https://lists.openwall.net/linux-kernel/2026/10/14/1949
- BCM4360 core revision 40/42 MAC setup: https://lkml.iu.edu/hypermail/linux/kernel/2611.1/16345.html
- BCM4360 AC microcode TX/RX formats: https://www.mail-archive.com/b43-dev%40lists.infradead.org/msg05065.html
- BCM4360 AC-PHY support: https://lists.openwall.net/linux-kernel/2026/11/12/3436
- IEEE 802.11-2020: clauses 12 (WEP/CCMP), 13 (BIP), 11.13 (SA Query)
- RFC 4493: AES-CMAC
- RFC 3610: CCM

## Non-goals

- Reconstructing vendor source or copying vendor bytes.
- Reimplementing wpa_supplicant, SAE, the 4-way handshake or RSN parsing.
- Defining a register map not backed by core/board data.
- Claiming suspend, power-save, hardware crypto or TX calibration work.
