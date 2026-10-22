#!/bin/sh
echo $0 $*
progdir=`dirname "$0"`
homedir=`dirname "$1"`

# Default core: SwanStation (RetroAchievements). Per game, Game List Options > Game core
# still offers PCSX ReARMed, which is faster on the Miyoo Mini.
cd /mnt/SDCARD/RetroArch/
HOME=/mnt/SDCARD/RetroArch/ $progdir/../../RetroArch/retroarch -v -L $progdir/../../RetroArch/.retroarch/cores/km_duckswanstation_xtreme_amped_libretro.so "$1"
