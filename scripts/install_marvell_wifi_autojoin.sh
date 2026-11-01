#!/bin/bash
# Run from an installed macOS system as root, with the bundled x86_64 loader.
set -euo pipefail
export LC_ALL=C

if (( EUID != 0 )); then
    printf 'Run this installer with sudo on the Surface macOS system.\n' >&2
    exit 2
fi
if (( $# == 1 )); then
    mode=wpa2
    ssid=$1
elif (( $# == 2 )) && [[ "$1" == --open ]]; then
    mode=open
    ssid=$2
else
    printf 'Usage: sudo %s SSID | sudo %s --open SSID\n' "$0" "$0" >&2
    exit 2
fi
if (( ${#ssid} < 1 || ${#ssid} > 32 )) || [[ "$ssid" == *$'\n'* ]]; then
    printf 'SSID must be 1 to 32 bytes and contain no newline.\n' >&2
    exit 2
fi

source_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
[[ -f "$source_dir/marvell_fwupload" && -r "$source_dir/marvell_wifi_autojoin.sh" &&
   -r "$source_dir/org.marvellw8897.autojoin.plist" ]] || {
    printf 'The loader, agent, and launchd plist must be beside this installer.\n' >&2
    exit 2
}

if [[ "$mode" == wpa2 ]]; then
    printf 'Enter the WPA2 password for %s into the System keychain.\n' "$ssid"
    /usr/bin/security add-generic-password -U -a "$ssid" \
        -s org.marvellw8897.wpa2 /Library/Keychains/System.keychain -w
fi

agent_dir='/Library/Application Support/MarvellW8897'
plist='/Library/LaunchDaemons/org.marvellw8897.autojoin.plist'
/bin/mkdir -p "$agent_dir"
/usr/sbin/chown root:wheel "$agent_dir"
/bin/chmod 700 "$agent_dir"
/usr/bin/install -o root -g wheel -m 700 \
    "$source_dir/marvell_fwupload" "$agent_dir/marvell_fwupload"
/usr/bin/install -o root -g wheel -m 700 \
    "$source_dir/marvell_wifi_autojoin.sh" "$agent_dir/marvell_wifi_autojoin.sh"
umask 077
printf '%s\n' "$ssid" > "$agent_dir/ssid"
printf '%s\n' "$mode" > "$agent_dir/mode"
/usr/sbin/chown root:wheel "$agent_dir/ssid" "$agent_dir/mode"

/usr/bin/install -o root -g wheel -m 644 \
    "$source_dir/org.marvellw8897.autojoin.plist" "$plist"
/usr/bin/plutil -lint "$plist" >/dev/null
/bin/launchctl bootout system/org.marvellw8897.autojoin 2>/dev/null || true
/bin/launchctl bootstrap system "$plist"
printf 'Boot-time association configured for %s. Check the driver and network service after the next boot.\n' "$ssid"
