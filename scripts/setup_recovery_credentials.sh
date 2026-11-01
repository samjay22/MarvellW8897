#!/bin/bash
# Run locally in Recovery Terminal; never echo or log the Wi-Fi password.
set -euo pipefail
[[ $EUID -eq 0 && -d /Volumes/Sequoia ]] || { echo 'Mount Sequoia in Recovery first.' >&2; exit 1; }
umask 077
dir='/Volumes/Sequoia/.marvell-recovery'
mkdir -p "$dir"
chmod 700 "$dir"
IFS= read -r -p 'Wi-Fi name: ' ssid
IFS= read -r -s -p 'Wi-Fi password: ' password
printf '\n'
[[ ${#ssid} -ge 1 && ${#ssid} -le 32 && ${#password} -ge 8 && ${#password} -le 63 ]] || { echo 'Invalid name or WPA2 password length.' >&2; exit 1; }
printf '%s\n' "$ssid" > "$dir/ssid.tmp"
printf '%s\n' "$password" > "$dir/passphrase.tmp"
unset password
chmod 600 "$dir/ssid.tmp" "$dir/passphrase.tmp"
mv "$dir/ssid.tmp" "$dir/ssid"
mv "$dir/passphrase.tmp" "$dir/passphrase"
echo 'Wi-Fi credentials stored locally for Recovery startup.'
