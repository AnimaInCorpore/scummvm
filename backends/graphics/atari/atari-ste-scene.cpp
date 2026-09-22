/* ScummVM - Graphic Adventure Engine
 *
 * This is the Atari STE frame compositor. SCUMM keeps drawing into its normal
 * 8-bit surface; this class performs one final scene conversion after all
 * background, objects, actors, text, and scrolling have been composed.
 */

#define FORCE_TEXT_CONSOLE

#include "atari-ste-scene.h"

#include <cstring>

#include "atari-surface.h"
#include "backends/platform/atari/ste-benchmark.h"
#include "common/debug.h"
#include "common/fs.h"
#include "common/ptr.h"
#include "common/stream.h"
#include "common/textconsole.h"

#ifdef ATARI_STE_GAME_ONLY
extern "C" {
AtariSteBenchmarkState atari_ste_bench_state = {};
const void *atari_ste_last_source = nullptr;
const void *atari_ste_last_palette = nullptr;
void __attribute__((noinline)) atari_ste_bench_marker() {
	__asm__ volatile("" : : : "memory");
}
void atari_ste_bench_mark(uint32 phase) {
	atari_ste_bench_state.phase = phase;
	++atari_ste_bench_state.serial;
	atari_ste_bench_marker();
}
}
#endif

namespace {

// Linear 12-bit colour, four bits per channel, red in the top nibble. All
// colour arithmetic in this file works on this form.
static uint16 toSteColor(const _RGB &color) {
	return (uint16)((((uint16)(color.red + 8) >> 4) & 0x0f) << 8
		| (((uint16)(color.green + 8) >> 4) & 0x0f) << 4
		| (((uint16)(color.blue + 8) >> 4) & 0x0f));
}

// STE palette register encoding: within each nibble the hardware keeps the
// ST-compatible high three bits in bits 0..2 and the fourth, least
// significant bit in bit 3.
static uint16 toHardwareWord(uint16 linear) {
	return (uint16)(((linear & 0x0eee) >> 1) | ((linear & 0x0111) << 3));
}

static const uint16 kSquares[16] = {
	0, 1, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 144, 169, 196, 225
};

// Scaling every channel by 17 before squaring, as this used to do, multiplies
// the result by a constant 289 and so cannot change which palette entry comes
// out nearest. Working on the raw 0..15 nibbles and reading the squares from a
// table gives the same ranking without a single MULU, which costs ~70 cycles
// each on a 68000 and dominated the whole frame conversion.
static int colorDistance(uint16 color, uint16 steColor) {
	int dr = (int)((color >> 8) & 0x0f) - (int)((steColor >> 8) & 0x0f);
	int dg = (int)((color >> 4) & 0x0f) - (int)((steColor >> 4) & 0x0f);
	int db = (int)(color & 0x0f) - (int)(steColor & 0x0f);

	if (dr < 0)
		dr = -dr;
	if (dg < 0)
		dg = -dg;
	if (db < 0)
		db = -db;

	return kSquares[dr] + kSquares[dg] + kSquares[db];
}

static bool testColorBit(const uint8 *bits, uint16 color) {
	return (bits[color >> 3] & (1 << (color & 7))) != 0;
}

static void setColorBit(uint8 *bits, uint16 color) {
	bits[color >> 3] |= (uint8)(1 << (color & 7));
}

static void clearColorBit(uint8 *bits, uint16 color) {
	bits[color >> 3] &= (uint8)~(1 << (color & 7));
}

} // namespace

AtariSteSceneRenderer::AtariSteSceneRenderer()
	: _sceneLines(144), _scheduleGeneration(1), _hasSchedule(false), _hasPreviousPalette(false) {
	memset(_previousPalettes, 0, sizeof(_previousPalettes));
	memset(_schedulePalettes, 0, sizeof(_schedulePalettes));
	memset(_scheduleMap, 0, sizeof(_scheduleMap));
	memset(_scheduleMapValid, 0, sizeof(_scheduleMapValid));
	memset(_histogram, 0, sizeof(_histogram));
	memset(_steInSource, 0, sizeof(_steInSource));
	memset(_steChosen, 0, sizeof(_steChosen));
}

AtariSteSceneRenderer::~AtariSteSceneRenderer() {
}

void AtariSteSceneRenderer::setSceneLines(int lines) {
	lines = CLIP<int>(lines, ATARI_STE_RASTER_MIN_LINES, ATARI_STE_RASTER_MAX_LINES);
	if (lines == _sceneLines)
		return;

	_sceneLines = lines;
	_hasSchedule = false;
}

