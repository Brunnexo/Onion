# Fake Miyoo: dropbear like Onion runs it (-R -B: root with a blank password),
# onion/onion, OpenSSH sftp-server and the real remote_tunnel.sh from static/.
# Build/run from the repo root (needs the relay container on the onion-test network):
#   docker build -t onion-fake-device -f remote/fixtures/device/Dockerfile .
#   docker run -d --name onion-device --network onion-test -p 2201:22 \
#       -e RELAY_CODE='onion:relaypass@onion-relay:22/2222' onion-fake-device
FROM alpine:3.20
RUN apk add --no-cache dropbear dropbear-dbclient openssh-sftp-server && passwd -d root \
 && adduser -D onion && echo 'onion:onion' | chpasswd \
 && mkdir -p /etc/dropbear /mnt/SDCARD/Roms/GB /mnt/SDCARD/Saves /mnt/SDCARD/.tmp_update/config \
      /mnt/SDCARD/.tmp_update/onionVersion /mnt/SDCARD/.tmp_update/bin /mnt/SDCARD/.tmp_update/logs \
 && echo 'v4.4.0-fake' > /mnt/SDCARD/.tmp_update/onionVersion/version.txt \
 && head -c 300000 /dev/urandom > '/mnt/SDCARD/Roms/GB/Tetris (World).gb' \
 && touch /mnt/SDCARD/.tmp_update/config/.logging && chmod -R a+rwX /mnt/SDCARD
COPY static/build/.tmp_update/script/log.sh /mnt/SDCARD/.tmp_update/script/log.sh
COPY static/build/.tmp_update/script/network/remote_tunnel.sh /mnt/SDCARD/.tmp_update/script/network/remote_tunnel.sh
COPY remote/fixtures/device/start.sh /start.sh
CMD ["/bin/sh", "/start.sh"]
