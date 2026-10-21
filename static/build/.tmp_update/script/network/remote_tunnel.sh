#!/bin/sh
# Keeps a reverse SSH tunnel to the relay in .tmp_update/config/remote_relay
# (user:password@host[:port][/tunnelport]) so the device is reachable from the
# Internet without port forwarding: the relay's 127.0.0.1:<tunnelport> leads to
# this device's dropbear. Started/stopped by update_networking.sh (remote toggle).
sysdir=/mnt/SDCARD/.tmp_update
export PATH="$sysdir/bin:$PATH"
conf=$sysdir/config/remote_relay

logfile=remote_tunnel
. $sysdir/script/log.sh

while [ -f $sysdir/config/.remoteState ] && [ -f "$conf" ]; do
    code=$(head -n 1 "$conf" | tr -d '\r')
    tport=${code##*/}
    [ "$tport" = "$code" ] && tport=2222
    code=${code%/*}
    auth=${code%@*}
    hp=${code#*@}
    user=${auth%%:*}
    pass=${auth#*:}
    host=${hp%%:*}
    port=${hp#*:}
    [ "$port" = "$hp" ] && port=22

    log "Remote: connecting to $user@$host:$port, tunnel port $tport"
    DROPBEAR_PASSWORD="$pass" dbclient -y -y -N -K 20 -o ExitOnForwardFailure=yes \
        -p "$port" -R "$tport:127.0.0.1:22" "$user@$host"
    log "Remote: tunnel closed (exit $?), retrying in 15s"
    sleep 15
done
log "Remote: tunnel loop ended"
