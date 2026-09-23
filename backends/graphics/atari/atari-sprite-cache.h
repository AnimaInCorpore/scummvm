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

#ifndef BACKENDS_GRAPHICS_ATARI_SPRITE_CACHE_H
#define BACKENDS_GRAPHICS_ATARI_SPRITE_CACHE_H

#include "common/array.h"
#include "common/noncopyable.h"
#include "common/scummsys.h"

namespace Graphics {
class Surface;
struct IndexedSprite;
}

class AtariSurface;

/**
 * A small LRU cache of indexed graphics converted to Falcon-native 8-plane
 * data.
 *
 * The cache owns only the immutable source planes. The per-draw visibility
 * mask stays with SCI because it depends on the priority and control maps.
 */
class AtariSpriteCache : public Common::NonCopyable {
public:
	AtariSpriteCache() = default;
	~AtariSpriteCache();

	void setBudget(uint32 bytes);
	void clear();

	bool draw(const Graphics::IndexedSprite &sprite, AtariSurface &destination);

private:
	struct Entry;

	Entry *find(const Graphics::IndexedSprite &sprite);
	Entry *get(const Graphics::IndexedSprite &sprite);
	void evictToFit(uint32 extraBytes);
	bool expandKeepMask(const Graphics::IndexedSprite &sprite, int paddedWidth);
	bool matchesSource(const Entry *entry, const Graphics::IndexedSprite &sprite) const;

	Common::Array<Entry *> _entries;
	AtariSurface *_expandedKeepMask = nullptr;
	uint32 _budget = 0;
	uint32 _size = 0;
	uint32 _age = 0;
};

#endif // BACKENDS_GRAPHICS_ATARI_SPRITE_CACHE_H
