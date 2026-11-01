#!/usr/bin/env python3
"""Exercise the test PCI model's BAR2/DMA firmware handshake via QEMU qtest.

This checks the emulator's behavior. It does not execute the macOS kext.
"""

import argparse
import subprocess
from pathlib import Path


BLOCK = 1024
DMA_ALIGNMENT = 256
PCI_CONFIG_ADDR = 0xCF8
PCI_CONFIG_DATA = 0xCFC
PCI_DEVFN_CONFIG = 0x80000000 | (3 << 11)
BAR2_BASE = 0x80000000
DMA_BASE = 0x01000000
READY = 0xFEDCBA00


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("qemu", type=Path, help="QEMU binary with marvell-8897-test")
    parser.add_argument("firmware", type=Path, help="pcie8897_uapsta.bin")
    args = parser.parse_args()
    firmware = args.firmware.read_bytes()
    if len(firmware) != 723540:
        raise SystemExit(f"unexpected firmware length: {len(firmware)}")

    cmd = [
        str(args.qemu.resolve()), "-machine", "q35", "-accel", "qtest",
        "-nodefaults", "-display", "none", "-monitor", "none",
        "-qtest", "stdio", "-device",
        f"marvell-8897-test,addr=3.0,firmware={args.firmware.resolve()}",
    ]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, bufsize=1)
    assert proc.stdin and proc.stdout

    def ask(command: str) -> str:
        proc.stdin.write(command + "\n")
        proc.stdin.flush()
        response = proc.stdout.readline().strip()
        if not response.startswith("OK"):
            raise RuntimeError(f"qtest {command.split()[0]}: {response}")
        return response

    def config_address(offset: int) -> None:
        ask(f"outl {PCI_CONFIG_ADDR:#x} {PCI_DEVFN_CONFIG | offset:#x}")

    def config_read(offset: int) -> int:
        config_address(offset)
        return int(ask(f"inl {PCI_CONFIG_DATA:#x}").split()[1], 0)

    def config_write(offset: int, value: int) -> None:
        config_address(offset)
        ask(f"outl {PCI_CONFIG_DATA:#x} {value:#x}")

    def read32(offset: int) -> int:
        return int(ask(f"readl {BAR2_BASE + offset:#x}").split()[1], 0)

    def write32(offset: int, value: int) -> None:
        ask(f"writel {BAR2_BASE + offset:#x} {value:#x}")

    try:
        assert config_read(0) == 0x2B3811AB, "wrong PCI vendor/device ID"
        assert config_read(0x2C) == 0x045E0004, "wrong PCI subsystem ID"
        config_write(0x18, BAR2_BASE | 0xC)
        config_write(0x1C, 0)
        config_write(0x04, 0x00000006)  # MEM decode + bus master
        assert read32(0x0C40) == BLOCK, "helper request not visible in BAR2"
        assert read32(0x0C44) == 0, "firmware unexpectedly ready"

        blocks = 0
        for start in range(0, len(firmware), BLOCK):
            payload = firmware[start:start + BLOCK]
            dma_length = (len(payload) + DMA_ALIGNMENT - 1) // DMA_ALIGNMENT * DMA_ALIGNMENT
            block = payload.ljust(dma_length, b"\0")
            ask(f"write {DMA_BASE:#x} {dma_length:#x} 0x{block.hex()}")
            write32(0x0C10, DMA_BASE)
            write32(0x0C14, 0)
            write32(0x0C40, dma_length)
            write32(0x0C18, 2)
            if read32(0x0C1C) != 0:
                raise AssertionError(f"doorbell did not clear at block {blocks}")
            blocks += 1

        assert read32(0x0C40) == 0, "helper still requests data after EOF"
        write32(0x0C3C, 0xF)
        write32(0x0CF0, READY)
        assert read32(0x0C44) == READY, "firmware acceptance failed"
        print(f"QEMU model accepted {blocks} exact firmware DMA blocks; BAR2 and PCI IDs match")
    finally:
        proc.terminate()
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()


if __name__ == "__main__":
    main()
