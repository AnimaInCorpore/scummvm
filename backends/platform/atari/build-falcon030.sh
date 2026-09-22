#!/bin/bash -eux
# -e: Exit immediately if a command exits with a non-zero status.
# -u: Treat unset variables as an error when substituting.
# -x: Display expanded script commands
#
# Stock Falcon030 target: 16 MHz 68030 + 68882, 14 MB RAM, DSP56001.
#
# Differs from build-release030.sh ("Atari Lite", which states a 16 MB
# minimum a real Falcon cannot reach) by being a game-only build: the SCUMM,
# SCI, Sky and Cruise engines, single game, statically linked, so as much of
# the 14 MB as possible is left for the game and for on-machine MT-32
# synthesis. All four engines' DOS games drive the AdLib that the DSP
# synthesizes; SCI32 stays out on its own, as it depends on
# --disable-highres's highres feature.
#
# -m68030 already selects the m68020-60 multilib and emits hardware FPU
# instructions, so it is the 68882 build; there is no -m68882 option in GCC
# (the spelling would be -m68881, and it changes nothing here).

mkdir -p build-falcon030
cd build-falcon030

PLATFORM=m68k-atari-mintelf
FASTCALL=false
# A single-game build has nothing to choose between at run time, so static
# linking is smaller: unreferenced engine families are never pulled out of
# the archives at all. ATARI_FALCON_GAME_ONLY (engines/scumm/metaengine.cpp)
# is what removes those references.
PLUGINS=false
# Munt cannot synthesise MT-32 in real time on a 16 MHz 68030. Current music
# work compiles MIDI/iMUSE and ROM-derived synth data for a future live backend;
# an opt-in score adapter feeds real iMUSE, but there is no internal waveform
# renderer yet. No prerendered music is selected by this build.
# See devtools/atari-falcon030/README.md.
MT32EMU=false

export ASFLAGS="-m68030"
# No -ffunction-sections/--gc-sections: section garbage collection does nothing
# on m68k-atari-mintelf (verified - ld 2.45 removes zero sections even for a
# trivially dead function), and the extra section headers cost ~11 KB.
export CXXFLAGS="-m68030 -DATARI_FALCON_GAME_ONLY -DATARI_DSP_OPL -DDISABLE_FANCY_THEMES -DDISABLE_LAUNCHERDISPLAY_GRID -DDISABLE_DOSBOX_OPL -DDISABLE_MAME_OPL"
export LDFLAGS="-m68030"

export PKG_CONFIG_LIBDIR="$(${PLATFORM}-gcc -print-sysroot)/usr/lib/m68020-60/pkgconfig"

if $FASTCALL
then
	ASFLAGS="$ASFLAGS -mfastcall"
	CXXFLAGS="$CXXFLAGS -mfastcall"
	LDFLAGS="$LDFLAGS -mfastcall"
fi

if $PLUGINS
then
	PLUGINS_FLAGS="--enable-plugins --default-dynamic --enable-detection-dynamic"
else
	PLUGINS_FLAGS=""
fi

if $MT32EMU
then
	MT32EMU_FLAGS=""
else
	MT32EMU_FLAGS="--disable-mt32emu"
fi

if [ ! -f config.log ]
then
../configure \
	--backend=atari \
	--host=${PLATFORM} \
	--enable-release \
	--disable-highres \
	--disable-bink \
	--enable-verbose-build \
	--disable-all-engines \
	--enable-engine=scumm,sci,sky,cruise \
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
	${MT32EMU_FLAGS} \
	${PLUGINS_FLAGS}
fi

# Packaging is unchanged from the Lite flavour for now; a dedicated
# atarifalcondist target is only worth adding once the data/theme set for
# this target is settled.
make -j$(getconf _NPROCESSORS_CONF) atarilitedist
