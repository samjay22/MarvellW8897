#!/bin/bash
# Run periodically as root by Recovery's launchd. The setup file and SSH
# bundle are stored on the Sequoia APFS volume and are readable only by root.
set -u
config='/Volumes/Sequoia/.marvell-recovery'
if [[ ! -d "$config" ]]; then
    /usr/sbin/diskutil mount Sequoia >/dev/null 2>&1 || true
fi
[[ -r "$config/ssid" && -r "$config/passphrase" ]] || exit 0
IFS= read -r ssid < "$config/ssid" || exit 0
[[ -n "$ssid" ]] || exit 0

sshdir='/tmp/marvell-recovery-ssh'
pidfile='/tmp/marvell-recovery-ssh.pid'
ssh_running=0
for existing_pidfile in "$pidfile" /tmp/recovery-dropbear.pid; do
    if [[ -r "$existing_pidfile" ]] && kill -0 "$(cat "$existing_pidfile")" 2>/dev/null; then
        ssh_running=1
        break
    fi
done
if [[ -r "$config/dropbear.tgz" && -x "$config/db2" ]] &&
   [[ $ssh_running -eq 0 ]]; then
    mkdir -p "$sshdir"
    chmod 700 "$sshdir"
    tar -xzf "$config/dropbear.tgz" -C "$sshdir" || exit 1
    chmod 600 "$sshdir/host_key" "$sshdir/authorized_keys"
    "$config/db2" -r "$sshdir/host_key" -D "$sshdir" -p 2223 -P "$pidfile" -m -z || true
fi

loader='/MarvellW8897/marvell_fwupload'
status=$("$loader" --status 2>/dev/null) || exit 0
[[ "$status" == *'registered=1 initComplete=1 initStatus=0'* ]] || exit 0
join=$("$loader" --connect-status 2>/dev/null) || exit 0
if [[ "$join" == *'associated=1'* ]]; then
    /sbin/ifconfig en0 up 2>/dev/null || exit 0
    address=$(/usr/sbin/ipconfig getifaddr en0 2>/dev/null) || address=''
    [[ -n "$address" ]] || /usr/sbin/ipconfig set en0 DHCP >/dev/null 2>&1 || true
    exit 0
fi
if [[ "$join" =~ connectPhase=([0-9]+) ]]; then
    phase=${BASH_REMATCH[1]}
    (( phase >= 1 && phase <= 10 )) && exit 0
fi
# The secret is passed over a pipe and is never put in command arguments.
"$loader" --connect "$ssid" < "$config/passphrase" >/dev/null 2>&1 || true
