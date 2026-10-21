#!/bin/sh
# Runs in the background the first time Wi-Fi comes up in a session: checks
# for an OTA update and, if there is one, asks the user whether to install it.
# The question is shown through cmd_to_run.sh so it runs on the framebuffer
# between MainUI sessions instead of fighting MainUI for the screen.
sysdir=/mnt/SDCARD/.tmp_update

[ -f /tmp/ota_prompted ] && exit 0
"$sysdir/script/ota_update.sh" check || exit 0
touch /tmp/ota_prompted

cmd='LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so /mnt/SDCARD/.tmp_update/bin/prompt -t "Atualização do Onion" -m "Há uma nova versão disponível.\nAtualizar agora?" "Atualizar" "Depois" && { touch /tmp/stay_awake; cd /mnt/SDCARD/.tmp_update/script && st -q -e sh ota_update.sh; }'

if pgrep -x MainUI > /dev/null; then
    # runtime.sh moves /tmp/cmd_to_run.sh into place when MainUI exits
    echo "$cmd" > /tmp/cmd_to_run.sh
    pkill -2 MainUI
elif [ -f /tmp/is_booting ]; then
    # still booting: MainUI has not started yet, runtime.sh runs this first
    echo "$cmd" > "$sysdir/cmd_to_run.sh"
fi
# otherwise a game or app is running: do not interrupt it
