#!/bin/bash
# Runs as a LaunchDaemon on an installed macOS system after OpenCore loads
# MarvellW8897Kext. Credentials are read from the System keychain on demand.
set -euo pipefail

agent_dir='/Library/Application Support/MarvellW8897'
loader="$agent_dir/marvell_fwupload"
[[ -x "$loader" && -r "$agent_dir/ssid" && -r "$agent_dir/mode" ]] || exit 0
IFS= read -r ssid < "$agent_dir/ssid" || exit 0
IFS= read -r mode < "$agent_dir/mode" || exit 0
[[ -n "$ssid" ]] || exit 0

core_status=$("$loader" --status 2>/dev/null) || exit 0
[[ "$core_status" == *'registered=1 initComplete=1 initStatus=0'* ]] || exit 0

join_status=$("$loader" --connect-status 2>/dev/null) || exit 0
dhcp_marker='/var/run/marvellw8897-dhcp'
if [[ "$join_status" == *'associated=1'* ]]; then
    interface=$("$loader" --interface 2>/dev/null) || interface=''
    if [[ ! "$interface" =~ ^en[0-9]+$ ]] &&
       /sbin/ifconfig en0 2>/dev/null | /usr/bin/grep -q 'status: active'; then
        interface=en0
    fi
    [[ "$interface" =~ ^en[0-9]+$ ]] || exit 0
    /sbin/ifconfig "$interface" up || exit 0
    previous=''
    if [[ -r "$dhcp_marker" ]]; then
        IFS= read -r previous < "$dhcp_marker" || previous=''
    fi
    address=$(/usr/sbin/ipconfig getifaddr "$interface" 2>/dev/null) || address=''
    if [[ "$previous" != "$interface" || -z "$address" ]]; then
        /usr/sbin/ipconfig set "$interface" DHCP || exit 0
        printf '%s\n' "$interface" > "$dhcp_marker"
    fi
    exit 0
fi
/bin/rm -f "$dhcp_marker"
if [[ "$join_status" =~ connectPhase=([0-9]+) ]]; then
    phase=${BASH_REMATCH[1]}
    (( phase >= 1 && phase <= 10 )) && exit 0
fi

case "$mode" in
    open)
        "$loader" --connect-open "$ssid" >/dev/null
        ;;
    wpa2)
        # A pipe keeps the password out of process arguments, files and logs.
        /usr/bin/security find-generic-password \
            -a "$ssid" -s org.marvellw8897.wpa2 -w \
            /Library/Keychains/System.keychain |
            "$loader" --connect "$ssid" >/dev/null
        ;;
    *) exit 2 ;;
esac
