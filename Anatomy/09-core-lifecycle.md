# 09. Core lifecycle contracts

Scope: host-side attach, reset and AC-PHY dependencies.
Binary offsets apply only to the [frozen artifact](02-binary-abi.md).
Native implementations use subsystem contracts, not these addresses or object layouts.

## Entry points

| Symbol | ELF `.text` offset | Role |
| --- | --- | --- |
| `wlc_bmac_attach` | `0x6984f` | Bus/MAC attachment and hardware dependencies |
| `wlc_phy_attach_acphy` | `0xa194f` | AC-PHY private state and board parameters |
| `wlc_reset` | `0x3fca4` | Host-coordinated MAC reset |
| `wlc_sendauth` | `0x40e02` | Authentication frame construction and queue submission |
| `wlc_authresp_client` | `0x5ee28` | Legacy client authentication response handling |

Offsets are section-relative.
An analysis load address is not a linker offset.
Resolve symbols through ELF metadata; never copy a displayed address into a call target.

## Bus/MAC attach dependencies

This table groups dependencies.
It does not prescribe a register sequence.

| Area | Compatibility symbols | Native contract |
| --- | --- | --- |
| Chip and bus identity | `wlc_chipmatch`, `wlc_is_singleband_5g`, `si_chip_hostif`, `si_coreidx`, `si_corerev`, `si_setcore` | Discover bus, core identity and revision before selecting register definitions |
| Device access | `wlc_bmac_validate_chip_access` | Check mapped hardware access before arming DMA or interrupts |
| Clock, regulator and reset | `si_clkctl_init`, `si_pmu_rfldo`, `wlc_bmac_xtal`, `wlc_bmac_pllreq`, `wlc_bmac_corereset`, `wlc_coredisable` | Preserve clock and RF-LDO prerequisites; unwind them only after device activity stops |
| PCIe policy | `si_pci_war16165`, `si_pcie_ltr_war`, `si_pcieobffenable`, `si_pcieltrenable`, `si_pcie_get_request_size` | Apply revision-specific bus workarounds from an established implementation |
| DMA | `dma_addrwidth`, `dma_attach`, `wl_alloc_dma_resources`, `wlc_hw_set_di` | Negotiate supported DMA addressing; allocate and map rings; assign queue ownership |
| Interrupt callbacks | `si_register_intr_callback` | Serialize core-window changes with interrupt handling |
| PHY integration | `wlc_phy_shim_attach`, `wlc_phy_shared_attach`, `wlc_phy_attach` | Bind PHY services to the selected MAC/core instance |
| Capabilities | `wlc_phy_get_phyversion`, `wlc_phy_get_encore`, `wlc_phy_get_coreflags`, `wlc_phy_machwcap_set` | Derive capabilities from hardware and board configuration |
| Queue and aggregation setup | `wlc_bmac_ampdu_set`, `wlc_template_cfg_init`, `wlc_hw_attach` | Initialize per-core MAC resources before advertising usable queues |
| Indicators | `wlc_bmac_led_attach` | Bind LED behavior to the selected core and board description |
| Coexistence and board switching | `wlc_bmac_btc_wire_set`, `wlc_bmac_btc_mode_set`, `wlc_bmac_set_btswitch`, `wlc_setxband` | Configure only wiring described by the board |
| Board variables | `getvar`, `getintvar`, `bcm_strtoul`, `bcm_ether_atoe`, `wlc_tunables_override` | Parse typed values with explicit defaults, ranges and malformed-input errors |

DMA descriptor geometry belongs to the MAC/DMA implementation.
Calibration values belong to the board.
Neither substitutes for the other.

Do not translate a call graph directly into a register programming sequence.
Branch conditions, clock state, revision gates and unwind behavior are part of the implementation.
Use the public b43/BCMA definitions for the exact supported core revision.

## Reset sequence

The compatibility reset coordinates host MAC state and hardware reset.
It is more than toggling the PCI device.

| Order | Operation | Condition |
| --- | --- | --- |
| 1 | Clear reset-related transient host state | Always |
| 2 | Run the power-management branch: clear PM state, update TBTT handling | Enclosing host flag set and a PM context present |
| 3 | Delay `4000` microseconds | Enclosing power-management branch |
| 4 | Update counters through `wlc_statsupd` | Always |
| 5 | Clear the associated `0x80`-byte counter/state block | Always |
| 6 | Stop offload through `wlc_ol_down` | Offload enabled and active |
| 7 | Flush host A-MSDU aggregation through `wlc_amsdu_agg_flush` | Always |
| 8 | Reset MAC hardware through `wlc_bmac_reset` | Always |
| 9 | Release the applicable PHY PLL request through `wlc_bmac_core_phypll_ctl(..., 0)` | Revision and PHY condition |
| 10 | Reset host A-MPDU TX state through `wlc_ampdu_tx_reset` | Always |
| 11 | Clear final host reset state | Always |

The conditional delay is not a universal reset timeout.
The sequence above does not define the internal register writes of `wlc_bmac_reset`.

Native recovery contract:

1. Stop new submissions.
2. Prevent RX/TX callbacks from using the old session.
3. Quiesce DMA and synchronize interrupt/deferred work.
4. Dispose of aggregation, queue and key state under their ownership rules.
5. Apply the core-specific reset sequence.
6. Reinitialize rings, firmware-facing state and capabilities.
7. Report link loss; create a new association attempt.

