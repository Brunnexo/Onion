// ROM Downloader: browse the romgi catalog (SQLite) and fetch ROMs over
// HTTP (curl) or BitTorrent (aria2c via torrent_dl.sh) on the Miyoo Mini.
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3/sqlite3.h>

#include "components/kbinput_wrapper.h"
#include "components/list.h"
#include "system/display.h"
#include "system/keymap_sw.h"
#include "system/lang.h"
#include "system/settings.h"
#include "theme/background.h"
#include "theme/theme.h"
#include "utils/file.h"
#include "utils/keystate.h"
#include "utils/log.h"
#include "utils/sdl_init.h"

#include "./bios.h"
#include "./platforms.h"

#define SYSDIR "/mnt/SDCARD/.tmp_update"
#define DB_PATH SYSDIR "/romdb/romdb.db"
#define ROMS_DIR "/mnt/SDCARD/Roms"
#define STATUS_FILE "/tmp/romdl_status"
#define DONE_FILE "/tmp/romdl_done"
#define PID_FILE "/tmp/romdl_pid"
#define JOB_FILE "/tmp/romdl_job.sh"
#define PAGE_SIZE 50
#define MAX_LINKS 32

typedef struct {
    char name[STR_MAX];
    char url[1024];
    char filename[STR_MAX];
    char size_str[32];
    char torrent_path[1024];
    long long size;
} Link;

// Download progress parsed from "P:<bytes>" status lines.
static long long prog_done = 0, prog_total = 0, prog_prev = 0;
static double prog_speed = 0; // bytes/s
static uint32_t prog_prev_ticks = 0;

enum { SCREEN_PLATFORMS,
       SCREEN_GAMES,
       SCREEN_LINKS,
       SCREEN_BIOS };
enum { JOB_NONE,
       JOB_CATALOG,
       JOB_DOWNLOAD };

static sqlite3 *db = NULL;
static List list_platforms, list_games, list_links, list_bios;
static int screen_level = SCREEN_PLATFORMS;
static const Platform *cur_platform = NULL;
static char cur_platform_name[STR_MAX];
static int cur_page = 0, cur_total = 0;
static char cur_search[STR_MAX] = "";
static char cur_pattern[STR_MAX + 2] = "%"; // LIKE pattern: %<cur_search>%
static char cur_slug[STR_MAX], cur_title[STR_MAX];
static Link links[MAX_LINKS];
static int job = JOB_NONE;
static char job_title[STR_MAX];
static bool confirm_open = false;
static bool result_open = false;
static char result_msg[STR_MAX * 2];
static KeyState keystate[320] = {RELEASED};
static bool quit = false;

static void sigHandler(int sig) { quit = true; }

// ---------------------------------------------------------------- shell ---

// Single-quote `in` for /bin/sh.
static void shell_quote(char *out, size_t out_size, const char *in)
{
    size_t n = 0;
    out[n++] = '\'';
    for (; *in && n < out_size - 6; in++) {
        if (*in == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        }
        else
            out[n++] = *in;
    }
    out[n++] = '\'';
    out[n] = '\0';
}

// Run `cmd` detached in its own process group; status lines go to STATUS_FILE,
// exit code to DONE_FILE when finished.
static void job_start(int kind, const char *title, const char *cmd)
{
    remove(STATUS_FILE);
    remove(DONE_FILE);
    // The command carries its own quoting, so it goes into a script file
    // instead of being nested inside another quoted sh -c string.
    FILE *fp = fopen(JOB_FILE, "w");
    if (!fp)
        return;
    fprintf(fp, "#!/bin/sh\n%s\n", cmd);
    fclose(fp);
    // No setsid: the stock busybox may not have it. Cancel kills the helpers by name.
    system("sh -c 'echo $$ > " PID_FILE "; sh " JOB_FILE " > " STATUS_FILE " 2>&1; echo $? > " DONE_FILE "' &");
    job = kind;
    strncpy(job_title, title, STR_MAX - 1);
    theme_clearDialogProgress();
}

