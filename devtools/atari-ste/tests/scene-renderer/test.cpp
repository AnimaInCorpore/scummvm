#include "atari-ste-scene.h"
#include <cstdio>
#ifdef __m68k__
extern "C" long scene_nf_id(const char *);
extern "C" long scene_nf_call(long, ...);
asm(".text\n.globl scene_nf_id\nscene_nf_id:\n.word 0x7300\nrts\n"
	".globl scene_nf_call\nscene_nf_call:\n.word 0x7301\nrts\n");
static long cycleId;
static uint32 cycles() { return scene_nf_call(cycleId); }
#else
static uint32 cycles() { return 0; }
#endif

static void checkWidth(int width) {
	Graphics::Surface source(width, 200, 13);
	_RGB palette[256] = {};
	for (int i = 0; i < 256; ++i) {
		palette[i].red = (i & 15) * 16;
		palette[i].green = ((i >> 4) & 15) * 16;
		palette[i].blue = ((i * 7) & 15) * 16;
	}
	for (int y = 0; y < source.h; ++y)
		for (int x = 0; x < width; ++x)
			source.pixels[y * source.pitch + x] = (x + y) & 15;
	AtariSteSceneRenderer renderer;
	AtariSurface destination;
	std::vector<uint16> schedule(renderer.scheduleWords(144));
	uint32 generation = 0;
	renderer.convert(source, destination, palette, schedule.data(), generation);
	renderer.convert(source, destination, palette, schedule.data(), generation);
	const auto originalSchedule = schedule;
	const auto originalGeneration = generation;
	const int offset = (320 - width) / 2;
	const Common::Rect cases[] = {
		{80, 60, 128, 100}, {81, 60, 129, 100}, {-9, -2, 17, 3},
		{303, 140, 335, 203}, {400, 40, 430, 80}, {100, 50, 100, 60},
		{64, 143, 96, 147}, {0, 0, 320, 200}
	};
	for (const auto &rect : cases) {
		// Dirty conversion must match reconversion of the entire chunky frame
		// with the SAME frozen schedule, byte for byte, including clean pixels.
		AtariSteSceneRenderer reference = renderer;
		AtariSurface expected = destination;
		std::vector<uint16> expectedSchedule = schedule;
		uint32 expectedGeneration = generation;
		for (int y = MAX(rect.top, 0); y < MIN(rect.bottom, 200); ++y)
			for (int x = MAX(rect.left, offset); x < MIN(rect.right, offset + width); ++x)
				source.pixels[y * source.pitch + x - offset] ^= 31;
		Screen::DirtyRects dirty = {rect}, full = {Common::Rect(320, 200)};
		destination.convertedPixels = 0;
		renderer.convert(source, destination, palette, schedule.data(), generation, &dirty);
		reference.convert(source, expected, palette, expectedSchedule.data(), expectedGeneration, &full);
		assert(destination.pixels == expected.pixels);
		assert(schedule == originalSchedule && generation == originalGeneration);
		if (rect.left == 80) assert(destination.convertedPixels == 1920);
	}
	// Rotate three buffers with different dirty histories and stale schedules.
	for (int buffer = 0; buffer < 3; ++buffer) {
		AtariSurface actual = destination, expected = destination;
		Screen::DirtyRects dirty = {{16, 20, 64 + buffer * 16, 50}, {32, 35, 80, 65}};
		Screen::DirtyRects full = {Common::Rect(320, 200)};
		AtariSteSceneRenderer reference = renderer;
		uint32 stale = 0, current = generation;
		std::vector<uint16> packed(schedule.size()), expectedPacked = schedule;
		for (const auto &rect : dirty)
			for (int y = rect.top; y < rect.bottom; ++y)
				for (int x = MAX(offset, rect.left); x < MIN(offset + width, rect.right); ++x)
					source.pixels[y * source.pitch + x - offset] ^= 7;
		renderer.convert(source, actual, palette, packed.data(), stale, &dirty);
		reference.convert(source, expected, palette, expectedPacked.data(), current, &full);
		assert(actual.pixels == expected.pixels && packed == schedule && stale == generation);
		destination = actual;
	}
	Screen::DirtyRects warm = {{80, 60, 128, 100}}, rows = {{0, 60, 320, 100}};
	// Prime lazy mapping, then measure the same warm conversion in both shapes.
	renderer.convert(source, destination, palette, schedule.data(), generation, &rows);
	uint32 start = cycles();
	renderer.convert(source, destination, palette, schedule.data(), generation, &warm);
	uint32 rectCycles = cycles() - start;
	start = cycles();
	renderer.convert(source, destination, palette, schedule.data(), generation, &rows);
	uint32 rowCycles = cycles() - start;
	printf("SCENE_RECT width=%d rect_pixels=1920 row_pixels=12800 rect_cycles=%lu row_cycles=%lu\n",
		width, (unsigned long)rectCycles, (unsigned long)rowCycles);
}

int main() {
#ifdef __m68k__
	cycleId = scene_nf_id("NF_CYCLES");
	assert(cycleId);
#endif
	checkWidth(320);
	checkWidth(304);
	checkWidth(319);
	puts("SCENE_RECT PASS: clipping, alignment, padding, overlap, panel, buffer generations, byte-exact output");
#ifdef __m68k__
	fflush(stdout);
	scene_nf_call(scene_nf_id("NF_EXIT"), 0L);
#endif
	return 0;
}