Every wait needs a bounded timeout and an error path.
After a failed reset, do not reuse DMA mappings or station pointers whose lifetime ended.

## AC-PHY attach contract

The compatibility function allocates a `0x920`-byte private AC-PHY state block and clears it
before initialization.
An allocation failure returns a null result before the block is initialized.
This size is an artifact constant, not a Rust layout or a wire format.

Dependencies:

| Function group | Role |
| --- | --- |
| `phy_reg_read` | Read PHY state using the established register-access layer |
| `si_alp_clock` | Obtain the reference clock |
| `si_get_sromctl`, `si_set_sromctl`, `otp_read_word` | Access board/OTP configuration with the required access-state handling |
| `phy_getvar_fabid` | Select fabrication-dependent configuration |
| `getintvararray`, `phy_getintvararray`, `phy_getintvar` | Read board parameters and per-chain arrays |
| `wlc_phy_txpwr_srom11_read` | Decode SROM revision 11 TX-power parameters |
| `wlc_phy_hirssi_elnabypass_init_acphy` | Configure high-RSSI external-LNA bypass behavior |
| `wlc_phy_btc_adjust_acphy` | Apply the board's Bluetooth-coexistence adjustment |
| `wlc_phy_hwaci_init_acphy` | Initialize hardware adjacent-channel interference handling |

Attach is not equivalent to complete TX calibration.
Table loading, channel switching and calibration need their own revision-specific contracts.

## Board parameter families

Names below are board-variable keys, not configuration values to hardcode.

| Family | Keys | Meaning |
| --- | --- | --- |
| Board identity | `vendid`, `devid`, `boardrev`, `sromrev`, `macaddr`, `boardflags`, `boardflags2` | Identity and capability flags read before the board description is trusted |
| RF front end | `swctrlmap_2g`, `swctrlmap_5g`, `antswctl2g`, `antswctl5g`, `femctrl`, `boardflags3` | Board wiring, antenna switching and front-end policy |
| External PA | `extpagain2g`, `extpagain5g`, `pdgain2g`, `pdgain5g` | Band-specific gain parameters |
| Subband handling | `subband5gver` | 5 GHz subband parameter interpretation |
| RX phase calibration | `rpcal2g`, `rpcal5gb0` through `rpcal5gb3` | Band and subband calibration inputs |
| TX limits | `txidxcap2g`, `txidxcap5g` | Band-specific transmit-index caps |
| Digital filtering | `cckdigfilttype` | CCK filter selection |
| RX gain and isolation | `rxgains2gelnagaina%d`, `rxgains2gtrelnabypa%d`, `rxgains2gtrisoa%d` and 5 GHz variants | Per-chain LNA, bypass and isolation parameters |
| Gain correction | `rxgainerr2ga0` through `rxgainerr2ga2`; `rxgainerr5ga*` | Per-chain and band gain error inputs |
| Noise and temperature | `noiselvl2ga*`, `noiselvl5ga*`, `rawtempsense` | Noise-floor and temperature calibration inputs |

Parse the complete format of each selected key from the applicable board-data implementation.
Validate array length against the active chain count.
Keep absent, malformed and intentionally zero values distinct.
Never substitute another board's values because the PCI device ID matches.

## Rust reconstruction boundary

| Layer | Reuse | Implement |
| --- | --- | --- |
| PCI/BCMA | Kernel bus/resource/DMA interfaces | Supported identity and core-revision selection |
| MAC | mac80211/cfg80211 | Device queue, status, filter and channel operations |
| PHY/radio | Public b43 AC-PHY and radio definitions | Revision-gated transport and board calibration handling |
| Firmware | Kernel firmware loading | Image compatibility checks and device load/start protocol |
| Security | mac80211, wpa_supplicant and kernel crypto | Correct capabilities, metadata and offload flags |
| Recovery | Kernel IRQ/work synchronization primitives | Bounded stop/reset/restart and session invalidation |

Firmware bytes remain a separate dependency.
A fully open host driver does not imply open D11 microcode.
A Rust wrapper around the vendor host object remains a compatibility bridge.

## Acceptance contracts

- Attach failure unwinds every completed allocation, mapping, callback registration and clock request.
- Reset cannot leave DMA writing into released memory.
- Repeated reset does not leak queue, aggregation or firmware resources.
- No PHY capability is advertised before the matching board/core path is usable.
- A partial calibration result cannot authorize transmission outside board limits.
- Link or session state from before reset cannot complete a later join.
- Firmware, board data and register definitions are independently versioned inputs.
- Device removal synchronizes all callbacks before releasing the adapter.

## Public implementation references

- [BCM4360 DMA/MAC work](https://lists.openwall.net/linux-kernel/2026/10/14/1949)
- [Core revision 40/42 MAC setup](https://lkml.iu.edu/hypermail/linux/kernel/2611.1/16345.html)
- [AC microcode TX/RX interface](https://www.mail-archive.com/b43-dev%40lists.infradead.org/msg05065.html)
- [AC-PHY/radio 2069 support](https://lists.openwall.net/linux-kernel/2026/11/12/3436)
