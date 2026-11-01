#!/bin/sh
i=0
while [ "$i" -lt 600 ]; do
    printf '%s ' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    /MarvellW8897/marvell_fwupload --connect-status 2>&1 | tr '\n' ' '
    /MarvellW8897/marvell_fwupload --status 2>&1 | tr '\n' ' '
    /sbin/ifconfig en0 2>&1 | grep 'status:' | tr '\n' ' '
    gateway=$(/sbin/route -n get default 2>/dev/null | /usr/bin/awk '/gateway:/{print $2; exit}')
    printf 'gateway='
    if [ -n "$gateway" ] && /sbin/ping -q -c 1 -W 1000 "$gateway" >/dev/null 2>&1; then
        printf 'ok\n'
    else
        printf 'fail\n'
    fi
    i=$((i + 1))
    sleep 3
done
