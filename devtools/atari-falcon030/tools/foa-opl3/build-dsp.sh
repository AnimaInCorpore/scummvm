#!/bin/sh
# Build the DSP kernels and their Falcon hosts. Both use the sibling
# F030MXDRV checkout's toolchain: Motorola's asm56000 under dosbox, and
# vasm/vlink. Two programs are built:
#   OPLBENCH.TOS  the exact synthesis loop benchmark (dsp/opl.asm)
#   OPLRT.TOS     the practical block-rate kernel bench (dsp/oplrt.asm),
#                 booted through the sibling's two-stage loader
#   OPLPLAY.TOS   the same kernel in its SSI stream mode, fed a captured
#                 register stream through the period refill protocol
#
# The checkout is found as gate_env.py finds it: $MXDRV, else under
# ~/Work, else beside this repository. DOSBox is $DOSBOX, else
# dosbox-staging or dosbox on the path.
set -eu
task_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mxdrv=${MXDRV:-$HOME/Work/F030MXDRV}
if [ -z "${MXDRV:-}" ] && [ ! -d "$mxdrv" ]; then
    mxdrv=$(CDPATH= cd -- "$task_dir/../../../../.." && pwd)/F030MXDRV
fi
tools="$mxdrv/third_party/f030dsp3d/tools/asm56k"
vasm="$mxdrv/build/tools/vasm/vasmm68k_mot"
vlink="$mxdrv/build/tools/vlink/vlink"
for required in "$tools/ASM56000.EXE" "$vasm" "$vlink" "$mxdrv/src/dsp/stage2_loader.asm"; do
    [ -f "$required" ] || { echo "error: missing $required" >&2; exit 1; }
done
dosbox=${DOSBOX:-$(command -v dosbox-staging || command -v dosbox || true)}
[ -n "$dosbox" ] || { echo "error: DOSBox Staging is required (set DOSBOX)" >&2; exit 1; }

mkdir -p "$task_dir/build"
python3 "$task_dir/generate-tables.py" --header "$task_dir/opl-tables.h" \
    --practical-header "$task_dir/opl-practical-tables.h" --practical-dsp "$task_dir/dsp/oplrttab.inc"

cd "$task_dir/dsp"
for file in ASM56000.EXE CLDLOD.EXE DOS4GW.EXE ioequ.inc; do cp "$tools/$file" .; done
cp "$mxdrv/src/dsp/stage2_loader.asm" OPLBOOT.ASM
# The repository ignores *.bat, so the assembler's batch file is generated.
cat > BUILD.BAT <<'BATCH'
@ECHO OFF
ASM56000.EXE -q -a -bOPL.CLD -z -lOPL.LST OPL.ASM
IF ERRORLEVEL 1 EXIT 1
CLDLOD.EXE OPL.CLD > OPL.LOD
IF ERRORLEVEL 1 EXIT 1
ASM56000.EXE -q -a -bOPLRT.CLD -z -lOPLRT.LST OPLRT.ASM
IF ERRORLEVEL 1 EXIT 1
CLDLOD.EXE OPLRT.CLD > OPLRT.LOD
IF ERRORLEVEL 1 EXIT 1
ASM56000.EXE -q -a -bOPLBOOT.CLD -z -lOPLBOOT.LST OPLBOOT.ASM
IF ERRORLEVEL 1 EXIT 1
CLDLOD.EXE OPLBOOT.CLD > OPLBOOT.LOD
EXIT
BATCH
rm -f OPL.CLD OPL.LOD OPL.LST OPLRT.CLD OPLRT.LOD OPLRT.LST OPLBOOT.CLD OPLBOOT.LOD OPLBOOT.LST
"$dosbox" --noprimaryconf --set output=texture "$task_dir/dsp/BUILD.BAT" >/dev/null 2>&1 || true
for listing in OPL OPLRT OPLBOOT; do
    [ -f "$listing.LST" ] || { echo "error: $listing.ASM did not assemble; see dsp/" >&2; exit 1; }
    grep -qE '^0 +Errors' "$listing.LST" || { echo "error: $listing.ASM failed; see dsp/$listing.LST" >&2; exit 1; }
    grep -qE '^0 +Warnings' "$listing.LST" || { echo "error: $listing.ASM warned; see dsp/$listing.LST" >&2; exit 1; }
done
python3 "$mxdrv/tools/generate_dsp_stage2.py" --standalone OPL.LOD --prefix opl > opl_boot.i
python3 "$mxdrv/tools/generate_dsp_stage2.py" --bootstrap OPLBOOT.LOD --program OPLRT.LOD > oplrt_image.i

mkdir -p "$task_dir/build/m68k"
cd "$task_dir"
"$vasm" m68k/oplbench.s -quiet -Felf -m68030 -I "$mxdrv/src/m68k" -I dsp \
    -o build/m68k/oplbench.o -L build/m68k/oplbench.lst
"$vlink" build/m68k/oplbench.o -b ataritos -s -e start -o build/OPLBENCH.TOS
"$vasm" m68k/oplrt.s -quiet -Felf -m68030 -I "$mxdrv/src/m68k" -I dsp \
    -o build/m68k/oplrt.o -L build/m68k/oplrt.lst
"$vlink" build/m68k/oplrt.o -b ataritos -s -e start -o build/OPLRT.TOS
"$vasm" m68k/oplplay.s -quiet -Felf -m68030 -I "$mxdrv/src/m68k" -I dsp \
    -o build/m68k/oplplay.o -L build/m68k/oplplay.lst
"$vlink" build/m68k/oplplay.o -b ataritos -s -e start -o build/OPLPLAY.TOS
# The ScummVM backend embeds the same kernel; the Falcon build cannot run the
# DOS assembler, so its image header is generated here and committed.
python3 "$task_dir/generate-image-header.py" --bootstrap dsp/OPLBOOT.LOD --program dsp/OPLRT.LOD \
    --mxdrv-tools "$mxdrv/tools" --output "$task_dir/../../../../backends/platform/atari/dsp-opl-image.h"
echo "built build/OPLBENCH.TOS with $(grep OPL_BOOT_WORDS dsp/opl_boot.i | awk '{print $NF}') DSP program words"
echo "built build/OPLRT.TOS: $(grep DSP_STAGE2_PROGRAM_WORDS dsp/oplrt_image.i | awk '{print $NF}') DSP program words in $(grep DSP_STAGE2_SECTION_COUNT dsp/oplrt_image.i | awk '{print $NF}') sections"
