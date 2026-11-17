# 08. Limits and prerequisites

What is required, what is proven, and what each mode still lacks.

## Status

- The shipping driver is Mode A in C. It is the only mode with live evidence.
- Mode A works on 2.4 GHz and 5 GHz and inherits upstream suspend support.
- Mode B is a native-port specification. No Rust driver is implemented.
- No complete recoverability is claimed.

## Required inputs

| Input | Needed by | Source | If missing |
| --- | --- | --- | --- |
| Microcode image | A and B | device or vendor package | radio does not run |
| Board SROM/OTP/NVRAM | A and B | device or board data | no MAC address, no calibration |
| AC-PHY and radio init tables | B | board data | radio deaf or unstable |
| TX power and PA tables | B | board data | transmit undefined |
| Regulatory limits | A and B | host regdb plus board data | illegal or clamped operation |
| Core revisions | B | device probe | register bases unknown |
| DMA ring layout | B | board data or public b43 work | TX/RX cannot start |
| Exact artifact hash | A | the one supported build | refuse to load |

Microcode and calibration are inputs, not constants. Do not embed them in the repository.

## Compat limitations (Mode A, current C driver)

| Area | Status |
| --- | --- |
| 2.4 GHz and 5 GHz | works |
| Suspend/resume | inherited from upstream; reconnect behavior is part of the live record |
| WPA3-SAE with PMF | works on the tested AP and board |
| WPA2-PSK with PMF | refused; this object builds its own PSK RSN element without PMF |
| Hardware crypto for management | not used; management protection is host software |
| Hardware crypto for data | object-managed; not a management dependency |
| Other BCM4360 boards | untested |
| Other artifact builds | rejected by the hash gate |
| Monitor mode, injection, AP, mesh, IBSS | out of scope |
| 6 GHz, VHT80+, MU-MIMO, beamforming | unsupported |
| Power save, ASPM, WoWLAN | unsupported or not enabled by default |
| SAE-PK, FILS, OWE, 802.11r | out of scope |
| Sustained throughput | not claimed by this specification; measure before publishing |

The current driver is not "unsupported hardware". It has a working 5 GHz path and upstream suspend
support. Those capabilities belong to Mode A and must not be projected onto a future native port.

## Native limitations (Mode B, not implemented)

| Area | Status |
| --- | --- |
| MAC, PHY, DMA, interrupts | new code required |
| Microcode load and status | new code required |
| Board calibration and TX power | new code required |
| 5 GHz | must be implemented; not inherited |
| Suspend/resume | must be implemented or explicitly reported as link loss; not inherited |
| PMF and SAE transport | should come from mac80211/cfg80211, not a new stack |
| Performance | unknown |

A native port that lacks 5 GHz or suspend is a limitation of that port, not evidence that the
current driver lacks them.

## Hardware init basis

Do not invent a register map or a reset sequence. A native port starts from the public b43 work:

| Need | Public basis |
| --- | --- |
| Core rev 40/42 MAC setup, 64-bit DMA, MAC/PHY fixes | b43 series in the external references |
| AC-PHY and radio 2069 attach | b43 AC-PHY series |
| AC microcode TX/RX formats | b43 AC microcode series |
| Board calibration and SROM/OTP parsing | board data plus existing b43 board code |

Where that work is incomplete, state the gap. Do not fill it with a guessed register write.

## Inferred types

| Name | Assumed | Risk |
| --- | --- | --- |
| `int` | 32-bit signed | low on x86-64 |
| `u16` | 16-bit unsigned | low |
| `u32` | 32-bit unsigned | low |
| `u64` | 64-bit unsigned | low |
| pointer | 64-bit | low |
| struct padding | compiler default | medium; offsets are observed, not derived |
| endianness | little-endian | fixed by target |

Offsets in file 02 are observed bytes, not a C layout. Do not compute them from a reconstructed
struct.

## Kernel ABI differences

| Surface | Risk |
| --- | --- |
| `cfg80211_ops` | fields added or renamed across versions |
| `sk_buff` helpers | linearity and headroom behavior |
| crypto API (`crypto_aead`, `crypto_shash`) | algorithm names and async flags |
| workqueue API | cancel semantics |
| PCI API | BAR mapping and DMA mask |
| IRQ API | shared handler return values |
| `net_device` | registration and stats |
| module loading | symbol visibility |

Pin one kernel range and test at both ends. Do not assume a newer kernel accepts the same ops
struct.

The current candidate is built for `7.2.15-1-omarchy`. A built module is bound to that kernel's
`vermagic`. Rebuild for any other kernel before preparing a trial bundle.

## Regulatory constraints

- Channel and power come from board data plus the host regulatory database.
- Do not hardcode a world domain.
- Do not bypass a regulatory clamp to make a link work.
- A channel not permitted by both board data and regdb is not usable.
- Country changes may force a reconnect.
- Passive scan rules apply where the domain requires them.

## Mode A prerequisites

1. Exact artifact hash matches.
2. The artifact links against the target kernel.
3. Required symbols are globalized and the redirect set applies cleanly.
4. The auth clamp passthrough is applied at the validated offset.
5. PCI probe, BAR map and IRQ work.
6. The packet free callback is wired.
7. Management TX and RX taps are installed.
8. Userspace SAE and RSN are available.
9. Board data is present for the installed card.

Missing any one: Mode A does not run.

## Mode B prerequisites

1. Core revisions identified for the exact board.
2. Microcode load protocol and status decoding known.
3. SROM/OTP layout known, including MAC address and calibration sections.
4. DMA descriptor layout, ring sizes and index/status addresses known.
5. AC-PHY and radio init tables known.
6. TX calibration and power tables known.
7. Regulatory mapping known.
8. Reset and clock/GPIO sequences known.

Mode B cannot start from this document alone.

## Correctness risks

| Risk | Effect | Mitigation |
| --- | --- | --- |
| Wrong offset | memory corruption | hash gate plus checked accessors |
| Wrong ownership | double free or leak | one-owner rule plus fault injection |
| Missing lock | race or crash | lock order plus lockdep |
| Stale pointer after teardown | use after free | epoch plus clear on teardown |
| Wrong replay floor | accept replay or drop valid | table-driven tests |
| Non-constant-time MIC | side channel | constant-time primitive |
| Stale association IE | wrong AKM in assoc | clear before teardown |
| Silent downgrade | security loss | fail-closed policy |
| Uncalibrated radio | deaf or illegal TX | require board data |
| Wrong microcode | dead or unstable MAC | version check |

## Open items

- No complete register map.
- No proven microcode load sequence for a native port.
- No native TX calibration.
- No native 5 GHz or suspend implementation.
- No hardware-crypto proof for management frames, because management crypto is host software.
- No multi-board validation.
- No regulatory certification.
- No on-air security or sustained-throughput claim.
