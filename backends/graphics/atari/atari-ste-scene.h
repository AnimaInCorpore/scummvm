/* ScummVM - Graphic Adventure Engine
 *
 * Atari STE scene conversion: compose the engine's completed chunky frame
 * into one four-plane frame with a stable 16-colour palette per raster line,
 * and pack that per-line schedule into the stream atari-ste-raster.S plays.
 */

#ifndef BACKENDS_GRAPHICS_ATARI_STE_SCENE_H
#define BACKENDS_GRAPHICS_ATARI_STE_SCENE_H

#include <mint/ostruct.h>

#include "common/path.h"
#include "common/scummsys.h"
#include "graphics/surface.h"

#include "atari-ste-raster.h"
#include "atari-screen.h"

class AtariSurface;

class AtariSteSceneRenderer {
public:
	AtariSteSceneRenderer();
	~AtariSteSceneRenderer();

	// Display lines the raster covers (see atari-ste-raster.h). Lines 0 and 1
	// share one palette, so do all lines from `lines` on. Changing it
	// invalidates the current schedule.
	void setSceneLines(int lines);
	int sceneLines() const { return _sceneLines; }

	// Size in words of one packed schedule for `lines` raster lines.
	static int scheduleWords(int lines) {
		return ATARI_STE_RASTER_HEADER_WORDS + ATARI_STE_RASTER_LINE_WORDS * (lines - 2);
	}

	// dirtyRects, when given, limits conversion to the rectangles the engine
	// actually changed in this destination buffer. X extents are aligned to
	// 16 pixels for the planar converter and clipped to the source. Pass
	// nullptr to reconvert the whole frame.
	//
	// schedule receives the packed raster stream of the current per-line
	// palettes whenever scheduleGeneration, which the caller keeps per
	// destination buffer, is behind the renderer's; it is updated then.
	void convert(const Graphics::Surface &source, AtariSurface &destination,
				 const _RGB *sourcePalette, uint16 *schedule, uint32 &scheduleGeneration,
				 const Screen::DirtyRects *dirtyRects = nullptr);

	// Temporal colour mixing. A table from
	// devtools/atari-ste/tools/monkey-flicker-compare.mjs maps every source
	// colour to a pair of entries of a fixed palette (or one entry from each
	// of two palettes). convertMix() writes both fields of a buffer; the VBL
	// hook alternates them. A table may carry a second set of palettes and
	// pairs for the lines from sceneLines() on (the verb bar), which Timer B
	// installs there.
	enum MixPattern {
		kMixChecker,	// pair counter-phased on (x + y) parity
		kMixAlternate,	// first colour in field 0, second in field 1
		kMixStatic		// (x + y) parity picks the colour in both fields
	};
	// False, with a warning, when the table cannot be used. Two-palette
	// tables always alternate, whatever the pattern.
	bool loadMix(const Common::Path &path, MixPattern pattern);
	bool mixEnabled() const { return _mixEnabled; }
	bool mixSplit() const { return _mixSplit; }
	bool mixDual() const { return _mixDual; }
	// Tables can be exchanged while the game runs (one per room); the caller
	// then has to redraw every buffer in full.
	//
	// Same dirty-rectangle and schedule contract as convert(). The schedule
	// receives 64 words: field 0's room and verb-bar palettes, then field 1's.
	void convertMix(const Graphics::Surface &source, AtariSurface &field0, AtariSurface &field1,
					const _RGB *sourcePalette, uint16 *schedule, uint32 &scheduleGeneration,
					const Screen::DirtyRects *dirtyRects = nullptr);

private:
	void buildMixTables(const _RGB *sourcePalette);
	// region: 0 for the room, 1 for the verb bar.
	void nearestMix(int region, const _RGB &color, byte &first, byte &second) const;
	void convertMixRect(const Graphics::Surface &source, AtariSurface &field0, AtariSurface &field1,
						Common::Rect rect);

	void choosePalette(const Graphics::Surface &source, int firstRow, int rowCount, int width,
					  const uint16 *sourceColors, uint16 *linePalette);
	void quantizeRow(const byte *sourceRow, byte *destinationRow, int width,
					 const uint16 *sourceColors, const uint16 *linePalette, int row);
	void packSchedule(uint16 *schedule) const;
	void convertRect(const Graphics::Surface &source, AtariSurface &destination,
					 const uint16 *sourceColors, Common::Rect rect);

	int _sceneLines;
	uint32 _scheduleGeneration;

	uint16 _previousPalettes[200 * 16];

	// The per-scanline palette schedule is selected once, on a full redraw, and
	// then held fixed while the room is on screen, so the background is
	// quantized once instead of once per frame. _scheduleMap caches each
	// scanline's source-colour to register mapping for as long as the schedule
	// stands; _scheduleMapValid says which of its entries have been filled in.
	uint16 _schedulePalettes[200 * 16];
	byte _scheduleMap[200][256];
	uint8 _scheduleMapValid[200][256 / 8];
	bool _hasSchedule;

	// Per-scanline scratch. choosePalette() leaves _histogram zeroed and
	// _steChosen cleared again on exit, so neither has to be wiped per row.
	uint16 _histogram[256];
	// Bitmaps over the 12-bit STE colour space (4096 entries).
	uint8 _steInSource[4096 / 8];
	uint8 _steChosen[4096 / 8];

	bool _hasPreviousPalette;

	// Colour mixing state (loadMix).
	bool _mixEnabled = false;
	bool _mixDual = false;
	bool _mixSplit = false;
	MixPattern _mixPattern = kMixChecker;
	// Hardware palette words in schedule order: palette (field * 2 + region)
	// starts at word (field * 2 + region) * 16; region 1 is the verb bar.
	uint16 _mixWords[64] = {};
	// Table slots per region and source colour, and the source palette the
	// table was made for.
	byte _mixLut[2][256 * 2] = {};
	byte _mixSource[256 * 3] = {};
	// _mixField[region][field][(x + y) & 1][source colour] is the register.
	byte _mixField[2][2][2][256] = {};
	// Source palette the field tables were built from.
	byte _mixPalette[256 * 3] = {};
	bool _mixTablesValid = false;
	// Counts loaded tables, so a buffer knows its palette words are stale.
	uint32 _mixGeneration = 0;
};

#endif
