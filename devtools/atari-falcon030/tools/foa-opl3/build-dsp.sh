#!/bin/sh
# Build the DSP kernel and its Falcon host. Both use the sibling F030MXDRV
# checkout's toolchain: Motorola's asm56000 under dosbox, and vasm/vlink.
set -eu
task_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mxdrv=${MXDRV:-$HOME/Work/F030MXDRV}
tools="$mxdrv/third_party/f030dsp3d/tools/asm56k"
vasm="$mxdrv/build/tools/vasm/vasmm68k_mot"
vlink="$mxdrv/build/tools/vlink/vlink"
for required in "$tools/ASM56000.EXE" "$vasm" "$vlink"; do
    [ -f "$required" ] || { echo "error: missing $required" >&2; exit 1; }
done
command -v dosbox-staging >/dev/null 2>&1 || { echo "error: dosbox-staging is required" >&2; exit 1; }

cd "$task_dir/dsp"
for file in ASM56000.EXE CLDLOD.EXE DOS4GW.EXE ioequ.inc; do cp "$tools/$file" .; done
# The repository ignores *.bat, so the assembler's batch file is generated.
cat > BUILD.BAT <<'BATCH'
@ECHO OFF
ASM56000.EXE -q -a -bOPL.CLD -z -lOPL.LST OPL.ASM
IF ERRORLEVEL 1 EXIT 1
CLDLOD.EXE OPL.CLD > OPL.LOD
EXIT
BATCH
rm -f OPL.CLD OPL.LOD OPL.LST
dosbox-staging --noprimaryconf --set output=texture "$task_dir/dsp/BUILD.BAT" >/dev/null 2>&1 || true
grep -qE '^0 +Errors' OPL.LST || { echo "error: the DSP assembly failed; see dsp/OPL.LST" >&2; exit 1; }
grep -qE '^0 +Warnings' OPL.LST || { echo "error: the DSP assembly warned; see dsp/OPL.LST" >&2; exit 1; }
python3 "$mxdrv/tools/generate_dsp_stage2.py" --standalone OPL.LOD --prefix opl > opl_boot.i

mkdir -p "$task_dir/build/m68k"
cd "$task_dir"
"$vasm" m68k/oplbench.s -quiet -Felf -m68030 -I "$mxdrv/src/m68k" -I dsp \
    -o build/m68k/oplbench.o -L build/m68k/oplbench.lst
"$vlink" build/m68k/oplbench.o -b ataritos -s -e start -o build/OPLBENCH.TOS
echo "built build/OPLBENCH.TOS with $(grep OPL_BOOT_WORDS dsp/opl_boot.i | awk '{print $NF}') DSP program words"
