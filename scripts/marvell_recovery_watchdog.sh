#!/bin/bash
# Temporary Recovery reconnect helper. The WPA2 secret stays in memory.
set -u

tool=/MarvellW8897/marvell_fwupload
pidfile=/tmp/marvell-recovery-watchdog.pid
logfile=/tmp/marvell-recovery-watchdog.log

if [[ ! -x "$tool" ]]; then
    printf 'Marvell tool is missing.\n' >&2
    exit 1
fi
if [[ -r "$pidfile" ]]; then
    read -r existing < "$pidfile" || existing=''
    if [[ "$existing" =~ ^[0-9]+$ ]] && kill -0 "$existing" 2>/dev/null; then
        printf 'Watchdog is already running as PID %s.\n' "$existing"
        exit 0
    fi
fi

read -r -p 'Wi-Fi SSID: ' ssid
read -r -s -p 'WPA2 password (hidden): ' pass
printf '\n'
if (( ${#ssid} < 1 || ${#ssid} > 32 || ${#pass} < 8 || ${#pass} > 63 )); then
    printf 'Invalid SSID or WPA2 password length.\n' >&2
    unset pass
    exit 2
fi

initial=$("$tool" --connect-status 2>/dev/null) || initial=''
previous=0
[[ "$initial" == *'associated=1'* ]] && previous=1

(
    trap '' HUP
    while :; do
        core=$("$tool" --status 2>/dev/null) || core=''
        if [[ "$core" != *'registered=1 initComplete=1 initStatus=0'* ]]; then
            printf '%s MLAN not ready\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
            sleep 10
            continue
        fi
        state=$("$tool" --connect-status 2>/dev/null) || state=''
        if [[ "$state" == *'associated=1'* ]]; then
            address=$(/usr/sbin/ipconfig getifaddr en0 2>/dev/null) || address=''
            if [[ "$previous" != 1 || -z "$address" ]]; then
                /sbin/ifconfig en0 up >/dev/null 2>&1
                /usr/sbin/ipconfig set en0 DHCP >/dev/null 2>&1
                printf '%s association restored; requested DHCP\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
            fi
            previous=1
        else
            if [[ "$previous" == 1 ]]; then
                printf '%s link dropped: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$state"
            fi
            previous=0
            phase=${state#*connectPhase=}
            phase=${phase%% *}
            case "$phase" in
                [1-9]|10) ;;
                *)
                    printf '%s requesting reassociation\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
                    printf '%s\n' "$pass" | "$tool" --connect "$ssid" >/dev/null 2>&1
                    ;;
            esac
        fi
        sleep 5
    done
) >"$logfile" 2>&1 </dev/null &

watchpid=$!
printf '%s\n' "$watchpid" > "$pidfile"
unset pass
printf 'Recovery Wi-Fi watchdog started as PID %s.\n' "$watchpid"
