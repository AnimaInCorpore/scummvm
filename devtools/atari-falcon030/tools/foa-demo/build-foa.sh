#!/bin/sh
# Build build/foa.tos: the Fate of Atlantis AdLib score, played by the DSP.
#
#   ./build-foa.sh <Atlantis game directory> [seconds]
#   ./build-foa.sh --trace <opl-writes.ev> [seconds]
#
# The first form records the score with the headless capture executable of
# ../foa-opl3 (the real game, resource selection, iMUSE and AdLib driver, on
# a virtual clock); the second uses a trace recorded earlier. Seconds defaults
# to 300, the longest window the capture takes past its first minute.
#
# Prerequisites, all from ../foa-opl3: ./build-dsp.sh (the DSP image and the
# generated tables), ./build-capture.sh (first form only), and the fixture,
# built in its headless tree:
#   make -f Makefile -f ../../kernel.mk opl-rt-fixture
# Tools come from the F030MXDRV checkout as build-dsp.sh finds it.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
opl=$here/../foa-opl3
mxdrv=${MXDRV:-$HOME/Work/F030MXDRV}
if [ -z "${MXDRV:-}" ] && [ ! -d "$mxdrv" ]; then
    mxdrv=$(CDPATH= cd -- "$here/../../../../.." && pwd)/F030MXDRV
fi
vasm="$mxdrv/build/tools/vasm/vasmm68k_mot"
vlink="$mxdrv/build/tools/vlink/vlink"
fixture=$opl/build/headless/opl-rt-fixture
[ -x "$fixture" ] || [ -x "$fixture.exe" ] || { echo "error: build $fixture first (see the header)" >&2; exit 1; }
[ -f "$opl/dsp/oplrt_image.i" ] || { echo "error: run $opl/build-dsp.sh first" >&2; exit 1; }
[ -f "$vasm" ] && [ -f "$vlink" ] || { echo "error: vasm/vlink not found under $mxdrv" >&2; exit 1; }

case "${1:-}" in
    --trace) trace=${2:?a trace file}; shift 2 ;;
    "") echo "usage: $0 <game directory> [seconds] | --trace <opl-writes.ev> [seconds]" >&2; exit 1 ;;
    *) game=$1; shift
       mkdir -p "$here/build"
       trace=$here/build/capture/opl-writes.ev
       seconds_wanted=${1:-300}
       if [ ! -f "$trace" ]; then
           python3 "$opl/capture-opl.py" --game "$game" --milliseconds "$((seconds_wanted * 1000))" \
               --output "$here/build/capture" >/dev/null
       fi ;;
esac
seconds=${1:-300}

mkdir -p "$here/build"
cd "$here/build"
"$fixture" trace opldata.tmp expect.tmp --trace "$trace" --seconds "$seconds" --play playdata.tmp >/dev/null
python3 "$here/pack-foa.py" opldata.tmp playdata.tmp --tables foa-tables.bin --score foa-score.bin
rm -f opldata.tmp expect.tmp playdata.tmp

"$vasm" "$here/foa.s" -quiet -Felf -m68030 -I "$mxdrv/src/m68k" -I "$opl/dsp" -I . \
    -o foa.o -L foa.lst
"$vlink" foa.o -b ataritos -s -e start -o foa.tos
echo "built build/foa.tos: $(wc -c < foa.tos) bytes"
