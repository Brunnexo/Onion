#!/bin/sh
# Runs in the background the first time Wi-Fi comes up in a session: waits for
# a route, checks for an OTA update and, if there is one, asks the user whether
# to install it. The question goes through cmd_to_run.sh so it runs on the
# framebuffer between MainUI sessions instead of fighting MainUI for the screen.
sysdir=/mnt/SDCARD/.tmp_update
logfile="$sysdir/logs/ota_notify.log"

log() { echo "$(date '+%H:%M:%S') $*" >> "$logfile"; }

[ -f /tmp/ota_prompted ] && exit 0
: > "$logfile"

# Wi-Fi is "enabled" before DHCP finishes: give it up to 90 s to get a route.
i=0
while [ -z "$(ip route get 1 2> /dev/null | awk '{print $NF; exit}')" ]; do
    i=$((i + 1))
    [ $i -ge 30 ] && { log "no route after 90s, giving up"; exit 0; }
    sleep 3
done
log "network up after $((i * 3))s"

if ! "$sysdir/script/ota_update.sh" check >> "$logfile" 2>&1; then
    log "no update"
    exit 0
fi
touch /tmp/ota_prompted
log "update available"

cmd='LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so /mnt/SDCARD/.tmp_update/bin/prompt -t "Atualização do Onion" -m "Há uma nova versão disponível.\nAtualizar agora?" "Atualizar" "Depois" && { touch /tmp/stay_awake; cd /mnt/SDCARD/.tmp_update/script && st -q -e sh ota_update.sh; }'

if pgrep MainUI > /dev/null; then
    # runtime.sh moves /tmp/cmd_to_run.sh into place when MainUI exits
    echo "$cmd" > /tmp/cmd_to_run.sh
    log "prompting (MainUI running)"
    pkill -2 MainUI
elif [ -f /tmp/is_booting ]; then
    # still booting: MainUI has not started yet, runtime.sh runs this first
    echo "$cmd" > "$sysdir/cmd_to_run.sh"
    log "prompting (boot)"
else
    log "game or app running, not interrupting"
fi
