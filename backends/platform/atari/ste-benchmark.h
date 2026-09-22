/* Optional Hatari measurement hooks. No timing source or raster changes. */
#ifndef BACKENDS_PLATFORM_ATARI_STE_BENCHMARK_H
#define BACKENDS_PLATFORM_ATARI_STE_BENCHMARK_H
#include "common/scummsys.h"
#ifdef ATARI_STE_GAME_ONLY
extern "C" {
// All fields are longs so the debugger can dump a stable, endian-defined record.
struct AtariSteBenchmarkState {
	uint32 enabled, serial, phase, frame, room;
	uint32 engineActive, rendererActive, ego, x, y, moving, cameraX, userPut;
	uint32 conversions, pixels, rebuilds, mapMisses, visibleActors, sceneHeight;
};
extern AtariSteBenchmarkState atari_ste_bench_state;
// The last engine frame handed to the STE converter and the _RGB[256]
// palette it is converted with, set in every build for debugger captures.
extern const void *atari_ste_last_source;
extern const void *atari_ste_last_palette;
void atari_ste_bench_marker();
void atari_ste_bench_mark(uint32 phase);
// The room the engine is showing. The STE graphics manager installs that
// room's colour-mixing table if one is next to the default table; the call is
// ignored on every other display path.
void atari_ste_scene_room(int room);
}
#endif
#endif