void AtariSteSceneRenderer::choosePalette(const Graphics::Surface &source, int firstRow,
		int rowCount, int width, const uint16 *sourceColors, uint16 *linePalette) {
	uint16 palette[16] = {};
	byte present[256];
	int presentCount = 0;
	int count = 0;

	// Rows that share a palette are histogrammed together; 198 rows of 320
	// pixels still fit the 16-bit counters.
	for (int r = 0; r < rowCount; ++r) {
		const byte *sourceRow = (const byte *)source.getBasePtr(0, firstRow + r);

		for (int x = 0; x < width; ++x)
			_histogram[sourceRow[x]]++;
	}

	// The candidate list is gathered in ascending colour order so that the
	// selection below still resolves equal pixel counts towards the lowest
	// index, and so that the passes that follow only walk the colours this
	// scanline actually uses instead of all 256.
	for (int color = 0; color < 256; ++color) {
		if (_histogram[color])
			present[presentCount++] = (byte)color;
	}

	// Preserve colours already used by this raster line. This keeps static room
	// pixels stable while a small actor moves through the line.
	if (_hasPreviousPalette) {
		const uint16 *previous = _previousPalettes + firstRow * 16;

		for (int i = 0; i < 16 && count < 16; ++i) {
			if (previous[i] && testColorBit(_steInSource, previous[i])) {
				palette[count++] = previous[i];
				setColorBit(_steChosen, previous[i]);
			}
		}
	}

	while (count < 16) {
		int best = -1;

		for (int i = 0; i < presentCount; ++i) {
			const int color = present[i];

			if (testColorBit(_steChosen, sourceColors[color]))
				continue;
			if (best < 0 || _histogram[color] > _histogram[best])
				best = color;
		}

		if (best < 0)
			break;

		palette[count++] = sourceColors[best];
		setColorBit(_steChosen, sourceColors[best]);
	}

	while (count < 16) {
		const int slot = count++;
		palette[slot] = slot == 0 ? 0 : palette[slot - 1];
	}

	// Hand both scratch arrays back to the next scanline in their empty state.
	// Every bit set above belongs to one of the palette entries, and clearing a
	// bit that was never set is harmless.
	for (int i = 0; i < presentCount; ++i)
		_histogram[present[i]] = 0;

	for (int i = 0; i < 16; ++i)
		clearColorBit(_steChosen, palette[i]);

	memcpy(linePalette, palette, sizeof(palette));
}

void AtariSteSceneRenderer::quantizeRow(const byte *sourceRow, byte *destinationRow,
		int width, const uint16 *sourceColors, const uint16 *linePalette, int row) {
	byte *map = _scheduleMap[row];
	uint8 *valid = _scheduleMapValid[row];

	for (int x = 0; x < width; ++x) {
		const byte color = sourceRow[x];
		if (!(valid[color >> 3] & (1 << (color & 7)))) {
#ifdef ATARI_STE_GAME_ONLY
			if (atari_ste_bench_state.enabled)
				++atari_ste_bench_state.mapMisses;
#endif
			const uint16 steColor = sourceColors[color];
			int bestSlot = -1;

			// The line palette is built out of this row's own colours, so most
			// pixels match an entry exactly. That entry is at distance zero and
			// the scan below keeps the first minimum, so taking the first exact
			// match is the same answer without computing any distances.
			for (int slot = 0; slot < 16; ++slot) {
				if (linePalette[slot] == steColor) {
					bestSlot = slot;
					break;
				}
			}

			if (bestSlot < 0) {
				int bestDistance = colorDistance(steColor, linePalette[0]);
				bestSlot = 0;
				for (int slot = 1; slot < 16; ++slot) {
					const int distance = colorDistance(steColor, linePalette[slot]);
					if (distance < bestDistance) {
						bestDistance = distance;
						bestSlot = slot;
					}
				}
			}

			map[color] = (byte)bestSlot;
			valid[color >> 3] |= (uint8)(1 << (color & 7));
		}
		destinationRow[x] = map[color];
	}
}