static void job_cancel(void)
{
    system("kill $(cat " PID_FILE " 2>/dev/null) 2>/dev/null; "
           "pkill -f romdl_job.sh; pkill -f rom_fetch.sh; pkill -f torrent_dl.sh; pkill -f romdb_update.sh; "
           "pkill aria2c; pkill curl; rm -f /mnt/SDCARD/Roms/*/*.part");
    job = JOB_NONE;
}

// Last useful status line: prefer aria2's "[#..." progress line among the
// tail, else the last non-empty line.
static void job_status(char *out, size_t out_size)
{
    out[0] = '\0';
    FILE *fp = fopen(STATUS_FILE, "r");
    if (!fp)
        return;
    char line[512], last[512] = "", aria[512] = "";
    long long done = -1;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line + strlen(line);
        while (p > line && (p[-1] == '\n' || p[-1] == '\r'))
            *--p = '\0';
        if (line[0] == '\0')
            continue;
        if (line[0] == 'P' && line[1] == ':') {
            done = atoll(line + 2);
            continue;
        }
        strcpy(last, line);
        if (line[0] == '[' && line[1] == '#')
            strcpy(aria, line);
        else if (strncmp(line, "FILE:", 5) != 0 && line[0] != '-' && line[0] != '=' && line[0] != ' ')
            aria[0] = '\0';
    }
    fclose(fp);
    if (done >= 0) {
        uint32_t now = SDL_GetTicks();
        if (prog_prev_ticks && now > prog_prev_ticks && done >= prog_prev)
            prog_speed = 0.7 * prog_speed + 0.3 * ((done - prog_prev) * 1000.0 / (now - prog_prev_ticks));
        prog_prev = done;
        prog_prev_ticks = now;
        prog_done = done;
    }
    strncpy(out, aria[0] ? aria : last, out_size - 1);
    out[out_size - 1] = '\0';
    if (strlen(out) > 36)
        strcpy(out + 33, "...");
}

static void fmt_mb(char *out, size_t n, long long bytes)
{
    snprintf(out, n, "%.1f MB", bytes / 1048576.0);
}

static void catalog_update(void)
{
    job_start(JOB_CATALOG, "Catalog update", "sh " SYSDIR "/script/romdb_update.sh");
}

// ---------------------------------------------------------------- bios ---

static bool bios_installed(const Bios *b)
{
    char path[STR_MAX];
    snprintf(path, sizeof(path), BIOS_DIR "/%s", b->file);
    return exists(path);
}

// `sh rom_fetch.sh http <url> <dest-dir> <file> '' 0` for one BIOS file.
static void bios_fetch_cmd(char *out, size_t out_size, const Bios *b)
{
    char url[512], dest[STR_MAX], name[STR_MAX], q_url[1024], q_dest[STR_MAX * 2], q_name[STR_MAX * 2];
    // archive.org accepts the file name as-is except for spaces
    size_t n = snprintf(url, sizeof(url), "%s", BIOS_URL);
    for (const char *p = b->remote; *p && n < sizeof(url) - 4; p++) {
        if (*p == ' ') {
            memcpy(url + n, "%20", 3);
            n += 3;
        }
        else
            url[n++] = *p;
    }
    url[n] = '\0';
    const char *slash = strrchr(b->file, '/');
    if (slash) {
        snprintf(dest, sizeof(dest), BIOS_DIR "/%.*s", (int)(slash - b->file), b->file);
        snprintf(name, sizeof(name), "%s", slash + 1);
    }
    else {
        strcpy(dest, BIOS_DIR);
        snprintf(name, sizeof(name), "%s", b->file);
    }
    shell_quote(q_url, sizeof(q_url), url);
    shell_quote(q_dest, sizeof(q_dest), dest);
    shell_quote(q_name, sizeof(q_name), name);
    snprintf(out, out_size, "sh " SYSDIR "/script/rom_fetch.sh http %s %s %s '' 0", q_url, q_dest, q_name);
}

