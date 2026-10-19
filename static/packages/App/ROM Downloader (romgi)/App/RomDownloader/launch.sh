#!/bin/sh
touch /tmp/stay_awake
cd /mnt/SDCARD/.tmp_update
./bin/romDownloader
rm -f /tmp/stay_awake