void AtariSteSceneRenderer::packSchedule(uint16 *schedule) const {
	// Layout: atari-ste-raster.h. One palette per line is expressed by
	// repeating the line's own palette in the two visible groups and putting
	// the next line's palette into the border group.
	const uint16 *rows = _schedulePalettes;
	uint16 *out = schedule;

	// Lines 0 and 1 (identical rows), written at VBL.
	for (int i = 0; i < 16; ++i)
		*out++ = toHardwareWord(rows[i]);

	// Line 1's border group: the palette line 2 starts with.
	for (int i = 0; i < 16; ++i)
		*out++ = toHardwareWord(rows[2 * 16 + i]);

	for (int y = 2; y < _sceneLines; ++y) {
		const uint16 *own = rows + y * 16;
		const uint16 *next = rows + MIN(y + 1, 199) * 16;

		for (int i = 0; i < 16; ++i)
			out[i] = out[16 + i] = toHardwareWord(own[i]);
		for (int i = 0; i < 16; ++i)
			out[32 + i] = toHardwareWord(next[i]);

		out += ATARI_STE_RASTER_LINE_WORDS;
	}
}

void AtariSteSceneRenderer::convertRect(const Graphics::Surface &source,
		AtariSurface &destination, const uint16 *sourceColors, Common::Rect rect) {
	const int width = MIN<int>(source.w, 320);
	const int height = MIN<int>(source.h, 200);
	const int xOffset = (destination.w - source.w) / 2;
	// Screen dirty rectangles are in destination coordinates. Keep their
	// aligned padding, but never read beyond a smaller source surface.
	rect.clip(Common::Rect(destination.w, height));
	if (rect.isEmpty())
		return;
	rect.left &= ~15;
	rect.right = MIN<int>((rect.right + 15) & ~15, destination.w);
#ifdef ATARI_STE_GAME_ONLY
	if (atari_ste_bench_state.enabled)
		atari_ste_bench_state.pixels += rect.width() * (rect.bottom - rect.top);
#endif
	assert(rect.width() % 16 == 0);
	byte quantizedRow[320];
	const int sourceLeft = MAX<int>(rect.left, xOffset);
	const int sourceRight = MIN<int>(rect.right, xOffset + width);
	for (int y = rect.top; y < rect.bottom; ++y) {
		if (sourceLeft > rect.left || sourceRight < rect.right)
			memset(quantizedRow, 0, rect.width());
		if (sourceRight > sourceLeft) {
			const byte *sourceRow = (const byte *)source.getBasePtr(sourceLeft - xOffset, y);
			quantizeRow(sourceRow, quantizedRow + sourceLeft - rect.left,
				sourceRight - sourceLeft, sourceColors, _schedulePalettes + y * 16, y);
		}
		destination.copyRectToSurface(quantizedRow, rect.width(), rect.left, y, rect.width(), 1);
	}
}

