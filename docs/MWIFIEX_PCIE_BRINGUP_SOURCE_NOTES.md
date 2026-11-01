# mwifiex PCIe bring-up source notes for Marvell 88W8897

This is an internet-backed, source-inspection-only pass for the next runtime bring-up phase. It documents Linux/vendor-source concepts and maps them to the current macOS/OpenCore driver scaffolding without changing runtime behavior.


## Current PR state for reviewers

This branch's final state is passive/manual diagnostic tooling only. The boot-time / normal-bring-up multi-BAR mwifiex snapshot logging that correlated with Surface/OpenCore freezes has been removed and is not part of the current runtime path. Kernel runtime source under `Contents/` is intended to remain equivalent to the validated baseline (`4f74f92` / `origin/main` for this work).

Current operator-facing behavior:

- default `local-test/marvell_probe` prints PCI/BAR/bring-up/transport metadata and performs no explicit BAR MMIO read;
- `local-test/marvell_probe --read-bar-reg <bar> <offset>` remains a separate manual, one-register, read-only action;
- `scripts/surface_passive_bar_snapshot.sh` captures a passive report and does not invoke `--read-bar-reg`;
- passive Surface validation of the snapshot tooling succeeded after replacing a stale/root-owned probe binary;
- the passive snapshot showed BAR0 -> resource0, physical base `0x7f91400000`, length `0x100000`, and BAR2 -> resource1, physical base `0x7f91500000`, length `0x100000`;
- bring-up remains conservative: `unsupported`, `no-grounded-reset-semantic`, `resetAttempted=0`, and `readyObserved=0`.

Current BAR-window interpretation remains inconclusive:

- BAR2/resource1 is structurally interesting because Linux maps PCI region/resource 2 to `pci_mmap1`;
- observed BAR2 sampled values of zero or `0xffffffff` do not prove it is the active mwifiex register window;
- BAR0 sampled values are readable but high-entropy-looking and do not prove semantic mwifiex register state;
- no readiness, firmware state, ring state, interrupt state, reset safety, or write safety should be inferred from either BAR.

## Sources inspected

| Source | Tree / revision | Files inspected | Notes |
| --- | --- | --- | --- |
| Linux upstream via Code Browser | Generated 2026-02-08 from Linux revision `v6.19-rc8-185-g2687c848e578` | `drivers/net/wireless/marvell/mwifiex/pcie.c`, `pcie.h`, `main.c`, `pcie_quirks.c`, `pcie_quirks.h`, `fw.h` | Primary source for PCIe register tables, firmware download, interrupt, ring, reset, and command/event flow. URLs: <https://codebrowser.dev/linux/linux/drivers/net/wireless/marvell/mwifiex/pcie.c.html>, <https://codebrowser.dev/linux/linux/drivers/net/wireless/marvell/mwifiex/pcie.h.html>, <https://codebrowser.dev/linux/linux/drivers/net/wireless/marvell/mwifiex/fw.h.html>. |
| Linux upstream raw source | `torvalds/linux` `master`, fetched 2026-05-11 for line inspection only | Same files as above | Used only for source inspection; no upstream source was committed. URLs use `https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/wireless/marvell/mwifiex/...`. |
| LKDDb | Web database crawled 2026-era | `CONFIG_MWIFIEX_PCIE` page | Confirms Kconfig description and PCI ID naming: <https://cateee.net/lkddb/web-lkddb/MWIFIEX_PCIE.html>. |
| Android/common kernel mirror | `kernel/common` mirror search result, commit `57e177ea01bde322590e5075d23dc4538881b1fa` | `drivers/net/wireless/marvell/mwifiex/pcie.c` | Secondary navigation fallback confirming mirrored mwifiex PCIe source availability: <https://android.googlesource.com/kernel/common/+/57e177ea01bde322590e5075d23dc4538881b1fa/drivers/net/wireless/marvell/mwifiex/pcie.c>. |
| Linux Wireless docs | Current web docs | mwifiex driver overview | Secondary confirmation that mwifiex is a FullMAC SDIO/PCIe/USB Marvell driver family: <https://wireless.docs.kernel.org/en/latest/en/users/drivers/mwifiex.html>. |

No firmware binaries, downloaded source trees, logs, or generated reports were added to this repository.

## External facts verified

- `CONFIG_MWIFIEX_PCIE` is described as the Marvell WiFi-Ex PCIe driver for `8766/8897/8997`, depends on `MWIFIEX && PCI`, and builds `mwifiex_pcie`.
- PCI vendor/device `11ab:2b38` is listed for `88W8897 [AVASTAR] 802.11ac Wireless`.
- Linux `pcie.h` defines `PCIE_DEVICE_ID_MARVELL_88W8897` as `0x2b38` and the default 8897 firmware names as `mrvl/pcie8897_uapsta.bin` plus A0/B0 variants.
- Linux `fw.h` defines `FIRMWARE_READY_PCIE` as `0xfedcba00`.
- Linux `pcie.c` accesses mwifiex PCIe MMIO registers through `card->pci_mmap1 + reg`, where `pci_mmap1` is mapped from PCI region/resource 2. Linux also maps region/resource 0 as `pci_mmap`, but the inspected mwifiex register access helpers use `pci_mmap1`. This is a Linux PCI-resource observation, not proof that the macOS/OpenCore BAR numbering or resource mapping has identical semantics.

