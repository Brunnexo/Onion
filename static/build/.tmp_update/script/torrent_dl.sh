#!/bin/sh
# Download one file out of a torrent (or a whole torrent) with aria2c.
#
# Usage:
#   torrent_dl.sh <torrent-url|torrent-file|magnet> <dest-dir> [file-path-in-torrent]
#
# <file-path-in-torrent> is the path as listed inside the torrent
# (e.g. "No-Intro/Nintendo - Game Boy Advance/Game.zip"). Omit to fetch all.
# The selected file ends up directly in <dest-dir>. Exit 0 on success.
sysdir=/mnt/SDCARD/.tmp_update
aria2c="$sysdir/bin/aria2c"
logfile=torrent_dl
. "$sysdir/script/log.sh"

src="$1"
dest="$2"
want="$3"
[ -n "$src" ] && [ -n "$dest" ] || { echo "usage: $0 <torrent|magnet> <dest-dir> [file-in-torrent]"; exit 1; }
mkdir -p "$dest"

tmp="$dest/.torrent_dl.$$"
mkdir -p "$tmp"
cleanup() { rm -rf "$tmp"; }
trap cleanup EXIT INT TERM

# Common aria2 options: no seeding, no preallocation (vfat), quiet-ish progress
# ponytail: no DHT bootstrap persistence; add --dht-file-path if magnets prove slow.
opts="--dir=$tmp --seed-time=0 --file-allocation=none --bt-remove-unselected-file=true \
--bt-stop-timeout=300 --max-connection-per-server=4 --summary-interval=1 \
--console-log-level=warn --check-certificate=false --continue=true"

# Resolve source to a local .torrent (magnets go straight through)
case "$src" in
    magnet:*) torrent="$src" ;;
    http://*|https://*)
        torrent="$tmp/src.torrent"
        log "fetching torrent $src"
        curl -sfL --retry 3 -o "$torrent" "$src" || { echo "Failed to fetch torrent"; exit 2; }
        ;;
    *) torrent="$src" ;;
esac

select=""
if [ -n "$want" ]; then
    case "$torrent" in
        magnet:*) echo "File selection needs a .torrent, not a magnet"; exit 1 ;;
    esac
    # aria2c -S prints "idx|path" lines (1-based idx); match on path suffix
    idx=$("$aria2c" -S "$torrent" | awk -F'|' -v w="$want" '
        $1 ~ /^ *[0-9]+$/ { p=$2; sub(/^\.\//,"",p); if (p==w || substr(p,length(p)-length(w)+1)==w) { gsub(/ /,"",$1); print $1; exit } }')
    [ -n "$idx" ] || { echo "File not found in torrent: $want"; exit 3; }
    select="--select-file=$idx"
    log "selected index $idx for $want"
fi

# shellcheck disable=SC2086
"$aria2c" $opts $select "$torrent"
rc=$?
[ $rc -eq 0 ] || { echo "aria2c failed ($rc)"; exit $rc; }

# Move the result(s) into dest (keep only wanted file when selecting)
if [ -n "$want" ]; then
    f=$(find "$tmp" -type f -name "$(basename "$want")" | head -n 1)
    [ -n "$f" ] || { echo "Downloaded file missing"; exit 4; }
    mv -f "$f" "$dest/"
    log "done: $dest/$(basename "$want")"
else
    find "$tmp" -mindepth 1 -maxdepth 1 ! -name 'src.torrent' -exec mv -f {} "$dest/" \;
fi
exit 0
