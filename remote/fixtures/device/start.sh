#!/bin/sh
# What update_networking.sh does when "Allow remote access" is on: dropbear + the tunnel loop.
dropbear -R -B -E
echo "$RELAY_CODE" > /mnt/SDCARD/.tmp_update/config/remote_relay
touch /mnt/SDCARD/.tmp_update/config/.remoteState
exec /bin/sh /mnt/SDCARD/.tmp_update/script/network/remote_tunnel.sh