## Linux mwifiex PCIe sequence summary

### Probe, PCI enable, BAR map, and profile selection

1. `mwifiex_pcie_probe()` allocates `pcie_service_card`, copies the device-specific `mwifiex_pcie_device` data from the PCI ID table, initializes work, checks platform quirks, then calls the common `mwifiex_add_card()` path.
2. The 88W8897 PCI ID table entry binds Marvell vendor `0x11ab`, device `0x2b38`, to `mwifiex_pcie8897`.
3. `mwifiex_init_pcie()` enables the PCI device, sets bus master, configures a 32-bit DMA mask, requests and maps region 0 into `pci_mmap`, requests and maps region 2 into `pci_mmap1`, allocates PCIe buffers/rings, and enables the 8897 `ignore_btcoex_events` quirk.
4. The Linux register accessors read/write `card->pci_mmap1 + offset`. Therefore Linux's mwifiex register offsets are relative to Linux PCI region/resource 2, even though Linux also maps region/resource 0.

### Register-window mapping source-inspection detail

The inspected upstream Linux path is internally consistent:

- `mwifiex_init_pcie()` maps PCI region/resource 0 into `card->pci_mmap` and PCI region/resource 2 into `card->pci_mmap1`.
- `mwifiex_write_reg()`, `mwifiex_read_reg()`, and `mwifiex_read_reg_byte()` all address `card->pci_mmap1 + reg`.
- The 88W8897 profile table stores the documented scratch, interrupt, and pointer offsets as `reg` values; no extra software offset base or dynamic window-select operation was found in those helpers.
- Consequently, offsets such as `0xC44`, `0xCF0`, `0xC08C`, `0xC05C`, `0xCE8`, and `0xCEC` are intended by Linux as offsets relative to `pci_mmap1`, not `pci_mmap`.
- The macOS/OpenCore BAR report uses BAR0/resource0 and BAR2/resource1, each length `0x100000`. That report resembles Linux's two mapped regions by count and size, but the current manual reads below show that neither macOS BAR0 nor BAR2 is yet proven to be the semantic mwifiex register window.

### 88W8897 register/profile table

Linux `mwifiex_reg_8897` maps these key fields:

- command address low/high: `PCIE_SCRATCH_0_REG` / `PCIE_SCRATCH_1_REG` (`0xC10` / `0xC14`);
- command size: `PCIE_SCRATCH_2_REG` (`0xC40`);
- firmware status: `PCIE_SCRATCH_3_REG` (`0xC44`);
- command response address low/high: `PCIE_SCRATCH_4_REG` / `PCIE_SCRATCH_5_REG` (`0xCD0` / `0xCD4`);
- TX read/write pointer: `PCIE_RD_DATA_PTR_Q0_Q1` / `PCIE_WR_DATA_PTR_Q0_Q1` (`0xC08C` / `0xC05C`);
- RX read/write pointer: same two registers reversed (`0xC05C` / `0xC08C`);
- event read/write pointer: `PCIE_SCRATCH_10_REG` / `PCIE_SCRATCH_11_REG` (`0xCE8` / `0xCEC`);
- driver ready: `PCIE_SCRATCH_12_REG` (`0xCF0`);
- 8897 TX mask/wrap: `0x03FF0000` / `0x07FF0000`;
- 8897 RX mask/wrap: `0x000003FF` / `0x000007FF`;
- PFU descriptor mode enabled;
- sleep cookie disabled for 8897;
- firmware dump control uses scratch 13/14 through `0xCFF`.

### Firmware status / ready semantics

- Firmware-ready value is `FIRMWARE_READY_PCIE = 0xfedcba00`.
- Linux `mwifiex_check_winner_status()` reads `reg->fw_status`; zero means PCIe is the winner, nonzero means this interface is not the winner.
- Linux `mwifiex_check_fw_status()` first writes `HOST_INTR_MASK` to `PCIE_HOST_INT_STATUS_MASK` to mask spurious interrupt status, then writes `FIRMWARE_READY_PCIE` to `reg->drv_rdy`, then polls `reg->fw_status` until it equals `FIRMWARE_READY_PCIE`.
- On cleanup/down paths Linux clears `reg->drv_rdy` to zero when firmware is ready or during PCIe down-device handling.

### Firmware download state machine

