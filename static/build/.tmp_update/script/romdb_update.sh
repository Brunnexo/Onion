#!/bin/sh
# Download the romgi catalog and trim it to the given platform ids.
# Usage: romdb_update.sh <platform-id>...
. /mnt/SDCARD/.tmp_update/script/romdl_common.sh
dbdir="$sysdir/romdb"
raw=https://raw.githubusercontent.com/caprado/romgi/main/db
mkdir -p "$dbdir"
cd "$dbdir" || exit 1

enable_wifi || exit 2

echo "Checking catalog version..."
remote=$(curl -ksfL "$raw/version.json" | "$sysdir/bin/jq" -r .version) || { echo "Cannot reach catalog"; exit 2; }
local=$(cat version 2>/dev/null)
if [ "$remote" = "$local" ] && [ -f romdb.db ]; then
    echo "Catalog is up to date ($local)"
    exit 0
fi

echo "Downloading catalog $remote..."
rm -f romdb.db.gz romdb.db.new
curl -ksfL -o romdb.db.gz "$raw/romdb.db.gz" &
watch_size romdb.db.gz $!
wait $! || { rm -f romdb.db.gz; echo "Download failed"; exit 1; }

echo "Extracting (takes a few minutes)..."
gzip -dc romdb.db.gz > romdb.db.new || { rm -f romdb.db.new; echo "Extract failed"; exit 1; }
rm -f romdb.db.gz

echo "Trimming catalog..."
# sqlite3 puts VACUUM/sort temp files in /tmp (tmpfs, RAM) by default: too small for 345 MB.
export SQLITE_TMPDIR="$dbdir" TMPDIR="$dbdir"
ids=""
for p in "$@"; do ids="$ids'$p',"; done
ids=${ids%,}
sql="$sysdir/bin/sqlite3"
err=$("$sql" romdb.db.new "PRAGMA temp_store_directory='$dbdir';
DELETE FROM entries WHERE platform NOT IN ($ids);
DELETE FROM links WHERE entry NOT IN (SELECT slug FROM entries);
DELETE FROM regions_entries WHERE entry NOT IN (SELECT slug FROM entries);
DELETE FROM entry_group_members WHERE entry NOT IN (SELECT slug FROM entries);
DELETE FROM entry_groups WHERE id NOT IN (SELECT group_id FROM entry_group_members);
DELETE FROM torrents WHERE infohash NOT IN (SELECT torrent_infohash FROM links WHERE torrent_infohash IS NOT NULL);" 2>&1)
if [ -n "$err" ]; then
    echo "$err" | tee -a update.log
    echo "Trim failed, keeping full catalog"
else
    # FTS table needs the fts4 module; drop it separately so a missing module is not fatal.
    "$sql" romdb.db.new "DROP TABLE IF EXISTS entries_fts;" 2>&1 | tee -a update.log
    err=$("$sql" romdb.db.new "PRAGMA temp_store_directory='$dbdir'; VACUUM;" 2>&1)
    [ -n "$err" ] && { echo "$err" | tee -a update.log; echo "Vacuum failed, catalog kept untrimmed on disk"; }
fi
rm -f etilqs_* 2>/dev/null

mv -f romdb.db.new romdb.db
echo "$remote" > version
echo "Catalog $remote ready ($(du -m romdb.db | cut -f1) MB)"
