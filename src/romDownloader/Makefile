INCLUDE_CJSON=1
include ../common/config.mk

TARGET = romDownloader
CFLAGS := $(CFLAGS) -D_DEFAULT_SOURCE
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_image -lSDL_ttf -lSDL_rotozoom -lsqlite3 -lkbinput

include ../common/commands.mk
include ../common/recipes.mk