void AtariSteSceneRenderer::convert(const Graphics::Surface &source,
		AtariSurface &destination, const _RGB *sourcePalette, uint16 *schedule,
		uint32 &scheduleGeneration, const Screen::DirtyRects *dirtyRects) {
	const int width = MIN<int>(source.w, 320);
	const int height = MIN<int>(source.h, 200);
	byte quantizedRow[320];
	uint16 sourceColors[256];

	// Membership of the source palette is asked once per previous-palette slot
	// per scanline; a bitmap over the 12-bit STE colour space answers that in
	// constant time instead of rescanning all 256 entries.
	memset(_steInSource, 0, sizeof(_steInSource));
	for (int color = 0; color < 256; ++color) {
		sourceColors[color] = toSteColor(sourcePalette[color]);
		setColorBit(_steInSource, sourceColors[color]);
	}

	if (!_hasPreviousPalette) {
		for (int y = 0; y < 200; ++y) {
			uint16 *linePalette = _schedulePalettes + y * 16;
			for (int slot = 0; slot < 16; ++slot)
				linePalette[slot] = sourceColors[0];
		}
		memset(destination.getPixels(), 0, destination.pitch * destination.h);
		memcpy(_previousPalettes, _schedulePalettes, sizeof(_previousPalettes));
		_hasPreviousPalette = true;
		_hasSchedule = false;
		++_scheduleGeneration;
		packSchedule(schedule);
		scheduleGeneration = _scheduleGeneration;
		return;
	}

	// Reselecting a palette per scanline per frame is what made this expensive.
	// A full redraw - which a room change or any palette change forces on every
	// buffer - is the only thing that picks a new schedule; while it stands the
	// background stays quantized and later frames only restamp the rectangles the
	// engine actually changed.
	const bool rebuildSchedule = (dirtyRects == nullptr) || !_hasSchedule;

	if (rebuildSchedule) {
#ifdef ATARI_STE_GAME_ONLY
		if (atari_ste_bench_state.enabled)
			++atari_ste_bench_state.rebuilds;
#endif
		const int sceneLines = MIN(_sceneLines, height);
		const int topRows = MIN(2, height);
		uint16 shared[16];

		// The raster takes over after line 0, so lines 0 and 1 share the
		// palette written at VBL.
		choosePalette(source, 0, topRows, width, sourceColors, shared);
		for (int y = 0; y < topRows; ++y)
			memcpy(_schedulePalettes + y * 16, shared, sizeof(shared));

		for (int y = topRows; y < sceneLines; ++y)
			choosePalette(source, y, 1, width, sourceColors, _schedulePalettes + y * 16);

		// Everything below the raster region shares the palette its last
		// border group installs.
		if (sceneLines < height) {
			choosePalette(source, sceneLines, height - sceneLines, width, sourceColors, shared);
			for (int y = sceneLines; y < height; ++y)
				memcpy(_schedulePalettes + y * 16, shared, sizeof(shared));
		}

		for (int y = MAX(height, 1); y < 200; ++y)
			memcpy(_schedulePalettes + y * 16, _schedulePalettes + (y - 1) * 16, 16 * sizeof(uint16));

		memcpy(_previousPalettes, _schedulePalettes, sizeof(_previousPalettes));
		memset(_scheduleMapValid, 0, sizeof(_scheduleMapValid));
		_hasSchedule = true;
		++_scheduleGeneration;
	}

	// Every buffer displays the same schedule; a buffer receives the packed
	// stream once per schedule, not once per frame.
	if (scheduleGeneration != _scheduleGeneration) {
		packSchedule(schedule);
		scheduleGeneration = _scheduleGeneration;
	}

	if (rebuildSchedule)
		convertRect(source, destination, sourceColors, Common::Rect(destination.w, height));
	else
		for (const Common::Rect &rect : *dirtyRects)
			convertRect(source, destination, sourceColors, rect);

	if (rebuildSchedule) {
		for (int y = height; y < 200; ++y) {
			memset(quantizedRow, 0, sizeof(quantizedRow));
			destination.copyRectToSurface(quantizedRow, 320, 0, y, 320, 1);
		}
	}

	_hasPreviousPalette = true;
}

namespace {

static const uint16 kMixSquares[31] = {
	0, 1, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 144, 169, 196, 225,
	256, 289, 324, 361, 400, 441, 484, 529, 576, 625, 676, 729, 784, 841, 900
};

// Linear 12-bit colour of a hardware palette word (inverse of toHardwareWord).
static uint16 fromHardwareWord(uint16 word) {
	return (uint16)(((word & 0x0777) << 1) | ((word & 0x0888) >> 3));
}

// MiNTLib declares the _RGB components as plain char, which is signed here.
static void unpackColor(const _RGB &color, byte *rgb) {
	rgb[0] = (byte)color.red;
	rgb[1] = (byte)color.green;
	rgb[2] = (byte)color.blue;
}

static bool samePalette(const byte *cached, const _RGB *palette) {
	for (int color = 0; color < 256; ++color) {
		byte rgb[3];
		unpackColor(palette[color], rgb);
		if (memcmp(cached + color * 3, rgb, 3))
			return false;
	}
	return true;
}

} // namespace

