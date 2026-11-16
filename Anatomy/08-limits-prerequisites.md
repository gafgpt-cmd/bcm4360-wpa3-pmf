# 08. Limits and prerequisites

What is not known, not supported, and required before code can work.

## Status

- Specification only.
- No complete recoverability is claimed.
- No working Rust driver is claimed.
- Mode A is a compatibility bridge for one artifact.
- Mode B is a research path with open prerequisites.

## Required inputs

| Input | Needed by | Source | If missing |
| --- | --- | --- | --- |
| Microcode image | A and B | board/vendor package | radio does not run |
| Board SROM/OTP/NVRAM | A and B | device or vendor data | no MAC address, no calibration |
| PHY/radio init tables | B | board data | radio deaf or unstable |
| TX power/PA tables | B | board data | transmit undefined |
| Regulatory limits | A and B | host regdb plus board data | illegal or clamped operation |
| Core revisions | B | device probe | register bases unknown |
| DMA ring layout | B | board data or reference | TX/RX cannot start |
| Exact artifact hash | A | the one supported build | refuse to load |

Microcode and calibration are inputs, not constants. Do not embed them in the repository.

## Unsupported or unverified

| Area | Status |
| --- | --- |
| Other BCM4360 revisions | unverified |
| Sibling device IDs | unsupported unless listed |
| 5 GHz operation | unverified; depends on board and regulatory data |
| VHT80 / wider channels | unverified |
| 80+80, MU-MIMO, beamforming | unsupported |
| Power save | unsupported |
| Suspend/resume | unsupported; must fail safely |
| ASPM | do not enable |
| WoWLAN | unsupported |
| Hardware crypto for management | not used |
| Hardware crypto for data | assumed only; not proven |
| TX calibration | open prerequisite for Mode B |
| Antenna diversity | unverified |
| Bluetooth coexistence | unsupported |
| Monitor mode / injection | unsupported |
| AP / mesh / IBSS | out of scope |
| Fast roaming (802.11r) | out of scope |
| FILS | out of scope |
| SAE-PK | out of scope |
| OWE | out of scope |
| 6 GHz | unsupported |

Absence from this table is not support. It means untested.

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

Offsets in file 02 are observed bytes, not a C layout. Do not compute them from a reconstructed struct.

## Unresolved indirect targets

| Target | Why it matters |
| --- | --- |
| Blob internal functions | a relink or rebuild can move them |
| Return thunks / PLT stubs | a wrong redirect can hard-lock |
| Auth state advancer | only one known address |
| Association builder | depends on internal state and IE layout |
| Packet free path | ownership mismatch corrupts the heap |
| TX completion path | null station context crashes |

The bridge is valid only for the exact artifact and the exact redirect set. Any change invalidates it.

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

Pin a kernel range and test at both ends. Do not assume a newer kernel accepts the same ops struct.

## Regulatory constraints

- Channel and power come from board data plus the host regulatory database.
- Do not hardcode a world domain.
- Do not bypass a regulatory clamp to make a link work.
- A channel not permitted by both board data and regdb is not usable.
- Country changes may force a reconnect.
- Passive scan rules apply where the domain requires them.

## Mode A prerequisites

1. Exact artifact hash matches.
2. Artifact loads and links against the target kernel.
3. Required symbols are globalized and the redirect set applies cleanly.
4. Auth clamp passthrough applied at the validated offset.
5. PCI probe, BAR map and IRQ work.
6. Packet free callback is wired.
7. Management TX/RX taps are installed.
8. Userspace SAE and RSN are available.
9. Board data is present for the installed card.

Missing any one: Mode A does not run.

## Mode B prerequisites

1. Core revisions identified for the exact board.
2. Microcode load protocol and status decoding known.
3. SROM/OTP layout known, including MAC address and calibration sections.
4. DMA descriptor layout, ring sizes and index/status addresses known.
5. PHY/radio init tables known.
6. TX calibration and power tables known.
7. Regulatory mapping known.
8. Reset and clock/GPIO sequences known.

Mode B cannot start from this document alone.

## Correctness risks

| Risk | Effect | Mitigation |
| --- | --- | --- |
| Wrong offset | memory corruption | hash gate plus checked accessors |
| Wrong ownership | double free or leak | one-owner rule plus fault injection |
| Missing lock | race, crash | lock order plus lockdep |
| Stale pointer after teardown | use after free | epoch plus clear on teardown |
| Wrong replay floor | accept replay or drop valid | table-driven tests |
| Non-constant-time MIC | side channel | constant-time primitive |
| Stale association IE | wrong AKM in assoc | clear before teardown |
| Silent downgrade | security loss | fail-closed policy |
| Uncalibrated radio | deaf or illegal TX | require board data |
| Wrong microcode | dead or unstable MAC | version check |

## Known gaps

- No complete register map.
- No proven microcode load sequence for this board.
- No proven TX calibration.
- No power-save or suspend support.
- No hardware-crypto proof for management frames.
- No multi-board validation.
- No regulatory certification.
- No performance characterization.
- No long-duration stability claim.

## Prerequisite checklist

Before writing device code:

- [ ] Exact board identified.
- [ ] Core revisions read and matched.
- [ ] Board data available and parsed.
- [ ] Microcode image available and version-checked.
- [ ] DMA layout documented from data, not guess.
- [ ] PHY tables available.
- [ ] Regulatory source chosen.
- [ ] Kernel range pinned.
- [ ] Artifact hash gate implemented (Mode A).
- [ ] Fail-closed policy encoded.
- [ ] Verification plan from file 07 accepted.

## Honest claims

Supported by this document:

- A precise host-side contract for SAE transport, PMF, RX/TX framing and ownership.
- A hash-gated compatibility seam for one artifact.
- A test plan that can falsify the contracts.

Not supported:

- That the device is fully understood.
- That any artifact can be swapped in.
- That the driver works on all BCM4360 boards.
- That suspend, power save, hardware crypto or TX calibration work.
- That the result is ready for production.
