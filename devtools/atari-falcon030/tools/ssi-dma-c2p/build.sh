#!/bin/sh
# Build the SSI DMA tests with the sibling F030MXDRV checkout's toolchain:
# Motorola's asm56000 under DOSBox, and vasm/vlink. These programs are built:
#   SSIECHO.TOS  the pass-through test (dsp/ssiecho.asm)
#   SSIC2P.TOS   the c2p test (dsp/ssic2p.asm, its group conversion
#                generated and checked by gen-c2p.py)
#   SSIMIX.TOS   the slot sharing test (dsp/ssimix.asm): the DAC on one
#                slot pair of the frame
# and, with the cross compiler, CPUC2P.TOS (see below). Everything
# generated lands in build/.
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
cp "$task_dir/dsp/ssic2p.asm" "$dsp/SSIC2P.ASM"
cp "$task_dir/dsp/ssimix.asm" "$dsp/SSIMIX.ASM"
python3 "$task_dir/gen-c2p.py" --output "$dsp/C2PCORE.INC"
cat > "$dsp/BUILD.BAT" <<'BATCH'
@ECHO OFF
ASM56000.EXE -q -a -bSSIECHO.CLD -z -lSSIECHO.LST SSIECHO.ASM
IF ERRORLEVEL 1 EXIT 1
CLDLOD.EXE SSIECHO.CLD > SSIECHO.LOD
IF ERRORLEVEL 1 EXIT 1
ASM56000.EXE -q -a -bSSIC2P.CLD -z -lSSIC2P.LST SSIC2P.ASM
IF ERRORLEVEL 1 EXIT 1
CLDLOD.EXE SSIC2P.CLD > SSIC2P.LOD
IF ERRORLEVEL 1 EXIT 1
ASM56000.EXE -q -a -bSSIMIX.CLD -z -lSSIMIX.LST SSIMIX.ASM
IF ERRORLEVEL 1 EXIT 1
CLDLOD.EXE SSIMIX.CLD > SSIMIX.LOD
EXIT
BATCH
for name in SSIECHO SSIC2P SSIMIX; do rm -f "$dsp/$name.CLD" "$dsp/$name.LOD" "$dsp/$name.LST"; done
"$dosbox" --noprimaryconf --set output=texture "$dsp/BUILD.BAT" >/dev/null 2>&1 || true
for name in SSIECHO SSIC2P SSIMIX; do
    [ -f "$dsp/$name.LST" ] || { echo "error: $name.ASM did not assemble; see build/dsp/" >&2; exit 1; }
    grep -qE '^0 +Errors' "$dsp/$name.LST" || { echo "error: $name.ASM failed; see build/dsp/$name.LST" >&2; exit 1; }
    grep -qE '^0 +Warnings' "$dsp/$name.LST" || { echo "error: $name.ASM warned; see build/dsp/$name.LST" >&2; exit 1; }
done
python3 "$mxdrv/tools/generate_dsp_stage2.py" --standalone "$dsp/SSIECHO.LOD" --prefix ssiecho \
    > "$dsp/ssiecho_boot.i"
python3 "$mxdrv/tools/generate_dsp_stage2.py" --standalone "$dsp/SSIC2P.LOD" --prefix ssic2p \
    > "$dsp/ssic2p_boot.i"
python3 "$mxdrv/tools/generate_dsp_stage2.py" --standalone "$dsp/SSIMIX.LOD" --prefix ssimix \
    > "$dsp/ssimix_boot.i"

cd "$task_dir"
for name in ssiecho ssic2p ssimix; do
    "$vasm" m68k/$name.s -quiet -Felf -m68030 -I "$mxdrv/src/m68k" -I m68k -I build/dsp \
        -o build/m68k/$name.o -L build/m68k/$name.lst
    upper=$(echo "$name" | tr a-z A-Z)
    "$vlink" build/m68k/$name.o -b ataritos -s -e start -o build/$upper.TOS
    echo "built build/$upper.TOS with $(grep -i "${name}_BOOT_WORDS" build/dsp/${name}_boot.i | awk '{print $NF}') DSP program words"
done

# The 68030 c2p the backend uses, timed for comparison (cpu/cpuc2p.c). It
# needs the m68k-atari-mintelf cross compiler: $CROSS is its prefix. The c2p
# is assembled for the 68030 as the backend's is; the harness links the
# 68000 mintlib, whose 68020-60 variant would demand an FPU the stock
# Falcon lacks.
cross=${CROSS:-m68k-atari-mintelf-}
if command -v "${cross}gcc" >/dev/null 2>&1; then
    repo=$(CDPATH= cd -- "$task_dir/../../../.." && pwd)
    "${cross}gcc" -m68030 -c -o build/m68k/atari-c2p-asm.o "$repo/backends/graphics/atari/atari-c2p-asm.S"
    "${cross}gcc" -O2 -fomit-frame-pointer -Wall -o build/CPUC2P.TOS \
        cpu/cpuc2p.c build/m68k/atari-c2p-asm.o
    echo "built build/CPUC2P.TOS"
else
    echo "skipped build/CPUC2P.TOS: no ${cross}gcc (set CROSS)"
fi
