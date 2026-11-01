# Marvell 88W8897 for Surface Pro 4: experimental macOS port

This repository contains an experimental x86_64 IOKit driver for the Surface Pro 4's PCIe Marvell AVASTAR 88W8897 (`11ab:2b38`), plus a macOS host adapter around NXP's GPL-2.0 MLAN protocol core. The target is macOS Sequoia via OpenCore.

**Current status (1 November 2026): experimental and intermittently connected.** The physical Surface Pro 4 booted macOS Sequoia Recovery and then installed macOS Sequoia with this kext injected by OpenCore. The controller matched PCI ID `11ab:2b38`, initialized MLAN (`registered=1 initComplete=1 initStatus=0`), exposed an Ethernet-style `en*` interface, joined a WPA2 network, received an IPv4 address by DHCP, and exchanged packets with both a local host and the internet. A supervised manual association from the installed desktop also completed on 1 November. The connection has dropped during a large installer download and an earlier desktop kernel panic implicated the driver under traffic. Reliable boot-time Wi-Fi, reconnect, sleep/wake, sustained traffic, and native Wi-Fi integration are not verified.

## Implemented in the development build

- PCI matching, identity/BAR/transport diagnostics, and BAR2 register access based on the Linux `mwifiex_pcie` mapping.
- Operational MMIO now maps PCI BAR2 by its configuration register through `IOPCIDevice::mapDeviceMemoryWithRegister`, and checks that the mapped physical base matches the BAR. Indexed IOKit resources are retained as metadata only; firmware and MLAN writes no longer depend on a fallback resource-order guess.
- A read-only snapshot from this Surface under Ubuntu confirms that active BAR2 returns the 88W8897 firmware-ready signature (`0xfedcba00`) at both `0xC44` and `0xCF0`; BAR0 does not. The kext logs PCI PMCSR and command state at match and auto-start so the earlier macOS BAR2 zero/invalid reads can be compared with the working D0 state.
- The PCI personality matches vendor/device `11ab:2b38` directly; it does not require a generated macOS PCI name to match as well.
- Boot-time matching maps and validates BAR2 without reading device registers or enabling bus mastering. Five seconds later, a deferred kernel callback attempts firmware upload, MLAN initialization, and interrupt setup using an embedded copy of the verified image.
- The x86_64 kext contains the exact 723,540-byte firmware image, so its automatic startup attempt does not depend on mounting the EFI volume from Recovery.
- A root-only manual firmware uploader that accepts only the exact 723,540-byte `pcie8897_uapsta.bin` image matching the target's Ubuntu firmware SHA-256.
- The firmware size and SHA-256 now come from one shared header used by both the kext and loader. The loader reads the exact file size before hashing; this corrects an earlier build that rejected the bundled firmware before it could contact the driver.
- If the combo chip reports that another interface owns firmware download, the uploader now waits up to 15 seconds for the shared firmware to become ready, following Linux's multi-interface path. A timeout on that path leaves a later manual retry available because no DMA transfer was submitted.
- Automatic boot startup retries up to three attempts, ten seconds apart, only if the helper made no firmware DMA request or another interface still owned the shared firmware. A failed transfer after DMA starts remains terminal for that boot attempt.
- If the helper stops requesting blocks after a partial transfer, the uploader follows Linux by checking the firmware-ready signature instead of requiring the image's end of file. A zero request before any transfer still reports a helper timeout.
- NXP MLAN PCIe 8897 core linked from 27 GPL-2.0 C files, with macOS memory, lock, timer, clock, 32-bit DMA, interrupt, and deferred-work callbacks. The host supplies NXP's default 4 KiB receive buffer size so PCIe receive rings have a nonzero allocation size.
- Firmware and MLAN DMA buffers request physically contiguous pages below 4 GB using IOKit's physical-address mask, matching the card's 32-bit DMA limit even when there is no usable IOMMU mapping.
- Auto deep sleep is disabled in the MLAN host record for this Surface's revision, following the behavior reported by its Ubuntu driver.
- Root-only `--register-core` and `--start-core` development commands, plus `--status` for initialization and event state.
- A root-only asynchronous `--scan [SSID]` request, with `--scan-status` reporting MLAN completion and the count of BSS entries. The scan request and result query keep their buffers alive across MLAN callbacks.
- A root-only manual `--connect SSID` attempt for WPA2-PSK/CCMP and `--connect-open SSID` for an open network. The command sequence scans, configures security, selects a BSS, and requests association; `--connect-status` reports each stage and the connected event. The WPA2 passphrase is read from standard input with terminal echo disabled and cleared from temporary buffers after use.
- An `IOEthernetController` interface that is attached only after MLAN reports successful initialization and a valid MAC address. The link stays down until MLAN reports association. TX buffers are owned until completion; RX frames are copied into macOS mbufs before MLAN reclaims them.
- Interface registration now occurs after the controller stores its interface pointer, so an early macOS `enable()` callback can recognize it. Synchronous detach runs outside the driver network lock to avoid blocking that lock during shutdown.
- The kext declares the BSD kernel interface used by its mbuf calls, along with the kernel interfaces needed by its deferred work and timers.
- Firmware-info and scan requests keep their buffers alive until asynchronous completion. Received frames are flushed into macOS after releasing the driver network lock. An association wait times out after 30 seconds so a failed manual attempt can be retried.
- Received frames are checked against the host allocation size before they are copied to a macOS mbuf, including their data offset; an invalid length now fails the receive callback.
- An immediate successful MLAN initialization now marks the core ready and schedules interface setup, matching the asynchronous completion path. The host also releases its firmware-info lock if thread-call allocation fails during registration.
- The MLAN bounded-copy callback now copies the bytes that fit, matching NXP's Linux host contract. Previously it skipped the entire copy when a caller requested more than the destination capacity.
- An optional LaunchDaemon for an installed macOS system can request association at boot or after disconnect. Its setup script stores the WPA2 passphrase in the System keychain, and the loader's `--interface` command identifies this driver's BSD interface so the daemon can start DHCP after association. Open networks are also supported by this setup path.