Linux firmware programming is a mutating DMA/bootstrap operation and should remain out of scope for the current driver until a deliberate firmware-upload milestone:

1. Disable host interrupts by writing zero to `PCIE_HOST_INT_MASK`.
2. Allocate a DMA buffer sized for upload/download transfer.
3. Read `PCIE_SCRATCH_13_REG`; if it contains `MWIFIEX_PCIE_FLR_HAPPENS` (`0xFEDCBABA`), extract the Wi-Fi part from combo firmware before transfer.
4. Poll `reg->cmd_size` until the helper/boot code publishes a nonzero requested length.
5. If the length has bit 0 set, treat it as a CRC/retry indication, clear the bit, and resend from the same firmware offset up to `MAX_WRITE_IOMEM_RETRY`.
6. Copy the requested firmware block, rounded into `MWIFIEX_PCIE_BLOCK_SIZE_FW_DNLD` (`256`) blocks, into the DMA buffer.
7. Write the DMA physical address to `reg->cmd_addr_lo` / `reg->cmd_addr_hi`, write byte length to `reg->cmd_size`, and ring `PCIE_CPU_INT_EVENT` with `CPU_INTR_DOOR_BELL`.
8. Poll `PCIE_CPU_INT_STATUS` until `CPU_INTR_DOOR_BELL` clears, indicating firmware acknowledged the block.
9. Repeat until all firmware bytes are transferred, then run the firmware-status/driver-ready handshake described above.

### Interrupt setup and interrupt handling

- Interrupt bits are `HOST_INTR_DNLD_DONE`, `HOST_INTR_UPLD_RDY`, `HOST_INTR_CMD_DONE`, and `HOST_INTR_EVENT_RDY`; `HOST_INTR_MASK` is their OR.
- Linux keeps host interrupts disabled during firmware download and only enables them by writing `HOST_INTR_MASK` to `PCIE_HOST_INT_MASK` after the stack is ready.
- The interrupt handler reads `PCIE_HOST_INT_STATUS`, ignores `0xffffffff` or zero, disables host interrupts for shared INTx, acknowledges pending bits by writing `~pcie_ireg` to `PCIE_HOST_INT_STATUS`, and later dispatches work based on the saved status bits.
- The processing path maps `DNLD_DONE` to TX completion, `UPLD_RDY` to RX data processing, `EVENT_RDY` to event processing, and `CMD_DONE` to command response processing.
- Reading `PCIE_HOST_INT_STATUS` is read-only, but acknowledging it is a mutating clear. Enabling interrupts is unsafe until rings, command response buffers, event buffers, and dispatch are implemented.

### Ring setup, descriptors, and pointer semantics

- Linux allocates coherent TX, RX, event, command-response, and optional sleep-cookie buffers before firmware setup completes.
- 88W8897 uses PFU descriptors (`mwifiex_pfu_buf_desc`) with flags/offset/fragment length/length/physical address fields.
- TX and RX rings each have `MWIFIEX_MAX_TXRX_BD = 0x20` entries; event ring has `MWIFIEX_MAX_EVT_BD = 0x08` entries.
- Linux comments state that, for RX and event rings, the driver maintains the read pointer and firmware maintains the write pointer; read pointers start at zero with rollover indication set.
- For TX, the driver writes the TX write pointer register after preparing descriptors; for 8897/8997 it advances by `reg->ring_tx_start_ptr` (`BIT(16)`) rather than by one.
- Linux combines TX and RX pointer fields in shared registers: writes to TX write pointer include the current RX wrap value, and writes to RX read pointer include the current TX wrap value.
- `mwifiex_pcie_init_fw_port()` writes the initial RX read pointer to `reg->rx_rdptr` before normal RX processing.

### Command/event flow

- Boot firmware blocks and normal commands both use command-address scratch registers, command-size scratch register, and `CPU_INTR_DOOR_BELL`.
- For normal commands, Linux also programs the command-response DMA buffer into `reg->cmdrsp_addr_lo` / `reg->cmdrsp_addr_hi` before ringing the doorbell.
- Event and command completion are interrupt-driven after host interrupts are enabled. Event completion writes updated event read pointer state and signals `CPU_INTR_EVENT_DONE` where applicable.
- Network activation is downstream of firmware initialization, command/event processing, BSS setup, and Linux netdev/cfg80211 registration; it is not part of the PCIe bootstrap itself.

### Reset semantics

- The main reset path is Linux PCI function-level reset via `pci_try_reset_function()`, not a scratch-register-only reset sequence.
- The PCI error-handler callbacks perform pre-reset software teardown, then post-reset firmware/software reinitialization.
- On selected Microsoft Surface systems, Linux applies a D3cold power-cycle quirk because FLR alone is not effective. The quirk power-cycles both the Wi-Fi function and upstream bridge before re-enabling D0.
- `CPU_INTR_RESET` is defined in `pcie.h`, but the inspected upstream path does not use it as the primary runtime card-reset mechanism.

