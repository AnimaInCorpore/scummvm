/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef BACKENDS_GRAPHICS_ATARI_BLITTER_H
#define BACKENDS_GRAPHICS_ATARI_BLITTER_H

#include "common/rect.h"

namespace Graphics {
class Surface;
}

namespace AtariBlitter {

bool init();
void deinit();

/**
 * Copy an aligned opaque rectangle between 8-plane Atari surfaces.
 *
 * The native 8bpl layout is contiguous for an aligned rectangle, so one
 * source-copy operation covers all eight planes. The call waits for
 * completion before returning.
 */
bool copyOpaque8(const Graphics::Surface &source, const Common::Rect &sourceRect,
				 Graphics::Surface &destination, int destX, int destY);

/**
 * Apply an 8-plane keep-mask and then OR an 8-plane source into the
 * destination. Both source rectangles must be identical, aligned 8bpl data.
 */
bool copyMasked8(const Graphics::Surface &keepMask, const Graphics::Surface &source,
				 const Common::Rect &sourceRect, Graphics::Surface &destination,
				 int destX, int destY);

} // End of namespace AtariBlitter

#endif // BACKENDS_GRAPHICS_ATARI_BLITTER_H
