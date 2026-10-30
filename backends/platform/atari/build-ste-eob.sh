#!/bin/bash -eux
# -e: Exit immediately if a command exits with a non-zero status.
# -u: Treat unset variables as an error when substituting.
# -x: Display expanded script commands
#
# Stock Atari STE target: 8 MHz 68000, 4 MiB RAM. Game-only build
# (ATARI_STE_GAME_ONLY) of the Kyra engine reduced to the DOS versions of
# Eye of the Beholder 1 and 2, statically linked, no sound and no ScummVM
# menus. The games run in their VGA mode (render_mode=vga, 256 colours);
# how the STE shows those colours is up to the display path. kyra.dat must
# be next to the program or in the game directory.
#
# Compiler flags as in build-ste.sh, which explains why the code is built
# for size and why configure's optimizations are disabled.
BUILD_DIR=${BUILD_DIR:-build-ste-eob}
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
PLATFORM=m68k-atari-mintelf
# DISABLE_COMMAND_LINE and DISABLE_DEFAULT_AUDIOCD_MANAGER go further than
# build-ste.sh: the game starts from the INI file and plays no CD audio.
# -fno-threadsafe-statics: the single-tasking STE needs no locks around local
# statics, and libstdc++'s lock pulls in its exception unwinder.
export CXXFLAGS="-m68000 -fomit-frame-pointer -ffunction-sections -fdata-sections -DDISABLE_FANCY_THEMES -DDISABLE_DOSBOX_OPL -DATARI_STE_GAME_ONLY -DDISABLE_COMMAND_LINE -DDISABLE_DEFAULT_AUDIOCD_MANAGER -fno-exceptions -fno-threadsafe-statics -ffast-math -fno-unsafe-math-optimizations -Os -fno-unwind-tables -fno-asynchronous-unwind-tables"
if [ ! -f config.log ]
then
../configure \
	--backend=atari \
	--host=${PLATFORM} \
	--enable-release \
	--disable-optimizations \
	--disable-all-engines \
	--enable-engine=eob \
	--disable-detection-full \
	--disable-highres \
	--disable-bink \
	--disable-mt32emu \
	--disable-seq-midi \
	--disable-timidity \
	--disable-lua \
	--disable-nuked-opl \
	--disable-scalers \
	--disable-aspect \
	--disable-taskbar \
	--disable-system-dialogs \
	--disable-system-printing \
	--disable-translation \
	--disable-tts \
	--disable-cloud \
	--disable-dlc \
	--disable-scummvmdlc \
	--disable-jpeg \
	--disable-png \
	--disable-gif \
	--disable-theoradec \
	--disable-vpx \
	--disable-faad \
	--disable-zlib \
	--disable-updates \
	--disable-freetype2 \
	--disable-16bit \
	--no-builtin-resources
fi
make -j$(getconf _NPROCESSORS_CONF)
