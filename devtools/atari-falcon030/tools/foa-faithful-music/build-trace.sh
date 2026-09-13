#!/bin/sh
set -eu
task_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$task_dir/../../../.." && pwd)
mkdir -p "$task_dir/build/headless"
cd "$task_dir/build/headless"
if [ ! -f config.mk ]; then
    "$source_dir/configure" --backend=null --disable-all-engines \
        --enable-engine=scumm --disable-engine=scumm_7_8 --disable-engine=he \
        --disable-detection-full --opengl-mode=none --disable-tts \
        --disable-libcurl --disable-fluidsynth --disable-mt32emu \
        --disable-readline --disable-debug --enable-optimizations
fi
make -j8 -f Makefile -f "$task_dir/trace.mk" scummvm