## Register and concept mapping table

| Linux name | Offset/value | Meaning in Linux | Current repo equivalent | Current implementation status | Safety class |
| --- | ---: | --- | --- | --- | --- |
| `PCIE_VENDOR_ID_MARVELL` / `PCIE_DEVICE_ID_MARVELL_88W8897` | `0x11ab` / `0x2b38` | PCI identity for 88W8897 | `kVendorIdMarvell88W8897`, `kDeviceIdMarvell88W8897` in `MarvellW8897Transport.cpp` | Matches Linux and LKDDb | read-only |
| `PCIE8897_DEFAULT_FW_NAME` | `mrvl/pcie8897_uapsta.bin` | Default 8897 firmware path; A0/B0 variants also exist | `kPcie8897DefaultFirmwareName` | Matches default; repo does not model A0 filename selection | documentation / future firmware upload |
| Linux BAR/region access | registers use `pci_mmap1`, mapped from region 2 | MMIO target for mwifiex register offsets | Current bring-up hardcodes BAR index 0 | **Gap:** current read-only sanity may be using BAR0, whereas Linux uses BAR/region 2 for these offsets | read-only validation gap |
| `PCIE_SCRATCH_3_REG` / `reg->fw_status` | `0xC44` | Firmware status / winner / ready polling | `firmwareStatusReg = 0x0C44` | Matches Linux 8897 profile | read-only until ready handshake |
| `FIRMWARE_READY_PCIE` | `0xfedcba00` | Firmware-ready status and driver-ready signature | No named constant in repo runtime path | Missing symbolic constant; no behavior change in this pass | read-only / mutating handshake later |
| `PCIE_SCRATCH_12_REG` / `reg->drv_rdy` | `0xCF0` | Driver ready signature; Linux writes ready value after firmware download and clears on down/cleanup | `driverReadyReg = 0x0CF0`; current run reads only | Offset matches; writes intentionally deferred | mutating reset/ready handshake |
| `PCIE_HOST_INT_STATUS` | `0xC30` | Read pending host interrupt bits | No current profile field | Could be read-only sampled; no ack writes yet | read-only if not acknowledged |
| `PCIE_HOST_INT_MASK` | `0xC34` | Host interrupt enable/mask register | No current profile field | Must remain disabled until interrupt/rings implemented | interrupt |
| `PCIE_HOST_INT_STATUS_MASK` | `0xC3C` | Used before driver-ready write to mask spurious statuses | No current profile field | Future handshake dependency | mutating interrupt/status mask |
| `HOST_INTR_MASK` | `0x0000000f` | DNLD done, UPLD ready, CMD done, EVENT ready | No current profile field | Future interrupt enable value only | interrupt |
| `PCIE_CPU_INT_EVENT` | `0xC18` | Doorbell/write event to firmware/CPU | No current profile field | No writes implemented | firmware upload / command / event |
| `PCIE_CPU_INT_STATUS` | `0xC1C` | Polled during firmware download for doorbell ACK | No current profile field | No firmware download implemented | read-only during firmware-upload milestone |
| `CPU_INTR_DOOR_BELL` | `BIT(1)` / `0x2` | Signal command/firmware block ready | No current profile field | Must remain disabled until DMA command path exists | firmware upload / command |
| `CPU_INTR_DNLD_RDY` | `BIT(0)` / `0x1` | Signal TX download ready | No current profile field | Must remain disabled until TX ring path exists | ring/network |
| `CPU_INTR_EVENT_DONE` | `BIT(5)` / `0x20` | Event completion signal | No current profile field | Must remain disabled until event path exists | command/event |
| `PCIE_RD_DATA_PTR_Q0_Q1` | `0xC08C` | 8897 TX read pointer and RX write pointer register | `txReadPointerReg`, `rxWritePointerReg` | Matches Linux 8897 profile | read-only until ring setup |
| `PCIE_WR_DATA_PTR_Q0_Q1` | `0xC05C` | 8897 TX write pointer and RX read pointer register | `txWritePointerReg`, `rxReadPointerReg` | Matches Linux 8897 profile | read-only until ring setup; writes are ring mutating |
| `PCIE_SCRATCH_10_REG` / `PCIE_SCRATCH_11_REG` | `0xCE8` / `0xCEC` | Event read/write pointers | `eventReadPointerReg`, `eventWritePointerReg` | Matches Linux 8897 profile | read-only until event ring setup |
| 8897 TX mask/wrap | `0x03FF0000` / `0x07FF0000` | TX pointer field/wrap extraction | `txRingMask`, `txRingWrapMask` | Matches Linux 8897 profile | ring/network |
| 8897 RX mask/wrap | `0x000003FF` / `0x000007FF` | RX pointer field/wrap extraction | `rxRingMask`, `rxRingWrapMask` | Matches Linux 8897 profile | ring/network |
| `MWIFIEX_MAX_TXRX_BD` | `0x20` | TX/RX ring entries | Not modeled | Future DMA/ring implementation | ring/network |
| `MWIFIEX_MAX_EVT_BD` | `0x08` | Event ring entries | Not modeled | Future DMA/event implementation | command/event |
| `mwifiex_pfu_buf_desc` | packed 20-byte logical layout | PFU descriptors used by 8897 | Not modeled | Future DMA descriptor implementation | ring/network |
| 8897 `pfu_enabled` | `1` | Selects PFU descriptor format | Not modeled | Future DMA descriptor implementation | ring/network |
| 8897 `sleep_cookie` | `0` | No sleep-cookie buffer for 8897 | Not modeled; current state does not use cookie | Acceptable for current read-only pass | read-only |
| `ignore_btcoex_events` | true for 88W8897 | Linux suppresses BT coexistence events for 8897 | `kMarvellW8897TransportQuirkIgnoreBtcoexEvents` | Matches Linux quirk intent | command/event later |
| 8897 firmware revision select | write/read `0x0C58`, mask `0xff00`; A0 `0x1100`, B0 `0x1200` | Chooses A0/B0/default firmware name | Not modeled; default only | **Gap:** future firmware loader should inspect revision safely before choosing A0/default | mutating read-selection write |
| Reset path | `pci_try_reset_function()`; Surface quirk D3cold cycle | Recovery/reset after hangs, not initial read-only bring-up | Current bring-up reports `no-grounded-reset-semantic` and does not reset | Correctly conservative | mutating reset |

