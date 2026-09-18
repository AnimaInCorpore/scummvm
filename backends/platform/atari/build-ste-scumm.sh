#!/bin/sh

set -eu

BUILD_DIR=${1:-build-ste-scumm-ste}
REPO=$(cd "$(dirname "$0")/../../.." && pwd)

# Cross tools: $MINT_BIN, else ../cross-mint/bin next to this checkout (the
# devtools/atari-ste default), else m68k-atari-mintelf-g++ from PATH.
if [ -z "${MINT_BIN:-}" ]; then
	if [ -x "$REPO/../cross-mint/bin/m68k-atari-mintelf-g++" ]; then
		MINT_BIN="$REPO/../cross-mint/bin"
	elif command -v m68k-atari-mintelf-g++ >/dev/null 2>&1; then
		MINT_BIN=$(dirname "$(command -v m68k-atari-mintelf-g++)")
	else
		echo "m68k-atari-mintelf-g++ not found; set MINT_BIN" >&2
		exit 1
	fi
fi
NM="$MINT_BIN/m68k-atari-mintelf-nm"
AS="$MINT_BIN/m68k-atari-mintelf-as"
CXX="$MINT_BIN/m68k-atari-mintelf-g++"

# Byte-order sorting keeps the symbol lists identical on every host.
export LC_ALL=C

cd "$REPO/$BUILD_DIR"

ROOT_FILE=ste-plugin-roots.txt

# xargs -s splits nm runs so no command line exceeds the 32 KB Windows limit.
root_symbols() {
	find engines/scumm base/detection -name '*.o' -print0 |
		xargs -0 -s 30000 "$NM" -u |
		awk 'NF >= 2 && $1 == "U" { print $2 }' |
		sort -u
}

root_symbols > "$ROOT_FILE"

make_host_symbol_object() {
	refs="ste-plugin-host-refs.S"
	refs_object="ste-plugin-host-refs.o"
	host_object="ste-plugin-host-symbols.o"
	all_symbols="$ROOT_FILE"
	main_symbols="ste-plugin-main-symbols.txt"
	main_values="ste-plugin-main-values.txt"
	plugin_symbols="ste-plugin-defined-symbols.txt"
	common_symbols="ste-plugin-main-roots.txt"

	"$NM" scummvm.prg |
		awk 'NF >= 3 && $2 != "U" { print $3 }' |
		sort -u > "$main_symbols"
	"$NM" -g scummvm.prg |
		awk 'NF >= 3 && $2 != "U" { print $1, $3 }' |
		sort -k2,2 -u > "$main_values"
	{
		find engines/scumm base/detection -name '*.o' -print0 |
			xargs -0 -s 30000 "$NM"
		"$NM" backends/plugins/elf/version.o
	} |
		awk 'NF >= 3 && $2 != "U" && $(NF - 1) != "U" { print $NF }' |
		sort -u > "$plugin_symbols"
	comm -12 "$all_symbols" "$main_symbols" |
		comm -23 - "$plugin_symbols" > "$common_symbols"

	{
		printf '.text\n'
		awk 'NR == FNR { values[$2] = $1; next }
			{ if ($1 in values) {
				printf ".globl %s\n.equ %s, 0x%s\n", $1, $1, values[$1]
			} }' "$main_values" "$common_symbols"
	} > "$refs"

	"$AS" -m68000 -o "$refs_object" "$refs"
	cp "$refs_object" "$host_object"
	printf '%s\n' "$host_object"
}

# gcc response files keep the long option and object lists off the command line.
main_link() {
	awk '{ printf "-Wl,--undefined=%s\n", $0 }' "$ROOT_FILE" > ste-plugin-roots.rsp

	"$CXX" -m68000 -Wl,--gc-sections -Wl,-X -Wl,--msuper-memory \
	-Wl,--stack,256k @ste-plugin-roots.rsp \
	backends/platform/atari/osystem_atari.o \
	backends/platform/atari/atari_ikbd.o \
	backends/platform/atari/native_features.o \
	backends/platform/atari/dlmalloc.o \
	base/libbase.a engines/libengines.a gui/libgui.a \
	backends/libbackends.a video/libvideo.a image/libimage.a \
	graphics/libgraphics.a audio/libaudio.a math/libmath.a \
		common/libcommon.a common/compression/libcompression.a \
		common/formats/libformats.a -lm -o scummvm.prg
}

main_link
host_object=$(make_host_symbol_object)

plugin_objects() {
	make -pn 2>/dev/null |
		awk -v target="$1" '$0 ~ "^" target ":" { sub(/^[^:]*: /, ""); print; exit }' |
		tr ' ' '\n' |
		awk -v target="$1" '
			$0 == "scummvm.prg" || $0 == "" { next }
			target != "plugins/scumm.plg" { print; next }
		$0 ~ /^engines\/scumm\/he\// ||
		$0 ~ /^engines\/scumm\/(gfx_nes|gfx_towns)\.o$/ ||
		$0 ~ /^engines\/scumm\/players\/player_(ad|apple2|he|mac_|mod|nes|pce|sid|towns)\.o$/ ||
		$0 ~ /^engines\/scumm\/imuse\/drivers\/(amiga|fmtowns|macintosh|pcspk)\.o$/ { next }
		{ print }'
}

link_plugin() {
	target=$1
	output=$2
	objects="ste-plugin-$(basename "$output" .plg)-objects.rsp"
	plugin_objects "$target" > "$objects"
	"$CXX" -m68000 -Wl,--gc-sections -Wl,-s \
		-Wl,--allow-multiple-definition \
		-Wl,-Map,"$output.map" \
		-Wl,--undefined=PLUGIN_getBuildDate \
		-Wl,--undefined=PLUGIN_getVersion \
		-Wl,--undefined=PLUGIN_getType \
		-Wl,--undefined=PLUGIN_getTypeVersion \
		-Wl,--undefined=PLUGIN_getObject \
		-Wl,--undefined=PLUGIN_finalize \
		-nostartfiles @"$objects" \
		backends/plugins/elf/version.o \
		"$host_object" \
		-Wl,-q,--retain-symbols-file,../backends/plugins/elf/plugin.syms \
		-Wl,-m,m68kelf,-T,../backends/plugins/mintelf/plugin.ld \
		-o "$output"
}

link_plugin plugins/scumm.plg plugins/scumm.plg
link_plugin plugins/detection.plg plugins/detection.plg

used_symbols=ste-plugin-used-symbols.txt
{
	"$NM" plugins/scumm.plg
	"$NM" plugins/detection.plg
} |
	awk 'NF >= 3 && $2 == "A" { print $3 }' |
	sort -u |
	comm -12 - "$ROOT_FILE" > "$used_symbols"

if ! cmp -s "$ROOT_FILE" "$used_symbols"; then
	cp "$used_symbols" "$ROOT_FILE"
	main_link
	host_object=$(make_host_symbol_object)
	link_plugin plugins/scumm.plg plugins/scumm.plg
	link_plugin plugins/detection.plg plugins/detection.plg
fi

"$MINT_BIN/m68k-atari-mintelf-size" scummvm.prg plugins/scumm.plg plugins/detection.plg
