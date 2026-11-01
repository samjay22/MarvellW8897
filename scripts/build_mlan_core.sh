#!/bin/bash
set -euo pipefail

project_root=$(cd "$(dirname "$0")/.." && pwd)
python3 "$project_root/scripts/generate_embedded_firmware.py"
source_root="$project_root/nxp-mwifiex/mlan"
output_root="$project_root/build/mlan-core"

if [[ ! -f "$source_root/mlan_shim.c" ]]; then
  echo "NXP mlan source is missing from $source_root" >&2
  exit 1
fi

developer_dir=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
compiler="$developer_dir/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang"
sdk=$(DEVELOPER_DIR="$developer_dir" /usr/bin/xcrun --sdk macosx --show-sdk-path)
archiver="$developer_dir/Toolchains/XcodeDefault.xctoolchain/usr/bin/libtool"

mkdir -p "$output_root"

# Build against the pinned NXP source, applying our reviewed PCIe RX length
# guard to a copy.  Do not modify the submodule checkout.  Refuse fuzzy patch
# matches so an upstream change cannot silently bypass or misplace the guard.
override_root="$output_root/overrides"
mkdir -p "$override_root"
cp "$source_root/mlan_pcie.c" "$override_root/mlan_pcie.c"
cp "$project_root/patches/marvell_rx_length_guard.h" "$override_root/"
patch -s -F 0 -d "$override_root" -i "$project_root/patches/nxp-mlan-pcie-rx-length.patch"

sources=(
  shim init txrx cmdevt misc cfp wmm pcie
  11n_aggr 11n_rxreorder 11n 11ac 11ax 11d 11h
  meas scan sta_ioctl sta_rx sta_tx sta_event sta_cmd sta_cmdresp join
  uap_ioctl uap_cmdevent uap_txrx
)

objects=()
for name in "${sources[@]}"; do
  object="$output_root/mlan_${name}.o"
  source_file="$source_root/mlan_${name}.c"
  if [[ "$name" == pcie ]]; then
    source_file="$override_root/mlan_pcie.c"
  fi
  "$compiler" \
    -target x86_64-apple-macos15.4 \
    -isysroot "$sdk" \
    -std=gnu99 -fgnu89-inline -O2 -fno-builtin -fno-stack-protector -fno-common \
    -Wno-pointer-sign -Wno-address-of-packed-member \
    -DKERNEL -DMLAN_64BIT -DPCIE -DPCIE8897 -DSTA_SUPPORT -DUAP_SUPPORT \
    -DDEBUG_LEVEL1 '-Dfallthrough=__attribute__((fallthrough))' \
    -I "$source_root" \
    -c "$source_file" -o "$object"
  objects+=("$object")
done

"$archiver" -static -o "$output_root/libmlan8897.a" "${objects[@]}"
echo "$output_root/libmlan8897.a"
