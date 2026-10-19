#!/bin/sh
# Shared helpers for the ROM Downloader scripts (sourced, not executed).
sysdir=/mnt/SDCARD/.tmp_update

# Bring Wi-Fi up if there is no route yet (same sequence as ota_update.sh).
enable_wifi() {
    if [ -n "$(ip route get 1 2>/dev/null | awk '{print $NF;exit}')" ]; then
        return 0
    fi
    echo "Enabling Wi-Fi..."
    insmod /mnt/SDCARD/8188fu.ko 2>/dev/null
    ifconfig lo up
    /customer/app/axp_test wifion
    sleep 2
    ifconfig wlan0 up
    wpa_supplicant -B -D nl80211 -iwlan0 -c /appconfigs/wpa_supplicant.conf
    udhcpc -i wlan0 -s /etc/init.d/udhcpc.script
    sleep 3
    [ -n "$(ip route get 1 2>/dev/null | awk '{print $NF;exit}')" ] && return 0
    echo "No network connection"
    return 1
}

# Print growing size of $1 every second while pid $2 is alive.
watch_size() {
    while kill -0 "$2" 2>/dev/null; do
        s=$(stat -c %s "$1" 2>/dev/null || echo 0)
        echo "$((s / 1048576)) MB"
        sleep 1
    done
}