static void build_bios(void)
{
    list_free(&list_bios);
    list_bios = list_createWithTitle(BIOS_COUNT + 1, LIST_SMALL, "Emulator BIOS");
    ListItem all = {.action_id = -1};
    strcpy(all.label, "Download all missing");
    list_addItem(&list_bios, all);
    for (int i = 0; i < BIOS_COUNT; i++) {
        ListItem item = {.action_id = i};
        snprintf(item.label, STR_MAX, "%s: %s  [%s]", BIOS_FILES[i].system, BIOS_FILES[i].file,
                 lang_tr(bios_installed(&BIOS_FILES[i]) ? "installed" : "missing"));
        list_addItem(&list_bios, item);
    }
}

// idx >= 0: that file (even if present); idx < 0: every missing file in one job.
static void bios_download(int idx)
{
    char cmd[8192] = "", one[2048];
    size_t n = 0;
    prog_total = 0;
    if (idx >= 0) {
        bios_fetch_cmd(cmd, sizeof(cmd), &BIOS_FILES[idx]);
        prog_total = BIOS_FILES[idx].size;
    }
    else {
        for (int i = 0; i < BIOS_COUNT; i++) {
            if (bios_installed(&BIOS_FILES[i]))
                continue;
            bios_fetch_cmd(one, sizeof(one), &BIOS_FILES[i]);
            n += snprintf(cmd + n, sizeof(cmd) - n, "%s%s", n ? "; " : "", one);
            if (n >= sizeof(cmd) - 1)
                break;
        }
        if (n == 0)
            return; // nothing missing
    }
    prog_done = prog_prev = 0;
    prog_speed = 0;
    prog_prev_ticks = 0;
    job_start(JOB_DOWNLOAD, "Downloading BIOS", cmd);
}

static void download_start(Link *link)
{
    char q_url[2048], q_dest[STR_MAX * 2], q_name[STR_MAX * 2], q_tpath[2048], cmd[8192];
    char dest[STR_MAX];
    snprintf(dest, sizeof(dest), ROMS_DIR "/%s", cur_platform->code);
    shell_quote(q_url, sizeof(q_url), link->url);
    shell_quote(q_dest, sizeof(q_dest), dest);
    shell_quote(q_name, sizeof(q_name), link->filename);
    shell_quote(q_tpath, sizeof(q_tpath), link->torrent_path);
    snprintf(cmd, sizeof(cmd), "sh " SYSDIR "/script/rom_fetch.sh %s %s %s %s %s %d",
             link->torrent_path[0] ? "torrent" : "http", q_url, q_dest, q_name, q_tpath,
             cur_platform->extract ? 1 : 0);
    prog_done = prog_prev = 0;
    prog_total = link->size;
    prog_speed = 0;
    prog_prev_ticks = 0;
    job_start(JOB_DOWNLOAD, "Downloading", cmd);
}

// ------------------------------------------------------------------- db ---

static bool db_open(void)
{
    if (db)
        sqlite3_close(db);
    db = NULL;
    if (!exists(DB_PATH))
        return false;
    if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        print_debug("sqlite open failed");
        sqlite3_close(db);
        db = NULL;
        return false;
    }
    return true;
}

