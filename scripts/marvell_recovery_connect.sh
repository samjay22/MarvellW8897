#!/bin/bash
# Run from the mounted EFI/MarvellW8897 directory in macOS Recovery Terminal.
# The loader prompts for the WPA2 password without echoing it.
set -euo pipefail
export LC_ALL=C

if (( EUID != 0 )); then
    printf 'Run this from Recovery Terminal as root.\n' >&2
    exit 2
fi

mode=${1:-wpa2}
if (( $# > 1 )) || [[ "$mode" != wpa2 && "$mode" != --open ]]; then
    printf 'Usage: %s [--open]\n' "$0" >&2
    exit 2
fi

tool_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
loader="$tool_dir/marvell_fwupload"
if [[ ! -x "$loader" ]]; then
    printf 'marvell_fwupload is missing beside this script.\n' >&2
    exit 1
fi

status=''
for (( attempt=0; attempt<60; attempt++ )); do
    status=$("$loader" --status 2>&1) || status=''
    if [[ "$status" == *'registered=1 initComplete=1 initStatus=0'* ]]; then
        break
    fi
    sleep 1
done
if [[ "$status" != *'registered=1 initComplete=1 initStatus=0'* ]]; then
    printf 'Marvell firmware or MLAN did not become ready. Last status: %s\n' "$status" >&2
    exit 1
fi

IFS= read -r -p 'Wi-Fi SSID: ' ssid
if (( ${#ssid} < 1 || ${#ssid} > 32 )) || [[ "$ssid" == *$'\n'* ]]; then
    printf 'SSID must be 1 to 32 bytes.\n' >&2
    exit 2
fi

if [[ "$mode" == --open ]]; then
    "$loader" --connect-open "$ssid"
else
    "$loader" --connect "$ssid"
fi

join_status=''
for (( attempt=0; attempt<90; attempt++ )); do
    join_status=$("$loader" --connect-status 2>&1) || join_status=''
    if [[ "$join_status" == *'associated=1'* ]]; then
        break
    fi
    if [[ "$join_status" == *'connectPhase=12'* ]]; then
        printf 'Wi-Fi association failed: %s\n' "$join_status" >&2
        exit 1
    fi
    sleep 1
done
if [[ "$join_status" != *'associated=1'* ]]; then
    printf 'Wi-Fi association timed out: %s\n' "$join_status" >&2
    exit 1
fi

interface=''
for (( attempt=0; attempt<10; attempt++ )); do
    interface=$("$loader" --interface 2>/dev/null) || interface=''
    [[ "$interface" =~ ^en[0-9]+$ ]] && break
    sleep 1
done
if [[ ! "$interface" =~ ^en[0-9]+$ ]] &&
   /sbin/ifconfig en0 2>/dev/null | /usr/bin/grep -q 'status: active'; then
    # Recovery can publish a working BSD interface before the user-client
    # registry walk finds its IOBSDName. This Surface has only the Marvell en0.
    interface=en0
    printf 'Using active en0 after interface registry lookup missed it.\n'
fi
if [[ ! "$interface" =~ ^en[0-9]+$ ]]; then
    printf 'Associated, but the macOS network interface did not appear.\n' >&2
    exit 1
fi

/sbin/ifconfig "$interface" up
/usr/sbin/ipconfig set "$interface" DHCP
address=''
for (( attempt=0; attempt<30; attempt++ )); do
    address=$(/usr/sbin/ipconfig getifaddr "$interface" 2>/dev/null) || address=''
    [[ -n "$address" ]] && break
    sleep 1
done
if [[ -z "$address" ]]; then
    printf 'Associated on %s, but DHCP did not return an IPv4 address.\n' "$interface" >&2
    exit 1
fi
printf 'Wi-Fi associated on %s with IPv4 address %s\n' "$interface" "$address"
