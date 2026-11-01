#!/bin/sh
# Add the test-only 88W8897 PCI model to a local QEMU 8.2 source checkout.
set -eu

if [ "$#" -ne 1 ] || [ ! -f "$1/hw/misc/meson.build" ]; then
    echo "Usage: $0 /path/to/qemu-8.2-source" >&2
    exit 2
fi

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
qemu_root=$(CDPATH= cd -- "$1" && pwd)
model="$qemu_root/hw/misc/marvell8897-test.c"
meson="$qemu_root/hw/misc/meson.build"
entry="system_ss.add(when: 'CONFIG_PCI', if_true: files('marvell8897-test.c'))"

cp "$project_root/tests/qemu-marvell8897-test.c" "$model"
if ! grep -Fq "$entry" "$meson"; then
    printf '\n%s\n' "$entry" >> "$meson"
fi
echo "Installed marvell-8897-test PCI model in $qemu_root"