static int db_count(const char *sql, const char *p1, const char *p2, const char *p3)
{
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    if (p1)
        sqlite3_bind_text(st, 1, p1, -1, SQLITE_TRANSIENT);
    if (p2)
        sqlite3_bind_text(st, 2, p2, -1, SQLITE_TRANSIENT);
    if (p3)
        sqlite3_bind_text(st, 3, p3, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void build_platforms(void)
{
    list_free(&list_platforms);
    list_platforms = list_createWithTitle(PLATFORM_COUNT + 2, LIST_SMALL, "ROM Downloader");
    ListItem update = {.action_id = -1};
    strcpy(update.label, db ? "Update catalog" : "Download catalog");
    list_addItem(&list_platforms, update);
    ListItem bios = {.action_id = -2};
    strcpy(bios.label, "Emulator BIOS");
    list_addItem(&list_platforms, bios);
    if (!db)
        return;
    sqlite3_stmt *st;
    const char *sql = "SELECT p.name, (SELECT COUNT(*) FROM entries e WHERE e.platform = p.id) FROM platforms p WHERE p.id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    for (int i = 0; i < PLATFORM_COUNT; i++) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, PLATFORMS[i].id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 1) == 0)
            continue;
        ListItem item = {.action_id = i};
        snprintf(item.label, STR_MAX, "%s (%d)", sqlite3_column_text(st, 0), sqlite3_column_int(st, 1));
        list_addItem(&list_platforms, item);
    }
    sqlite3_finalize(st);
}

static void load_games_page(int page)
{
    cur_page = page;
    list_free(&list_games);
    list_games = list_createWithTitle(PAGE_SIZE, LIST_SMALL, "");
    int pages = (cur_total + PAGE_SIZE - 1) / PAGE_SIZE;
    if (cur_search[0])
        snprintf(list_games.title, STR_MAX, "%.120s \"%.60s\"  %d/%d", cur_platform_name, cur_search, page + 1, pages > 0 ? pages : 1);
    else
        snprintf(list_games.title, STR_MAX, "%.200s  %d/%d", cur_platform_name, page + 1, pages > 0 ? pages : 1);

    sqlite3_stmt *st;
    const char *sql =
        "SELECT e.slug, e.title, (SELECT group_concat(r.name, ', ') FROM regions_entries re "
        "JOIN regions r ON r.id = re.region WHERE re.entry = e.slug) "
        "FROM entries e WHERE e.platform = ? AND e.title LIKE ? ORDER BY e.title COLLATE NOCASE, e.slug LIMIT ? OFFSET ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, cur_platform->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, cur_pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, PAGE_SIZE);
    sqlite3_bind_int(st, 4, page * PAGE_SIZE);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ListItem item = {0};
        const char *regions = (const char *)sqlite3_column_text(st, 2);
        strncpy(item.payload, (const char *)sqlite3_column_text(st, 0), STR_MAX - 1);
        if (regions && regions[0])
            snprintf(item.label, STR_MAX, "%s [%s]", sqlite3_column_text(st, 1), regions);
        else
            strncpy(item.label, (const char *)sqlite3_column_text(st, 1), STR_MAX - 1);
        list_addItem(&list_games, item);
    }
    sqlite3_finalize(st);
}

