#!/usr/bin/env python3
"""Verify the licensed binary firmware and generate a local C++ byte include."""

from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware" / "pcie8897_uapsta.bin"
INCLUDE = ROOT / "firmware" / "pcie8897_uapsta.inc"
EXPECTED_SIZE = 723540
EXPECTED_SHA256 = "cf4f51f41bd7ef4d7fe65fb76b8a2a0897bc70a0742bc4aea13d93b03fffd03a"

data = FIRMWARE.read_bytes()
if len(data) != EXPECTED_SIZE or sha256(data).hexdigest() != EXPECTED_SHA256:
    raise SystemExit("Firmware size or SHA-256 does not match the verified 88W8897 image")

lines = [
    "    " + ", ".join(f"0x{byte:02x}" for byte in data[offset : offset + 16]) + ","
    for offset in range(0, len(data), 16)
]
content = "\n".join(lines) + "\n"
if not INCLUDE.exists() or INCLUDE.read_text() != content:
    INCLUDE.write_text(content)
print(f"Verified firmware; generated {INCLUDE.relative_to(ROOT)}")
