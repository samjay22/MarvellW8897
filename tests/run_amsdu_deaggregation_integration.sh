#!/bin/bash
# Compile the NXP A-MSDU deaggregator with a fresh, strict application of our
# bounds patch and execute it under AddressSanitizer and UndefinedBehaviorSanitizer.
# This is a local userspace regression test, not a macOS-kernel or hardware test.
set -euo pipefail

project_root=$(cd "$(dirname "$0")/.." && pwd)
source_root="$project_root/nxp-mwifiex/mlan"
test_source="$project_root/tests/amsdu_deaggregation_integration.c"
patch_file="$project_root/patches/nxp-mlan-amsdu-length.patch"
guard_file="$project_root/patches/marvell_amsdu_guard.h"
temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/marvell-amsdu-test.XXXXXX")
trap 'rm -rf "$temporary_root"' EXIT

if [[ ! -f "$source_root/mlan_11n_aggr.c" ]]; then
  echo "Missing pinned NXP source: $source_root/mlan_11n_aggr.c" >&2
  exit 1
fi

compile() {
  local source_file=$1
  local output_file=$2
  clang -std=gnu99 -fgnu89-inline -O1 -g \
    -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined \
    -DMLAN_64BIT -DPCIE -DPCIE8897 -DSTA_SUPPORT -DUAP_SUPPORT -DDEBUG_LEVEL1 \
    '-Dfallthrough=__attribute__((fallthrough))' \
    -I "$source_root" -I "$temporary_root/patched" \
    "$test_source" "$source_file" -Wl,-dead_strip -o "$output_file"
}

mkdir -p "$temporary_root/patched"
cp "$source_root/mlan_11n_aggr.c" "$temporary_root/patched/mlan_11n_aggr.c"
cp "$guard_file" "$temporary_root/patched/marvell_amsdu_guard.h"
patch -s -F 0 -d "$temporary_root/patched" -i "$patch_file"

compile "$temporary_root/patched/mlan_11n_aggr.c" "$temporary_root/patched-test"
"$temporary_root/patched-test"

# The intentionally malformed 14-byte subframe gives the unpatched parser a
# length it used to treat as valid.  Requiring ASan to catch the resulting
# out-of-bounds read proves this harness reaches the formerly unsafe path.
compile "$source_root/mlan_11n_aggr.c" "$temporary_root/upstream-test"
set +e
/usr/bin/env bash -c 'ASAN_OPTIONS=halt_on_error=1 "$1" --malformed-boundary; status=$?; exit "$status"' \
  _ "$temporary_root/upstream-test" >"$temporary_root/upstream.log" 2>&1
upstream_status=$?
set -e
if [[ "$upstream_status" -eq 0 ]]; then
  echo "The unpatched NXP parser unexpectedly accepted the malformed boundary" >&2
  exit 1
fi
if ! grep -q "AddressSanitizer: heap-buffer-overflow" "$temporary_root/upstream.log"; then
  echo "The unpatched parser failed, but not with the expected ASan boundary finding" >&2
  sed -n '1,120p' "$temporary_root/upstream.log" >&2
  exit 1
fi
echo "Unpatched parser reproduces the expected ASan heap-buffer-overflow."
