#!/bin/bash -eux
# -e: Exit immediately if a command exits with a non-zero status.
# -u: Treat unset variables as an error when substituting.
# -x: Display expanded script commands
#
# The Falcon030 all-assets game build with the game screen's
# chunky-to-planar conversion on the DSP (ATARI_DSP_C2P, see
# backends/platform/atari/atari-dsp-c2p.h): the sound DMA streams every
# frame through the DSP in the background, so the 68030 keeps the time the
# conversion took for the game. The DSP and the sound DMA then belong to
# the screen, so this build plays no sound: no DSP OPL, a silent mixer.
#
# Being silent, it is not bound to the AdLib games of the normal build.
# This profile keeps all SCUMM constructors and SCI32 enabled for the games
# installed in assets, including older SCUMM titles and high-resolution SCI.
#
# The DSP kernel's image, dsp-c2p-image.h, comes from
# devtools/atari-falcon030/tools/ssi-dma-c2p/build.sh.

BUILD_DIR=${BUILD_DIR:-build-falcon030-dspc2p-all-games}
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

PLATFORM=m68k-atari-mintelf

export ASFLAGS="-m68030"
export CXXFLAGS="-m68030 -DATARI_FALCON_GAME_ONLY -DATARI_DSP_C2P -DDISABLE_FANCY_THEMES -DDISABLE_LAUNCHERDISPLAY_GRID -DDISABLE_DOSBOX_OPL -DDISABLE_MAME_OPL"
export LDFLAGS="-m68030"

export PKG_CONFIG_LIBDIR="$(${PLATFORM}-gcc -print-sysroot)/usr/lib/m68020-60/pkgconfig"

if [ ! -f config.log ]
then
../configure \
	--backend=atari \
	--host=${PLATFORM} \
	--enable-release \
	--disable-bink \
	--enable-verbose-build \
	--disable-all-engines \
	--enable-engine=scumm,scumm-7-8,sci,sci32,sky,cruise \
	--disable-translation \
	--disable-cloud \
	--disable-tts \
	--disable-eventrecorder \
	--disable-16bit \
	--disable-scalers \
	--disable-taskbar \
	--disable-system-dialogs \
	--disable-seq-midi \
	--disable-timidity \
	--disable-nuked-opl \
	--disable-savegame-timestamp \
	--disable-mt32emu
fi

make -j$(getconf _NPROCESSORS_CONF) scummvm.prg