## Comparison with current macOS driver abstractions

### What matches Linux/vendor sources

- The current transport profile matches Linux's 88W8897 PCI ID, firmware-status register, driver-ready register, TX/RX/event pointer registers, TX/RX masks, TX/RX wrap masks, default firmware filename, and ignore-BT-coexistence-events quirk.
- The current bring-up path reads profile registers and reports an unsupported/no-grounded-reset-semantic state instead of attempting an ungrounded reset.
- The current user-client ABI can expose PCI identity, BAR information, bring-up result, and transport state without requiring firmware upload, interrupt enablement, rings, or network bring-up.

### Important gaps and corrections from source inspection

1. **macOS BAR2 accessibility remains unresolved.** Linux maps both region/resource 0 and region/resource 2, and mwifiex register accessors use `pci_mmap1`, the region-2 mapping. A later live Ubuntu read confirms the physical card's active BAR2 contains the firmware-ready registers. The earlier macOS/OpenCore BAR2 zero/invalid reads still need explanation from PCI power, decode, or mapping state before interpreting firmware writes.
2. **Driver-ready writes are not a reset.** Linux writes `0xfedcba00` to `drv_rdy` only as part of the post-firmware-download ready handshake, after masking spurious status bits. It also clears `drv_rdy` during cleanup/down. This supports deferring driver-ready writes until firmware download and interrupt/ring prerequisites are understood.
3. **Reset is a PCI/PM operation, not a scratch-only write.** Upstream uses PCI FLR and Surface-specific D3cold power cycling for recovery. That is much broader than a single MMIO register write and should not be introduced as the first mutating runtime change.
4. **Firmware upload implies DMA and doorbells.** The firmware downloader writes DMA addresses and lengths, rings `CPU_INTR_DOOR_BELL`, and polls `PCIE_CPU_INT_STATUS`. That should not be attempted before a deliberate firmware-loading milestone with rollback and hardware access.
5. **Interrupt enablement depends on rings and dispatch.** Linux interrupts drive TX completion, RX data, event, and command completion. Enabling `HOST_INTR_MASK` without rings and handlers risks unhandled device activity.
6. **Ring pointer writes are coupled.** 8897 shares TX/RX pointer fields across two registers. Linux preserves wrap fields from the opposite direction when writing TX/RX pointers. Any future ring write helper must preserve this coupling.
7. **Firmware name selection has a mutating read step.** Linux writes `0x80c00000` to `0x0c58` before reading revision to choose A0/B0/default firmware. This is not suitable for the immediate read-only phase.

## BAR-window probing safety update

The initial read-only BAR-window diagnostic was intentionally non-mutating, but Surface/OpenCore testing associated the PR-branch artifact (`3CD558C6-684F-39BD-BFBC-9F30750DA511`) with two full-system freezes during or after multi-BAR snapshot validation. The restored baseline/main artifact (`9244A956-7276-399B-9AD4-7C0B6B468D5D`) appeared stable afterward. Later validation of the current branch confirmed that the default `local-test/marvell_probe` path no longer performs the former default `readReg32FromBar(bar=0, off=0)` read, the loaded kext UUID matches the built/deployed UUID, the passive snapshot script completes successfully, and single explicit read-only MMIO reads appear stable so far.