The Ethernet-style interface gives macOS a data path; it does **not** make the chip appear in Apple's built-in Wi-Fi menu. Automatic firmware and MLAN startup succeeded in the tested Recovery image, but Recovery still requires a manual join and the link has dropped under sustained use. An installed system can use the optional boot-time helper after one-time credential setup; that helper has not run on the Surface. Native Wi-Fi menu integration, reliable reconnection, and power management remain open work.

## Build

Clone the repository with its pinned MacKernelSDK and NXP MLAN submodules, then build with Xcode:

```sh
git clone --recurse-submodules https://github.com/samjay22/MarvellW8897.git
cd MarvellW8897
```

If already cloned, run `git submodule update --init --recursive` before building.

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild \
  -project MarvellW8897.xcodeproj -scheme MarvellW8897Kext \
  -configuration Release -derivedDataPath build/xcode \
  CODE_SIGNING_ALLOWED=NO build
```

The build script verifies the licensed binary firmware and generates `firmware/pcie8897_uapsta.inc` locally before compilation. The generated text is excluded from Git. The resulting unsigned kext is a development artifact. `local-test/marvell_fwupload.cpp` is the manual user client. Its `--start-core` operation enables interrupts and asks MLAN to initialize PCIe rings; use it only on a supervised Surface macOS test boot with a recovery path.

For a supervised Sequoia Recovery boot, place `scripts/marvell_recovery_connect.sh` beside the built `marvell_fwupload` binary in a Recovery-accessible folder. Run it as root from Recovery Terminal. It waits for MLAN initialization, prompts for the SSID and a hidden WPA2 password, requests association, and starts DHCP on the interface published by this driver. Pass `--open` for an open network. An earlier copy of the script in the tested Recovery image missed the working `en0` interface after association; manual `ipconfig set en0 DHCP` completed the connection. The current script includes an `en0` fallback. This script cannot compensate for a kext that fails to attach or initialize, and it does not automatically recover a later disconnect.

For a Recovery session with intermittent Wi-Fi, `scripts/marvell_recovery_watchdog.sh` can be copied to `/tmp` and run as root. It prompts for the WPA2 credentials and keeps the password only in a background shell's memory while it checks for driver disconnects and requests reassociation. `scripts/monitor_marvell_recovery.sh` records a time series of connection state and gateway reachability. These helpers are experimental and do not repair the underlying disconnect cause.

After macOS is installed and the driver has demonstrated a stable manual connection, copy `marvell_fwupload`, `marvell_wifi_autojoin.sh`, `install_marvell_wifi_autojoin.sh`, and `org.marvellw8897.autojoin.plist` to one folder on the Surface. Run `sudo bash install_marvell_wifi_autojoin.sh 'SSID'` for WPA2 or `sudo bash install_marvell_wifi_autojoin.sh --open 'SSID'` for an open network. The WPA2 setup prompts for the passphrase directly into the System keychain; it does not put the password in a command argument. The LaunchDaemon polls for MLAN readiness, joins the configured network, and starts DHCP when the link reports association. This setup path is untested on the Surface and depends on the underlying kext actually initializing and associating.

## Firmware helper simulation

`tests/firmware_protocol_test.cpp` emulates the helper's BAR2 request, doorbell, and firmware-ready registers in user space. It checks every DMA block against the verified firmware file, including a CRC retry and the zero-padded final block:

```sh
clang++ -std=c++17 tests/firmware_protocol_test.cpp \
  Contents/MacOS/MarvellW8897FirmwareProtocol.cpp \
  -o firmware_protocol_test