static void open_platform(int idx)
{
    cur_platform = &PLATFORMS[idx];
    sqlite3_stmt *st;
    strcpy(cur_platform_name, cur_platform->code);
    if (sqlite3_prepare_v2(db, "SELECT name FROM platforms WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, cur_platform->id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            strncpy(cur_platform_name, (const char *)sqlite3_column_text(st, 0), STR_MAX - 1);
        sqlite3_finalize(st);
    }
    cur_search[0] = '\0';
    strcpy(cur_pattern, "%");
    cur_total = db_count("SELECT COUNT(*) FROM entries WHERE platform = ? AND title LIKE ?", cur_platform->id, cur_pattern, NULL);
    load_games_page(0);
    screen_level = SCREEN_GAMES;
}

// SELECT on the game list: on-screen keyboard, then filter titles by substring.
static void games_search(void)
{
    const char *typed = launch_keyboard(cur_search, "Buscar");
    // libkbinput re-creates the SDL video surface: take the new one and
    // restore our key-repeat settings.
    video = SDL_SetVideoMode(g_display.width, g_display.height, 32, SDL_HWSURFACE);
    SDL_EnableKeyRepeat(300, 50);
    // The keyboard consumed the key-up events, so every key we saw pressed
    // would stay PRESSED and re-trigger on the next event: reset all of them.
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        ;
    memset(keystate, RELEASED, sizeof(keystate));
    if (!typed)
        return;
    strncpy(cur_search, typed, STR_MAX - 1);
    snprintf(cur_pattern, sizeof(cur_pattern), "%%%s%%", cur_search);
    cur_total = db_count("SELECT COUNT(*) FROM entries WHERE platform = ? AND title LIKE ?", cur_platform->id, cur_pattern, NULL);
    load_games_page(0);
}

static int games_index(void) { return cur_page * PAGE_SIZE + list_games.active_pos; }

static void games_goto(int idx)
{
    if (cur_total == 0)
        return;
    idx = ((idx % cur_total) + cur_total) % cur_total;
    if (idx / PAGE_SIZE != cur_page)
        load_games_page(idx / PAGE_SIZE);
    list_scrollTo(&list_games, idx % PAGE_SIZE);
}

// Upper-cased first character of the title at absolute index `idx`.
static char games_letter_at(int idx)
{
    sqlite3_stmt *st;
    char c = 0;
    const char *sql = "SELECT upper(substr(title, 1, 1)) FROM entries WHERE platform = ? AND title LIKE ? "
                      "ORDER BY title COLLATE NOCASE, slug LIMIT 1 OFFSET ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, cur_platform->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, cur_pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, idx);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        c = sqlite3_column_text(st, 0)[0];
    sqlite3_finalize(st);
    return c;
}

// Absolute index of the first title whose first letter is >= `c` (> if `after`).
static int games_letter_index(char c, bool after)
{
    char letter[2] = {c, 0};
    return db_count(after ? "SELECT COUNT(*) FROM entries WHERE platform = ? AND upper(substr(title, 1, 1)) <= ? AND title LIKE ?"
                          : "SELECT COUNT(*) FROM entries WHERE platform = ? AND upper(substr(title, 1, 1)) < ? AND title LIKE ?",
                    cur_platform->id, letter, cur_pattern);
}

static void games_next_letter(void)
{
    int cur = games_index();
    games_goto(games_letter_index(games_letter_at(cur), true));
}

static void games_prev_letter(void)
{
    int cur = games_index();
    int first = games_letter_index(games_letter_at(cur), false);
    if (first == cur)
        first = games_letter_index(games_letter_at(first == 0 ? cur_total - 1 : first - 1), false);
    games_goto(first);
}

static void open_game(ListItem *game)
{
    strncpy(cur_slug, game->payload, STR_MAX - 1);
    strncpy(cur_title, game->label, STR_MAX - 1);
    list_free(&list_links);
    list_links = list_createWithTitle(MAX_LINKS, LIST_SMALL, cur_title);

    sqlite3_stmt *st;
    const char *sql = "SELECT name, url, filename, coalesce(size_str, ''), coalesce(torrent_file_path, ''), coalesce(size, 0) "
                      "FROM links WHERE entry = ? AND type = 'Game' AND requires_auth = 0 ORDER BY name";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, cur_slug, -1, SQLITE_STATIC);
    int n = 0;
    while (n < MAX_LINKS && sqlite3_step(st) == SQLITE_ROW) {
        Link *l = &links[n];
        strncpy(l->name, (const char *)sqlite3_column_text(st, 0), STR_MAX - 1);
        strncpy(l->url, (const char *)sqlite3_column_text(st, 1), sizeof(l->url) - 1);
        strncpy(l->filename, (const char *)sqlite3_column_text(st, 2), STR_MAX - 1);
        strncpy(l->size_str, (const char *)sqlite3_column_text(st, 3), sizeof(l->size_str) - 1);
        strncpy(l->torrent_path, (const char *)sqlite3_column_text(st, 4), sizeof(l->torrent_path) - 1);
        l->size = sqlite3_column_int64(st, 5);
        ListItem item = {.action_id = n};
        snprintf(item.label, STR_MAX, "%.180s  %.31s  (%s)", l->filename, l->size_str,
                 l->torrent_path[0] ? "torrent" : "http");
        list_addItem(&list_links, item);
        n++;
    }
    sqlite3_finalize(st);
    if (n == 0) {
        ListItem none = {.action_id = -1};
        strcpy(none.label, "No downloadable link");
        list_addItem(&list_links, none);
    }
    screen_level = SCREEN_LINKS;
}

static List *current_list(void)
{
    switch (screen_level) {
    case SCREEN_GAMES: return &list_games;
    case SCREEN_LINKS: return &list_links;
    case SCREEN_BIOS: return &list_bios;
    default: return &list_platforms;
    }
}

// --------------------------------------------------------------- render ---