This does **not** prove read-only MMIO caused the freezes, and it does **not** prove any BAR is safe for writes. It is enough evidence to keep boot-time or normal-bring-up multi-BAR MMIO probing disabled/deferred for this target.

Updated runtime policy:

1. No default multi-BAR mwifiex register-window snapshot runs from the normal bring-up path.
2. The conservative bring-up path keeps its existing BAR0 sanity check and final `unsupported` / `no-grounded-reset-semantic` result.
3. BAR/resource window probing remains explicit, manual, and opt-in rather than part of default boot or service startup.
4. Probing should remain one BAR and one selected offset per invocation. Do not script a sweep across every mapped memory BAR.
5. The current branch adds no kernel ABI selector; if a future opt-in probe needs ABI support, it should be proposed separately with a narrow selector, strict bounds, one selected BAR/resource per call, and clear operator warnings.

Why this is safe/minimal:

- It keeps multi-BAR MMIO read sweeps out of default runtime flow.
- It preserves the Linux source-inspection value and register mapping notes without repeating the risky validation pattern by default.
- It keeps future investigation possible as an explicit manual diagnostic step.
- It does not introduce reset, firmware upload, interrupt enablement, rings, command processing, network bring-up, or new MMIO writes.

## Current repo BAR/resource reporting architecture

The current macOS driver exposes enough passive metadata to line up raw PCI BAR slots with IOKit mapped memory resources, but not enough to prove Linux `pci_mmap1` equivalence by identity alone:

- `MarvellW8897PciBars::mapDeviceBars()` reads raw PCI config BAR registers `BAR0..BAR5`, marks 64-bit continuation slots, records raw low/high values, derives memory-vs-I/O and 32-vs-64-bit status, and computes a config BAR base value.
- The same helper separately iterates `IOPCIDevice::getDeviceMemoryCount()` / `mapDeviceMemoryWithIndex(resourceIndex)` and records each mapped IOKit memory resource physical base, virtual mapping, and length.
- It first associates raw BAR slots with IOKit resources by exact physical-base match; if no exact match is found for a present memory BAR, it falls back to the next unclaimed mapped resource.
- `getBarInfoV2` exposes: BAR index, raw low/high config values, combined config value, present bit, memory bit, 64-bit bit, continuation bit, mapped resource index, mapped length, and mapped physical address. It does not expose the kernel virtual address through the user-client ABI.
- `local-test/marvell_probe` now labels these fields in its BAR output. This is passive metadata only and does not add MMIO reads.

Implication for Linux correlation:

- Linux `pci_iomap(pdev, 2, 0)` takes Linux PCI BAR/resource number `2` as used by the kernel PCI API. In the inspected Linux code, that mapped address becomes `card->pci_mmap1`.
- The current Surface report showing macOS BAR2/resource1 with length `0x100000` is therefore structurally interesting, because raw BAR slot 2 is the Linux-requested resource number.
- However, the manual BAR2 reads below returning zero/`0xffffffff` for the sampled offsets mean raw BAR number alone is not sufficient proof of semantic equivalence on this OpenCore/macOS path.
- Evidence strong enough to prove the mapping would include: matching raw BAR physical base and length against Linux `lspci -vv -s <device>` region output on the same hardware/boot state, consistent non-random reads of source-defined status/pointer registers, and ideally a Linux-side register dump for the same cold/warm boot condition. Any such proof should still precede writes, firmware upload, interrupts, or rings.

## Surface manual BAR/register validation snapshot

Current macOS/OpenCore BAR reporting from `local-test/marvell_probe`:

| macOS BAR | Mapped resource | Type | Length | Current note |
| ---: | ---: | --- | ---: | --- |
| BAR0 | resource0 | mapped memory | `0x100000` | physical base `0x7f91400000`; readable via explicit probe; values below are non-trivial but high-entropy-looking |
| BAR2 | resource1 | mapped memory | `0x100000` | physical base `0x7f91500000`; readable for some offsets, but current values are zero or `0xffffffff` for the sampled mwifiex offsets |
| odd BARs | n/a | continuation/non-memory in current report | n/a | not candidates for this register-window question |

Manual explicit reads collected on the Surface/OpenCore machine:

