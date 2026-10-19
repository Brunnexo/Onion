#ifndef ROMDL_PLATFORMS_H__
#define ROMDL_PLATFORMS_H__

#include <stdbool.h>

// romgi platform id -> Onion Roms/<code>. `extract` = emulator extlist has no zip.
// Codes derived from static/packages/Emu/*/Emu/*/config.json rompath.
typedef struct {
    const char *id;
    const char *code;
    bool extract;
} Platform;

static const Platform PLATFORMS[] = {
    {"nes", "FC", false},
    {"fds", "FDS", false},
    {"snes", "SFC", false},
    {"gb", "GB", false},
    {"gbc", "GBC", false},
    {"gba", "GBA", false},
    {"vb", "VB", false},
    {"min", "POKE", false},
    {"nds", "NDS", false},
    {"smd", "MD", false},
    {"sms", "MS", false},
    {"gg", "GG", false},
    {"32x", "THIRTYTWOX", false},
    {"scd", "SEGACD", false},
    {"ps1", "PS", true},
    {"a26", "ATARI", false},
    {"a52", "FIFTYTWOHUNDRED", false},
    {"a78", "SEVENTYEIGHTHUNDRED", false},
    {"lynx", "LYNX", false},
    {"cv", "COLECO", false},
    {"intv", "INTELLIVISION", false},
    {"tg16", "PCE", false},
    {"tgcd", "PCECD", false},
    {"ngcd", "NEOCD", true},
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
