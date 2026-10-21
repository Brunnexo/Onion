#!/bin/sh
# Fetch one ROM.
# Usage: rom_fetch.sh <http|torrent> <url> <dest-dir> <filename> <path-in-torrent> <extract 0|1>
. /mnt/SDCARD/.tmp_update/script/romdl_common.sh
kind=$1 url=$2 dest=$3 name=$4 tpath=$5 extract=$6
[ -n "$url" ] && [ -n "$dest" ] && [ -n "$name" ] || { echo "bad arguments"; exit 1; }
mkdir -p "$dest"

enable_wifi || exit 2

if [ "$kind" = "torrent" ]; then
    "$sysdir/script/torrent_dl.sh" "$url" "$dest" "$tpath" || exit $?
else
    echo "Downloading $name"
    curl -ksfL --retry 3 -A "Mozilla/5.0" -e "https://archive.org/" -o "$dest/$name.part" "$url" &
    watch_size "$dest/$name.part" $!
    wait $! || { rm -f "$dest/$name.part"; echo "Download failed"; exit 1; }
    mv -f "$dest/$name.part" "$dest/$name"
fi

if [ "$extract" = "1" ]; then
    case "$name" in
        *.zip | *.7z)
            echo "Extracting..."
            "$sysdir/bin/7z" x -y -o"$dest" "$dest/$name" > /dev/null && rm -f "$dest/$name"
            ;;
    esac
fi
# MainUI lists ROMs from a per-folder cache: drop it so the new file shows up
# (same files reset_list.sh removes).
rm -f "$dest/$(basename "$dest")_cache2.db" "$dest/$(basename "$dest")_cache6.db" 2> /dev/null
echo "Saved to ${dest#/mnt/SDCARD/}"