static void render(void)
{
    List *list = current_list();
    theme_renderHeader(screen, list->title, screen_level == SCREEN_PLATFORMS);
    theme_renderList(screen, list);
    theme_renderFooter(screen);
    if (screen_level == SCREEN_GAMES) {
        theme_renderStandardHint(screen, "SELECT", "BACK   SELECT: SEARCH   L/R: LETTER   L2/R2: PAGE");
        theme_renderFooterStatus(screen, cur_total ? games_index() + 1 : 0, cur_total);
    }
    else {
        theme_renderStandardHint(screen, "SELECT", "BACK");
        theme_renderFooterStatus(screen, list->active_pos + 1, list->item_count);
    }

    if (confirm_open) {
        Link *l = &links[list_links.items[list_links.active_pos].action_id];
        char msg[1024];
        snprintf(msg, sizeof(msg), "%s\n%s\n \nSave to Roms/%s", l->filename, l->size_str, cur_platform->code);
        theme_renderDialog(screen, "Download?", msg, true);
    }
    else if (job != JOB_NONE) {
        char status[128], msg[256];
        job_status(status, sizeof(status));
        if (job == JOB_DOWNLOAD && prog_done > 0) {
            char done_s[32], total_s[32], speed_s[32];
            fmt_mb(done_s, sizeof(done_s), prog_done);
            fmt_mb(total_s, sizeof(total_s), prog_total);
            if (prog_speed >= 1048576.0)
                snprintf(speed_s, sizeof(speed_s), "%.1f MB/s", prog_speed / 1048576.0);
            else
                snprintf(speed_s, sizeof(speed_s), "%.0f KB/s", prog_speed / 1024.0);
            if (prog_total > 0)
                snprintf(msg, sizeof(msg), "%s / %s  (%d%%)\n%s\n%s", done_s, total_s,
                         (int)(100 * prog_done / prog_total), speed_s, lang_tr("B: cancel"));
            else
                snprintf(msg, sizeof(msg), "%s\n%s\n%s", done_s, speed_s, lang_tr("B: cancel"));
            theme_renderDialog(screen, job_title, msg, false);
            // progress bar near the bottom edge of the dialog box, below the text
            // (the text block is centred at pop_bg.y + 160, so the theme's dot
            // position at y=225 would overlap it)
            SDL_Surface *pop_bg = resource_getSurface(POP_BG);
            int box_y = (g_display.height - pop_bg->h) / 2;
            int w = 240 * g_scale, h = 10 * g_scale;
            SDL_Rect bar = {(g_display.width - w) / 2, box_y + pop_bg->h - 45 * g_scale - h / 2, w, h};
            SDL_FillRect(screen, &bar, SDL_MapRGB(screen->format, 0x40, 0x40, 0x40));
            if (prog_total > 0) {
                SDL_Color c = theme()->grid.color;
                SDL_Rect fill = {bar.x, bar.y, (Uint16)(w * (prog_done > prog_total ? 1.0 : (double)prog_done / prog_total)), h};
                SDL_FillRect(screen, &fill, SDL_MapRGB(screen->format, c.r, c.g, c.b));
            }
        }
        else {
            snprintf(msg, sizeof(msg), "%s\n \n%s", lang_tr(status[0] ? status : "Starting..."),
                     job == JOB_DOWNLOAD ? lang_tr("B: cancel") : " ");
            theme_renderDialogProgress(screen, job_title, msg, false);
        }
    }
    else if (result_open) {
        theme_renderDialog(screen, job_title, result_msg, false);
    }

    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

// ---------------------------------------------------------------- input ---

static void job_finish(void)
{
    char code[16] = "1";
    file_readLastLine(DONE_FILE, code);
    int rc = atoi(code);
    char status[128];
    job_status(status, sizeof(status));
    int kind = job;
    job = JOB_NONE;
    // Keep the full job output on the SD card for troubleshooting.
    system("cp " STATUS_FILE " " SYSDIR "/logs/romdl.log 2>/dev/null");
    if (kind == JOB_CATALOG) {
        db_open();
        build_platforms();
        screen_level = SCREEN_PLATFORMS;
    }
    else if (screen_level == SCREEN_BIOS) {
        int pos = list_bios.active_pos;
        build_bios(); // refresh installed/missing flags
        list_scrollTo(&list_bios, pos);
    }
    snprintf(result_msg, sizeof(result_msg), "%s\n \n%s", lang_tr(rc == 0 ? "Done" : "Failed"), lang_tr(status));
    result_open = true;
}

static bool handle_keys(void)
{
    if (!updateKeystate(keystate, &quit, true, NULL))
        return false;

    if (job != JOB_NONE) {
        if (keystate[SW_BTN_B] == PRESSED && job == JOB_DOWNLOAD)
            job_cancel();
        return true;
    }
    if (result_open) {
        if (keystate[SW_BTN_A] == PRESSED || keystate[SW_BTN_B] == PRESSED)
            result_open = false;
        return true;
    }
    if (confirm_open) {
        if (keystate[SW_BTN_A] == PRESSED) {
            confirm_open = false;
            download_start(&links[list_links.items[list_links.active_pos].action_id]);
        }
        else if (keystate[SW_BTN_B] == PRESSED)
            confirm_open = false;
        return true;
    }

    List *list = current_list();
    if (keystate[SW_BTN_UP] >= PRESSED)
        list_keyUp(list, keystate[SW_BTN_UP] == REPEATING);
    else if (keystate[SW_BTN_DOWN] >= PRESSED)
        list_keyDown(list, keystate[SW_BTN_DOWN] == REPEATING);
    else if (screen_level == SCREEN_GAMES && keystate[SW_BTN_R1] == PRESSED)
        games_next_letter();
    else if (screen_level == SCREEN_GAMES && keystate[SW_BTN_L1] == PRESSED)
        games_prev_letter();
    else if (screen_level == SCREEN_GAMES && keystate[SW_BTN_R2] == PRESSED)
        games_goto((cur_page + 1) * PAGE_SIZE);
    else if (screen_level == SCREEN_GAMES && keystate[SW_BTN_L2] == PRESSED)
        games_goto((cur_page - 1) * PAGE_SIZE);
    else if (screen_level == SCREEN_GAMES && keystate[SW_BTN_SELECT] == PRESSED)
        games_search();
    else if (keystate[SW_BTN_A] == PRESSED && list->item_count > 0) {
        ListItem *item = &list->items[list->active_pos];
        switch (screen_level) {
        case SCREEN_PLATFORMS:
            if (item->action_id == -2) {
                build_bios();
                screen_level = SCREEN_BIOS;
            }
            else if (item->action_id < 0)
                catalog_update();
            else
                open_platform(item->action_id);
            break;
        case SCREEN_BIOS: bios_download(item->action_id); break;
        case SCREEN_GAMES: open_game(item); break;
        case SCREEN_LINKS:
            if (item->action_id >= 0)
                confirm_open = true;
            break;
        }
    }
    else if (keystate[SW_BTN_B] == PRESSED) {
        if (screen_level == SCREEN_PLATFORMS)
            quit = true;
        else if (screen_level == SCREEN_BIOS)
            screen_level = SCREEN_PLATFORMS;
        else
            screen_level--;
    }
    else if (keystate[SW_BTN_MENU] == PRESSED)
        quit = true;
    return true;
}

int main(int argc, char *argv[])
{
    log_setName("romDownloader");
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    SDL_InitDefault();
    settings_load();
    lang_load();

    db_open();
    build_platforms();
    if (!db)
        catalog_update();

    bool dirty = true;
    uint32_t last_poll = 0;
    while (!quit) {
        dirty |= handle_keys();

        if (job != JOB_NONE && SDL_GetTicks() - last_poll > 500) {
            last_poll = SDL_GetTicks();
            if (exists(DONE_FILE))
                job_finish();
            dirty = true;
        }

        if (dirty) {
            render();
            dirty = false;
        }
    }

    if (job != JOB_NONE)
        job_cancel();
    if (db)
        sqlite3_close(db);
    list_free(&list_platforms);
    list_free(&list_games);
    list_free(&list_links);
    list_free(&list_bios);
    SDL_FreeSurface(screen);
    TTF_Quit();
    SDL_Quit();
    return EXIT_SUCCESS;
}
