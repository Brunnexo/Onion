#ifndef ROMDL_PLATFORMS_H__
#define ROMDL_PLATFORMS_H__

#include <stdbool.h>

// romgi platform id -> Onion Roms/<code>. `extract` = emulator extlist has no zip.
// `icon` = Onion default icon name (static/build/Icons/Default).
// Codes derived from static/packages/Emu/*/Emu/*/config.json rompath.
typedef struct {
    const char *id;
    const char *code;
    bool extract;
    const char *icon; // Icons/<pack>/<icon>.png
} Platform;

static const Platform PLATFORMS[] = {
    {"nes", "FC", false, "fc"},
    {"fds", "FDS", false, "fds"},
    {"snes", "SFC", false, "sfc"},
    {"gb", "GB", false, "gb"},
    {"gbc", "GBC", false, "gbc"},
    {"gba", "GBA", false, "gba"},
    {"vb", "VB", false, "vb"},
    {"min", "POKE", false, "poke"},
    {"nds", "NDS", false, "nds"},
    {"smd", "MD", false, "md"},
    {"sms", "MS", false, "ms"},
    {"gg", "GG", false, "gg"},
    {"32x", "THIRTYTWOX", false, "32X"},
    {"scd", "SEGACD", false, "segacd"},
    {"ps1", "PS", true, "ps"},
    {"a26", "ATARI", false, "atari"},
    {"a52", "FIFTYTWOHUNDRED", false, "5200"},
    {"a78", "SEVENTYEIGHTHUNDRED", false, "7800"},
    {"lynx", "LYNX", false, "lynx"},
    {"cv", "COLECO", false, "col"},
    {"intv", "INTELLIVISION", false, "itv"},
    {"tg16", "PCE", false, "pce"},
    {"tgcd", "PCECD", false, "pcecd"},
    {"ngcd", "NEOCD", true, "neocd"},
};
#define PLATFORM_COUNT (int)(sizeof(PLATFORMS) / sizeof(PLATFORMS[0]))

static inline const Platform *platform_find(const char *id)
{
    for (int i = 0; i < PLATFORM_COUNT; i++)
        if (strcmp(PLATFORMS[i].id, id) == 0)
            return &PLATFORMS[i];
    return NULL;
}

#endif
