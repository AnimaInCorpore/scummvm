#!/bin/bash -eux
# -e: Exit immediately if a command exits with a non-zero status.
# -u: Treat unset variables as an error when substituting.
# -x: Display expanded script commands
#
# Stock Atari STE target: 8 MHz 68000, 4 MiB RAM. Game-only build
# (ATARI_STE_GAME_ONLY): SCUMM engine, the DOS CD versions of Monkey Island
# and Fate of Atlantis, statically linked, no audio. Output in
# build-ste-scumm-static4/, the directory the devtools/atari-ste tools use by
# default (STE_BUILD); stage it with the HD layout described there.
#
# The code must be built for size. configure's release optimization level is
# -O2, which gives about 3.15 MB of code; the game then runs out of memory
# (std::bad_alloc) at start on 4 MiB. -Os without unwind tables gives about
# 2.17 MB. configure has no option for an optimization level, so optimizations
# are disabled there (after --enable-release, which enables them) and the
# flags configure would add for m68k-atari-mint come from here instead:
# -fomit-frame-pointer, -ffast-math -fno-unsafe-math-optimizations.
#
# -ffunction-sections/-fdata-sections stay as in the measured build, although
# section garbage collection does nothing on m68k-atari-mintelf (see
# devtools/atari-falcon030/README.md).
BUILD_DIR=${BUILD_DIR:-build-ste-scumm-static4}
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
PLATFORM=m68k-atari-mintelf
export CXXFLAGS="-m68000 -fomit-frame-pointer -ffunction-sections -fdata-sections -DDISABLE_FANCY_THEMES -DDISABLE_DOSBOX_OPL -DATARI_STE_GAME_ONLY -fno-exceptions -ffast-math -fno-unsafe-math-optimizations -Os -fno-unwind-tables -fno-asynchronous-unwind-tables"
if [ ! -f config.log ]
then
../configure \
	--backend=atari \
	--host=${PLATFORM} \
	--enable-release \
	--disable-optimizations \
	--disable-all-engines \
	--enable-engine=scumm \
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