./firmware_protocol_test firmware/pcie8897_uapsta.bin
```

This runs the portable firmware protocol, not the macOS PCI, DMA, interrupt, MLAN, or radio paths.

## QEMU PCI device model

`tests/qemu-marvell8897-test.c` is a test-only QEMU 8.2.2 PCIe model for the Surface's `11ab:2b38` / `0004:045e` device. It exposes 1 MiB 64-bit BAR0 and BAR2 windows, power-management and MSI capabilities, and the firmware helper registers in BAR2. It reads guest DMA blocks and compares all 723,540 firmware bytes with the real firmware image supplied by `firmware=`. The last 596 firmware bytes are sent in a 768-byte DMA block, rounded to the 256-byte download granularity used by Linux and this macOS uploader. It intentionally does not implement MLAN commands, scanning, radio, or Wi-Fi association.

Build QEMU from [the official 8.2.2 source](https://download.qemu.org/qemu-8.2.2.tar.xz), after installing QEMU's documented build dependencies:

```sh
scripts/install_qemu_test_device.sh /path/to/qemu-8.2.2
cd /path/to/qemu-8.2.2
./configure --target-list=x86_64-softmmu --disable-werror
ninja -C build qemu-system-x86_64
```

Confirm that the model registers and accepts the expected firmware DMA sequence:

```sh
python3 tests/qemu_firmware_smoke.py \
  /path/to/qemu-8.2.2/build/qemu-system-x86_64 \
  firmware/pcie8897_uapsta.bin
```

The QEMU model and smoke run work on this Mac. Testing the macOS PCI and IOKit paths still requires an x86_64 macOS guest with this device; testing RF and Wi-Fi traffic still requires the physical Surface.

## Boot and test boundaries

- The safe OpenCore EFI package keeps this kext disabled. The test EFI enables the kext, embedded firmware startup, and verbose/panic logging. These locally generated packages are not included here because they contain Apple Recovery files or machine-specific OpenCore settings.
- The physical Surface booted a Sequoia 15.4.1 Recovery image using the test EFI. The kext initialized and passed real IP traffic, but the connection dropped during installer download. This is a successful bring-up test, not a stable installed-system validation.
- A macOS kext runs after the kernel starts. It cannot supply Wi-Fi to OpenCore or macOS Recovery before the kernel loads it.
- The physical Recovery test exercised firmware upload, MLAN initialization, association, DHCP, and packet traffic. It did not prove long-running reliability, automatic rejoin, native Wi-Fi controls, or operation after a full Sequoia installation.
- Do not use this development driver as the only route to recover or install the machine.

## Source and licensing

- `Contents/MacOS/` contains the macOS driver and MLAN host adapter.
- `nxp-mwifiex/mlan/` is a pinned submodule containing NXP's MLAN source; its GPL-2.0 license is in `nxp-mwifiex/LICENSE`.
- `firmware/` contains the exact binary firmware image and its upstream license. The text include is generated locally and is not published.
- `MacKernelSDK/` is a pinned submodule supplying build headers and carrying its own license.
- `docs/` includes earlier diagnostic notes; some notes describe the scaffold before the firmware and network changes in this working tree.

The linked binary includes GPL-2.0 code; the original scaffold's MIT license does not relicense NXP's MLAN core.
