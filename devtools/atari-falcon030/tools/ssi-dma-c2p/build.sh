#!/bin/sh
# Build SSIECHO.TOS, the SSI DMA pass-through test, with the sibling
# F030MXDRV checkout's toolchain: Motorola's asm56000 under DOSBox, and
# vasm/vlink. Everything generated lands in build/.
#
# The checkout is found as foa-opl3/gate_env.py finds it: $MXDRV, else under
# ~/Work, else beside this repository. DOSBox is $DOSBOX, else
# dosbox-staging or dosbox on the path.
set -eu
task_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mxdrv=${MXDRV:-$HOME/Work/F030MXDRV}
if [ -z "${MXDRV:-}" ] && [ ! -d "$mxdrv" ]; then
    mxdrv=$(CDPATH= cd -- "$task_dir/../../../../.." && pwd)/F030MXDRV
fi
# A worktree sits deeper than the main checkout; look beside that one too.
if [ -z "${MXDRV:-}" ] && [ ! -d "$mxdrv" ]; then
    common=$(git -C "$task_dir" rev-parse --path-format=absolute --git-common-dir)
    mxdrv=$(CDPATH= cd -- "$common/../.." && pwd)/F030MXDRV
fi
tools="$mxdrv/third_party/f030dsp3d/tools/asm56k"
vasm="$mxdrv/build/tools/vasm/vasmm68k_mot"
vlink="$mxdrv/build/tools/vlink/vlink"
for required in "$tools/ASM56000.EXE" "$vasm" "$vlink" "$mxdrv/src/m68k/xbios.i"; do
    [ -f "$required" ] || [ -f "$required.exe" ] || { echo "error: missing $required" >&2; exit 1; }
done
dosbox=${DOSBOX:-$(command -v dosbox-staging || command -v dosbox || true)}
[ -n "$dosbox" ] || { echo "error: DOSBox Staging is required (set DOSBOX)" >&2; exit 1; }

dsp="$task_dir/build/dsp"
mkdir -p "$dsp" "$task_dir/build/m68k"
for file in ASM56000.EXE CLDLOD.EXE DOS4GW.EXE ioequ.inc; do cp "$tools/$file" "$dsp/"; done
cp "$task_dir/dsp/ssiecho.asm" "$dsp/SSIECHO.ASM"
cat > "$dsp/BUILD.BAT" <<'BATCH'
@ECHO OFF
ASM56000.EXE -q -a -bSSIECHO.CLD -z -lSSIECHO.LST SSIECHO.ASM
IF ERRORLEVEL 1 EXIT 1
CLDLOD.EXE SSIECHO.CLD > SSIECHO.LOD
EXIT
BATCH
rm -f "$dsp/SSIECHO.CLD" "$dsp/SSIECHO.LOD" "$dsp/SSIECHO.LST"
"$dosbox" --noprimaryconf --set output=texture "$dsp/BUILD.BAT" >/dev/null 2>&1 || true
[ -f "$dsp/SSIECHO.LST" ] || { echo "error: SSIECHO.ASM did not assemble; see build/dsp/" >&2; exit 1; }
grep -qE '^0 +Errors' "$dsp/SSIECHO.LST" || { echo "error: SSIECHO.ASM failed; see build/dsp/SSIECHO.LST" >&2; exit 1; }
grep -qE '^0 +Warnings' "$dsp/SSIECHO.LST" || { echo "error: SSIECHO.ASM warned; see build/dsp/SSIECHO.LST" >&2; exit 1; }
python3 "$mxdrv/tools/generate_dsp_stage2.py" --standalone "$dsp/SSIECHO.LOD" --prefix ssiecho \
    > "$dsp/ssiecho_boot.i"

cd "$task_dir"
"$vasm" m68k/ssiecho.s -quiet -Felf -m68030 -I "$mxdrv/src/m68k" -I build/dsp \
    -o build/m68k/ssiecho.o -L build/m68k/ssiecho.lst
"$vlink" build/m68k/ssiecho.o -b ataritos -s -e start -o build/SSIECHO.TOS
echo "built build/SSIECHO.TOS with $(grep SSIECHO_BOOT_WORDS build/dsp/ssiecho_boot.i | awk '{print $NF}') DSP program words"