bool AtariSteSceneRenderer::loadMix(const Common::Path &path, MixPattern pattern) {
	_mixEnabled = false;

	const Common::String name = path.toString(Common::Path::kNativeSeparator);
	Common::ScopedPtr<Common::SeekableReadStream> stream(Common::FSNode(path).createReadStream());
	if (!stream) {
		warning("STE mix: cannot open %s", name.c_str());
		return false;
	}

	// Big-endian palette words, a pair of palette slots per source colour,
	// then the source palette the pairs were computed for. A split table has
	// a palette set and slot pairs for the room and another for the verb bar.
	// The words come per field (one field for one-palette tables), each field
	// in room, verb-bar order.
	const int64 size = stream->size();
	const uint32 pairBytes = sizeof(_mixLut[0]), sourceBytes = sizeof(_mixSource);
	int regions = 0, fields = 0;
	if (size == 16 * 2 + pairBytes + sourceBytes) {
		regions = 1;
		fields = 1;
	} else if (size == 32 * 2 + pairBytes + sourceBytes) {
		regions = 1;
		fields = 2;
	} else if (size == 32 * 2 + 2 * pairBytes + sourceBytes) {
		regions = 2;
		fields = 1;
	} else if (size == 64 * 2 + 2 * pairBytes + sourceBytes) {
		regions = 2;
		fields = 2;
	} else {
		warning("STE mix: %s has an unexpected size of %ld bytes", name.c_str(), (long)size);
		return false;
	}

	for (int field = 0; field < fields; ++field) {
		for (int region = 0; region < regions; ++region) {
			for (int i = 0; i < 16; ++i)
				_mixWords[(field * 2 + region) * 16 + i] = stream->readUint16BE();
		}
		if (regions == 1)
			memcpy(_mixWords + (field * 2 + 1) * 16, _mixWords + field * 2 * 16, 16 * sizeof(uint16));
	}
	if (fields == 1)
		memcpy(_mixWords + 32, _mixWords, 32 * sizeof(uint16));

	for (int region = 0; region < regions; ++region) {
		if (stream->read(_mixLut[region], pairBytes) != pairBytes) {
			warning("STE mix: cannot read %s", name.c_str());
			return false;
		}
		for (uint i = 0; i < pairBytes; ++i) {
			if (_mixLut[region][i] > 15) {
				warning("STE mix: %s contains an invalid palette slot", name.c_str());
				return false;
			}
		}
	}
	if (regions == 1)
		memcpy(_mixLut[1], _mixLut[0], pairBytes);
	if (stream->read(_mixSource, sourceBytes) != sourceBytes || stream->err()) {
		warning("STE mix: cannot read %s", name.c_str());
		return false;
	}

	_mixSplit = regions == 2;
	_mixDual = fields == 2;
	// A checkerboard needs both colours of a pair in the palette of each
	// field, which two different palettes do not provide.
	_mixPattern = _mixDual ? kMixAlternate : pattern;
	_mixTablesValid = false;
	_mixEnabled = true;
	// Every buffer's packed palette words are stale after a new table; any
	// non-zero generation marks them as live for presentation again.
	++_mixGeneration;

	debug("STE mix: %s, %s, %s%s", name.c_str(), _mixDual ? "two palettes" : "one palette",
		_mixPattern == kMixChecker ? "checkerboard" : _mixPattern == kMixAlternate ? "alternating" : "static checkerboard",
		_mixSplit ? ", separate verb-bar palette" : "");
	return true;
}

void AtariSteSceneRenderer::nearestMix(int region, const _RGB &color, byte &first, byte &second) const {
	// Colours the table was not computed for. Compares the sum of the two
	// 4-bit levels (0..30) with the 8-bit colour on the same scale; this
	// ignores gamma, unlike the table, but needs no multiplication.
	byte rgb[3];
	unpackColor(color, rgb);
	const int target[3] = {
		(rgb[0] * 2 + 8) / 17, (rgb[1] * 2 + 8) / 17, (rgb[2] * 2 + 8) / 17
	};
	const uint16 *palette0 = _mixWords + region * 16, *palette1 = _mixWords + (2 + region) * 16;
	int best = 0x7fffffff;

	first = second = 0;
	for (int a = 0; a < 16; ++a) {
		const uint16 ca = fromHardwareWord(palette0[a]);

		for (int b = _mixDual ? 0 : a; b < 16; ++b) {
			const uint16 cb = fromHardwareWord(palette1[b]);
			int distance = 0;

			for (int channel = 0; channel < 3; ++channel) {
				const int shift = 8 - 4 * channel;
				int d = (int)((ca >> shift) & 0x0f) + (int)((cb >> shift) & 0x0f) - target[channel];
				if (d < 0)
					d = -d;
				distance += kMixSquares[d];
			}
			if (distance < best) {
				best = distance;
				first = (byte)a;
				second = (byte)b;
			}
		}
	}
}

void AtariSteSceneRenderer::buildMixTables(const _RGB *sourcePalette) {
	const int regions = _mixSplit ? 2 : 1;
	int matched = 0;

	for (int color = 0; color < 256; ++color) {
		byte rgb[3];
		unpackColor(sourcePalette[color], rgb);
		const bool inTable = !memcmp(rgb, _mixSource + color * 3, 3);
		matched += inTable;

		for (int region = 0; region < regions; ++region) {
			byte first, second;

			if (inTable) {
				first = _mixLut[region][color * 2];
				second = _mixLut[region][color * 2 + 1];
			} else {
				nearestMix(region, sourcePalette[color], first, second);
			}

			byte (*field)[2][256] = _mixField[region];
			switch (_mixPattern) {
			case kMixChecker:
				field[0][0][color] = field[1][1][color] = first;
				field[0][1][color] = field[1][0][color] = second;
				break;
			case kMixAlternate:
				field[0][0][color] = field[0][1][color] = first;
				field[1][0][color] = field[1][1][color] = second;
				break;
			case kMixStatic:
				field[0][0][color] = field[1][0][color] = first;
				field[0][1][color] = field[1][1][color] = second;
				break;
			}
		}

		memcpy(_mixPalette + color * 3, rgb, 3);
	}

	_mixTablesValid = true;
	debug("STE mix: palette mapped, %d colours from the table, %d nearest", matched, 256 - matched);
}

