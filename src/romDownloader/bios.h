#ifndef ROMDL_BIOS_H__
#define ROMDL_BIOS_H__

// BIOS files the Onion emulators look for under /mnt/SDCARD/BIOS, as listed
// in the Onion docs (website/docs/04-emulators), served file by file from the
// Internet Archive "retroarch_bios" item.
#define BIOS_DIR "/mnt/SDCARD/BIOS"
#define BIOS_URL "https://archive.org/download/retroarch_bios/"

typedef struct {
    const char *system; // shown in the list
    const char *file;   // path under BIOS_DIR
    const char *remote; // file name inside the archive.org item
    long long size;
} Bios;

static const Bios BIOS_FILES[] = {
    {"PS1", "PSXONPSP660.bin", "PSXONPSP660.bin", 524288},
    {"PS1", "scph5501.bin", "scph5501.bin", 524288},
    {"PS1", "scph7001.bin", "scph7001.bin", 524288},
    {"PS1", "scph1001.bin", "scph1001.bin", 524288},
    {"GBA", "gba_bios.bin", "gba_bios.bin", 16384},
    {"GB", "gb_bios.bin", "gb_bios.bin", 256},
    {"GBC", "gbc_bios.bin", "gbc_bios.bin", 2304},
    {"SGB", "sgb_bios.bin", "sgb_bios.bin", 256},
    {"FDS", "disksys.rom", "disksys.rom", 8192},
    {"PCE CD", "syscard3.pce", "syscard3.pce", 262656},
    {"Sega CD", "bios_CD_U.bin", "bios_CD_U.bin", 131072},
    {"Sega CD", "bios_CD_E.bin", "bios_CD_E.bin", 131072},
    {"Sega CD", "bios_CD_J.bin", "bios_CD_J.bin", 131072},
    {"Mega Drive", "bios_MD.bin", "bios_MD.bin", 16384},
    {"Master System", "bios_U.sms", "bios_U.sms", 8192},
    {"Master System", "bios_E.sms", "bios_E.sms", 8192},
    {"Master System", "bios_J.sms", "bios_J.sms", 8192},
    {"Game Gear", "bios.gg", "bios.gg", 131072},
    {"Lynx", "lynxboot.img", "lynxboot.img", 512},
    {"Atari 5200", "5200.rom", "5200.rom", 2048},
    {"Atari 5200", "ATARIBAS.ROM", "ATARIBAS.ROM", 8192},
    {"Atari 7800", "7800 BIOS (U).rom", "7800 BIOS (U).rom", 4096},
    {"ColecoVision", "coleco.rom", "colecovision.rom", 8192},
    {"Intellivision", "exec.bin", "exec.bin", 8192},
    {"Intellivision", "grom.bin", "grom.bin", 2048},
    {"Neo Geo CD", "neocd/000-lo.lo", "000-lo.lo", 131072},
    {"Neo Geo CD", "neocd/neocd.bin", "neocd.bin", 524288},
    {"Pokemon Mini", "bios.min", "bios.min", 4096},
    {"Sufami Turbo", "STBIOS.bin", "STBIOS.bin", 262144},
    {"Satellaview", "BS-X.bin", "BS-X.bin", 1048576},
};
#define BIOS_COUNT (int)(sizeof(BIOS_FILES) / sizeof(BIOS_FILES[0]))

#endif
