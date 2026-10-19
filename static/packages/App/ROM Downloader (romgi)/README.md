# ROM Downloader (romgi)

Graphical ROM browser/downloader for Onion, backed by the
[romgi](https://github.com/caprado/romgi) catalog (SQLite). HTTP links are
fetched with `curl`; torrent links with the bundled static `aria2c`
(`.tmp_update/script/torrent_dl.sh`, single-file selection, no seeding).

## Files

| Path | Role |
|---|---|
| `src/romDownloader/` | SDL app (`romDownloader.c`, platform map in `platforms.h`) |
| `.tmp_update/script/romdb_update.sh` | Downloads `romdb.db.gz`, trims to supported platforms, VACUUMs |
| `.tmp_update/script/rom_fetch.sh` | Fetches one ROM (http or torrent), optional 7z extract |
| `.tmp_update/script/romdl_common.sh` | Wi-Fi bring-up + size watcher, sourced by both |
| `.tmp_update/romdb/romdb.db` | Trimmed catalog (created on device) |

The GUI runs each script detached (`setsid`), reads `/tmp/romdl_status` for
the dialog text and `/tmp/romdl_done` for the exit code. B cancels a download
by killing the script's process group.

## Controls

- Platforms: A open, A on "Update catalog" refreshes the DB, B/MENU quit.
- Games: Up/Down move, L1/R1 jump letter, L2/R2 page (50 per page), A open.
- Links: A then A to confirm download into `Roms/<code>`.

## Manual device test (cannot run from a PC)

1. Build: `make with-toolchain CMD="core"` (or just `cd src/romDownloader && make` inside the toolchain) and copy `build/.tmp_update/bin/romDownloader`, the three scripts, `bin/aria2c` and `script/torrent_dl.sh` to the SD card.
2. Install the package from Package Manager > Apps > "ROM Downloader (romgi)".
3. With Wi-Fi configured, launch the app. First run shows "Catalog update" and
   downloads ~55 MB, extracts (~345 MB temp), trims. Expect several minutes;
   `du -m .tmp_update/romdb/romdb.db` afterwards should be well under 100 MB.
4. Open "Game Boy Advance", R1 a few times to jump letters, pick a small game,
   choose the torrent link, confirm. Dialog should show aria2 progress
   (`[#... DL:...]`) and end with "Done". Check `Roms/GBA/<file>.zip`.
5. Open "PlayStation", pick a game with an http link (Internet Archive),
   confirm. Shows "N MB" growing, then "Extracting...", ends with "Done".
   Check `Roms/PS/` has the bin/cue (zip removed).
6. Press B during a download: dialog closes, `pgrep aria2c` / `pgrep curl`
   should be empty, partial files stay in `Roms/<code>/.torrent_dl.*` or `*.part`
   only until the next run.
7. Logs: enable `.tmp_update/config/.logging`, see `logs/torrent_dl.log`.