void AtariSteSceneRenderer::convertMixRect(const Graphics::Surface &source, AtariSurface &field0,
		AtariSurface &field1, Common::Rect rect) {
	const int width = MIN<int>(source.w, 320);
	const int height = MIN<int>(source.h, 200);
	const int xOffset = (field0.w - source.w) / 2;
	rect.clip(Common::Rect(field0.w, height));
	if (rect.isEmpty())
		return;
	rect.left &= ~15;
	rect.right = MIN<int>((rect.right + 15) & ~15, field0.w);
#ifdef ATARI_STE_GAME_ONLY
	if (atari_ste_bench_state.enabled)
		atari_ste_bench_state.pixels += rect.width() * (rect.bottom - rect.top);
#endif
	byte row0[320];
	byte row1[320];
	const int sourceLeft = MAX<int>(rect.left, xOffset);
	const int sourceRight = MIN<int>(rect.right, xOffset + width);
	const int count = sourceRight - sourceLeft;

	for (int y = rect.top; y < rect.bottom; ++y) {
		if (sourceLeft > rect.left || sourceRight < rect.right) {
			memset(row0, 0, rect.width());
			memset(row1, 0, rect.width());
		}
		if (count > 0) {
			const byte *sourceRow = (const byte *)source.getBasePtr(sourceLeft - xOffset, y);
			byte *out0 = row0 + sourceLeft - rect.left;
			byte *out1 = row1 + sourceLeft - rect.left;
			// With a split, the lines from sceneLines() on use the verb-bar tables.
			byte (*field)[2][256] = _mixField[_mixSplit && y >= _sceneLines ? 1 : 0];
			// Tables for the first pixel's (x + y) parity and for the next one's.
			const int parity = (sourceLeft + y) & 1;
			const byte *first0 = field[0][parity], *next0 = field[0][parity ^ 1];
			const byte *first1 = field[1][parity], *next1 = field[1][parity ^ 1];
			int x = 0;

			for (; x + 1 < count; x += 2) {
				out0[x] = first0[sourceRow[x]];
				out1[x] = first1[sourceRow[x]];
				out0[x + 1] = next0[sourceRow[x + 1]];
				out1[x + 1] = next1[sourceRow[x + 1]];
			}
			if (x < count) {
				out0[x] = first0[sourceRow[x]];
				out1[x] = first1[sourceRow[x]];
			}
		}
		field0.copyRectToSurface(row0, rect.width(), rect.left, y, rect.width(), 1);
		field1.copyRectToSurface(row1, rect.width(), rect.left, y, rect.width(), 1);
	}
}

void AtariSteSceneRenderer::convertMix(const Graphics::Surface &source, AtariSurface &field0,
		AtariSurface &field1, const _RGB *sourcePalette, uint16 *schedule, uint32 &scheduleGeneration,
		const Screen::DirtyRects *dirtyRects) {
	const int height = MIN<int>(source.h, 200);
	bool fullRedraw = dirtyRects == nullptr;

	// setPalette() forces a full redraw of every buffer, so the palette only
	// has to be compared then.
	if (!_mixTablesValid || (fullRedraw && !samePalette(_mixPalette, sourcePalette))) {
		buildMixTables(sourcePalette);
		fullRedraw = true;
	}

	if (scheduleGeneration != _mixGeneration) {
		memcpy(schedule, _mixWords, sizeof(_mixWords));
		scheduleGeneration = _mixGeneration;
	}

	if (!fullRedraw) {
		for (const Common::Rect &rect : *dirtyRects)
			convertMixRect(source, field0, field1, rect);
		return;
	}

	convertMixRect(source, field0, field1, Common::Rect(field0.w, height));

	byte blank[320] = {};
	for (int y = height; y < 200; ++y) {
		field0.copyRectToSurface(blank, 320, 0, y, 320, 1);
		field1.copyRectToSurface(blank, 320, 0, y, 320, 1);
	}
}
