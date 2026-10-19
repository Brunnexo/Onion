#!/bin/sh
# Download the pre-filtered romgi catalog for Onion (see github.com/Brunnexo/onion-romdb).
. /mnt/SDCARD/.tmp_update/script/romdl_common.sh
dbdir="$sysdir/romdb"
catalog_url=$(cat "$sysdir/config/romdl_catalog_url" 2>/dev/null)
[ -n "$catalog_url" ] || catalog_url=https://raw.githubusercontent.com/Brunnexo/onion-romdb/main
mkdir -p "$dbdir"
cd "$dbdir" || exit 1

enable_wifi || exit 2

echo "Checking catalog version..."
remote=$(curl -ksfL "$catalog_url/version.json" | "$sysdir/bin/jq" -r .version) || { echo "Cannot reach catalog"; exit 2; }
local=$(cat version 2>/dev/null)
if [ "$remote" = "$local" ] && [ -f romdb.db ]; then
    echo "Catalog is up to date ($local)"
    exit 0
fi

echo "Downloading catalog $remote..."
rm -f romdb.db.gz romdb.db.new
curl -ksfL -o romdb.db.gz "$catalog_url/romdb.db.gz" &
watch_size romdb.db.gz $!
wait $! || { rm -f romdb.db.gz; echo "Download failed"; exit 1; }

echo "Extracting..."
gzip -dc romdb.db.gz > romdb.db.new || { rm -f romdb.db.new; echo "Extract failed"; exit 1; }
rm -f romdb.db.gz
mv -f romdb.db.new romdb.db
echo "$remote" > version
echo "Catalog $remote ready ($(du -m romdb.db | cut -f1) MB)"