| Offset | Linux meaning | BAR0 value | BAR2 value | Cautious interpretation |
| ---: | --- | ---: | ---: | --- |
| `0xC44` | firmware status / `PCIE_SCRATCH_3_REG` | `0xabc6f99d` | `0x00000000` | BAR0 is readable but not obviously semantic; BAR2 zero does not look like a ready status |
| `0xCF0` | driver ready / `PCIE_SCRATCH_12_REG` | `0x9bd42683` | `0x00000000` | BAR0 is non-zero/high-entropy-looking; BAR2 is zero |
| `0xCE8` | event read pointer / `PCIE_SCRATCH_10_REG` | `0x12432ef8` | `0x00000000` | BAR0 is not an obvious small ring pointer; BAR2 is zero |
| `0xCEC` | event write pointer / `PCIE_SCRATCH_11_REG` | `0xc9250fd8` | `0x00000000` | BAR0 is not an obvious small ring pointer; BAR2 is zero |
| `0xC08C` | TX read / RX write pointer | `0x7d24c3d5` | `0xffffffff` | BAR2 returns the read-failure sentinel pattern for this offset; BAR0 is high-entropy-looking |
| `0xC05C` | TX write / RX read pointer | `0xaa6b1167` | `0xffffffff` | BAR2 returns the read-failure sentinel pattern for this offset; BAR0 is high-entropy-looking |

### Live Ubuntu BAR comparison on this Surface (2026-10-31)

An SSH read-only snapshot with Ubuntu 24.04, kernel `6.10.3-surface-1`, and the bound `mwifiex_pcie` driver found PCI function `03:00.0` in D0 with memory decoding, bus mastering, and MSI enabled. Linux exposes BAR0 at `0xb9500000` and BAR2 at `0xb9400000`, both 1 MiB, 64-bit, and prefetchable. These addresses are allocated by this Ubuntu boot and are not expected to match macOS's addresses.

| Offset | BAR0 while Linux Wi-Fi is active | BAR2 while Linux Wi-Fi is active |
| ---: | ---: | ---: |
| `0xC40` helper request/scratch2 | `0x00000000` | `0x00000010` |
| `0xC44` firmware status/scratch3 | `0x00000000` | `0xfedcba00` |
| `0xCF0` driver ready/scratch12 | `0x00000000` | `0xfedcba00` |
| `0xCE8` event read pointer | `0x00000000` | `0x00000000` |
| `0xCEC` event write pointer | `0x00000000` | `0x00000080` |
| `0xC08C` TX read/RX write pointer | `0x42889803` | `0x041d0403` |
| `0xC05C` TX write/RX read pointer | `0x7fffffff` | `0x041d0003` |

This proves that Linux BAR2 is the semantic mwifiex register window on this physical card while firmware is active. It does **not** explain the earlier macOS/OpenCore BAR2 zero and `0xffffffff` reads. The next macOS boot should compare PMCSR, PCI command, direct BAR2 physical mapping, and these read-only registers before interpreting a firmware timeout. BAR0's nonzero data should not be mistaken for mwifiex register responses.

Preserved interpretation:

- The earlier macOS BAR2 reads did not show an active mwifiex register window at that moment. The live Ubuntu snapshot above subsequently confirms that physical BAR2 is the correct window when the card is active. The macOS discrepancy remains unresolved.
- BAR0 is readable and returns non-trivial values, but the sampled values look high-entropy/random rather than obvious firmware-ready, driver-ready, small ring-pointer, or event-pointer values. BAR0 is therefore **not** proven to be the correct mwifiex register window.
- Do not infer firmware readiness, firmware absence, ring state, interrupt state, safe reset semantics, or safe write semantics from these values.
- The Linux source still says those offsets are relative to `pci_mmap1` / Linux region-resource 2. The macOS resource identity mismatch remains an open mapping question rather than a reason to write either BAR.

## Acceptance criteria for future opt-in BAR-window validation

### Safe explicit validation

- No default boot-time or normal bring-up path scans all mapped BARs.
- Any future diagnostic requires an explicit operator action and samples only one selected BAR/resource and one selected offset per invocation.
- Candidate reads, if continued, should remain limited to the small known register set: `fwStatus@0xC44`, `drvRdy@0xCF0`, `txRd_rxWr@0xC08C`, `txWr_rxRd@0xC05C`, `evtRd@0xCE8`, and `evtWr@0xCEC`.
- No MMIO writes occur in this milestone.
- Selector output remains backward-compatible in this pass; no user-client ABI change is required for this safety cleanup.
- Surface/OpenCore validation records exact BAR/resource selected, register values, whether any reads return `0xffffffff`, and whether the system remains stable after each explicit single-window probe.

## Manual opt-in BAR/register read first step

This branch does not add a new kernel ABI selector. The existing read-only selector `kMarvellMethodReadReg32FromBar` already accepts exactly one BAR index and one register offset, and the kernel path validates BAR existence, memory BAR type, mapping, continuation status, 4-byte alignment, and access bounds before performing a 32-bit read.

The smallest safe next step is therefore local-test tooling only:

- default `local-test/marvell_probe` no longer performs the former default `readReg32FromBar(bar=0, off=0)` read;
- an operator must pass `--read-bar-reg <bar> <offset>` to request exactly one additional 32-bit BAR MMIO read;
- the tool rejects unaligned offsets locally before opening the user client;
- the tool prints an explicit warning before the read and warns not to loop or sweep BARs;
- all BAR existence/type/mapping/bounds checks remain enforced by the existing kernel read path;
- values are returned through the local tool output, not IOLog spam.

Recommended Surface sequence for identifying the Linux `pci_mmap1` equivalent remains intentionally slow and manual: capture BAR info first, choose one plausible mapped memory BAR, perform one selected offset read such as `0xC44`, wait and observe stability, then decide whether another single selected read is justified. Do not script a loop over BARs or offsets on the Surface target. Treat high-entropy-looking read values as suspicious until tied to a source-grounded semantic.

### Potentially mutating reset writes or PCI/PM reset

- Deferred until after read-only BAR validation.
- If reset is attempted later, choose between PCI FLR and D3cold-style power cycling based on macOS feasibility and Surface-specific evidence; do not invent a scratch-register reset.
- Define rollback: disable bus master/memory if needed, release mappings, and require reboot if the device stops responding.
- Acceptance requires a before/after read-only snapshot and a clear reason to proceed.

### Firmware upload/download

- Deferred until correct BAR/resource is proven and a firmware loading policy is documented.
- Must not commit firmware binaries.
- Must implement DMA-safe buffer allocation, physical address programming, command-size polling, `CPU_INTR_DOOR_BELL`, doorbell-ACK polling, retry handling, and timeout handling.
- Must not enable host interrupts during firmware download.

### Interrupt enablement

- Deferred until command-response, RX, event rings, and dispatch paths exist.
- Must keep `PCIE_HOST_INT_MASK` zero until handlers can process and acknowledge all enabled bits.
- Must explicitly distinguish read-only status sampling from acknowledge writes to `PCIE_HOST_INT_STATUS`.

### Ring allocation/initialization

- Deferred until DMA descriptor model is implemented.
- Must implement PFU descriptors for 8897, `0x20` TX/RX descriptors, `0x08` event descriptors, coherent DMA memory, and coupled pointer/wrap semantics.
- Must preserve opposite-direction wrap bits when writing shared TX/RX pointer registers.

### Command/event processing

- Deferred until command-response and event buffers are DMA-programmed.
- Must handle command response completion, event ready, event completion signaling, and ignored BT coexistence events.

### Network interface activation

- Last milestone after firmware, interrupts, rings, and command/event path are stable.
- Must not be enabled during PCIe bootstrap experimentation.

## Unknowns requiring Surface/OpenCore validation

- Why the earlier macOS `IOPCIDevice` BAR2 reads differed from Linux's proven active BAR2 window. Check direct BAR mapping, PCI PMCSR/command state, and firmware status on the next macOS boot before changing register targets.
- Whether BAR0 high-entropy-looking values are an alias, an unrelated memory window, a firmware-side mirror, or accidental benign reads.
- Exact values of `PCIE_HOST_INT_STATUS`, `PCIE_HOST_INT_MASK`, and `PCIE_HOST_INT_STATUS_MASK` before any interrupt enablement.
- Whether `fw_status` is zero, `0xfedcba00`, `0xffffffff`, or another boot-helper value at cold boot and warm reboot.
- Whether `drv_rdy` contains stale signatures after macOS/OpenCore boot and after Linux/Windows warm reboots.
- Whether reading candidate registers on BAR/region 2 is stable across repeated boots.
- Whether macOS/OpenCore exposes a safe equivalent to Linux PCI FLR or D3cold bridge/device power cycling on the target Surface.
- Which firmware file variant is required for the actual revision if/when firmware loading is implemented; Linux's revision-selection write to `0x0c58` needs separate safety review.

## Explicit non-goals for this pass

- No runtime driver behavior change.
- No reset enablement.
- No firmware loading or upload.
- No interrupt enablement.
- No ring allocation or pointer writes.
- No command/event processing.
- No network interface activation.
- No Info.plist, Xcode project, build setting, OpenCore deployment, or user-client ABI change.

## Passive Surface snapshot helper

`scripts/surface_passive_bar_snapshot.sh` is the recommended wrapper for collecting the next Surface/OpenCore validation bundle before any further manual BAR reads. It writes a timestamped report under `out/diagnostics/` and captures only passive/default diagnostics:

- git branch/head/status;
- built kext identity and executable UUID when available;
- loaded Marvell kext line from `kmutil showloaded` when available;
- default `local-test/marvell_probe` output;
- compact `MarvellW8897Controller` IORegistry view;
- recent Marvell log lines from the current boot.

The script intentionally does **not** pass `--read-bar-reg`, does not perform explicit BAR MMIO reads, does not deploy or mount EFI, does not reboot/shutdown, and does not modify files outside its report path. Explicit BAR reads remain separate manual one-at-a-time commands after reviewing the passive report.
