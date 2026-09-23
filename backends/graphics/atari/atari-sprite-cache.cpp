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

#include "backends/graphics/atari/atari-sprite-cache.h"

#include "backends/graphics/atari/atari-blitter.h"
#include "backends/graphics/atari/atari-surface.h"
#include "common/system.h"

struct AtariSpriteCache::Entry {
	uint64 key;
	uint64 sourceHash;
	const byte *pixels;
	byte clearKey;
	bool opaque;
	int width;
	int height;
	int paddedWidth;
	uint32 size;
	uint32 lastUsed;
	AtariSurface *surface;
	Common::Array<byte> sourceCopy;

	Entry()
		: key(0), sourceHash(0), pixels(nullptr), clearKey(0), opaque(false), width(0), height(0), paddedWidth(0),
		  size(0), lastUsed(0), surface(nullptr) {
	}

	~Entry() {
		delete surface;
	}
};

AtariSpriteCache::~AtariSpriteCache() {
	clear();
}

void AtariSpriteCache::setBudget(uint32 bytes) {
	_budget = bytes;
	evictToFit(0);
}

void AtariSpriteCache::clear() {
	for (Entry *entry : _entries)
		delete entry;
	_entries.clear();
	_size = 0;
	_age = 0;

	delete _expandedKeepMask;
	_expandedKeepMask = nullptr;
}

AtariSpriteCache::Entry *AtariSpriteCache::find(const Graphics::IndexedSprite &sprite) {
	for (Entry *entry : _entries) {
		if (entry->key == sprite.cacheKey && entry->sourceHash == sprite.sourceHash
			&& entry->clearKey == sprite.clearKey && entry->opaque == !sprite.mask
			&& entry->width == sprite.sourceWidth && entry->height == sprite.sourceHeight
			&& matchesSource(entry, sprite)) {
			entry->lastUsed = ++_age;
			return entry;
		}
	}

	return nullptr;
}

bool AtariSpriteCache::matchesSource(const Entry *entry, const Graphics::IndexedSprite &sprite) const {
	if (!sprite.sourceHash)
		return entry->pixels == sprite.pixels;

	const int sourcePitch = sprite.sourcePitch ? sprite.sourcePitch : sprite.sourceWidth;
	if (entry->sourceCopy.size() != (uint)(sprite.sourceWidth * sprite.sourceHeight))
		return false;

	for (int y = 0; y < sprite.sourceHeight; ++y) {
		if (memcmp(entry->sourceCopy.data() + y * sprite.sourceWidth,
				sprite.pixels + y * sourcePitch, sprite.sourceWidth) != 0)
			return false;
	}

	return true;
}

void AtariSpriteCache::evictToFit(uint32 extraBytes) {
	while (!_entries.empty() && _size + extraBytes > _budget) {
		uint oldestIndex = 0;
		for (uint i = 1; i < _entries.size(); ++i) {
			if (_entries[i]->lastUsed < _entries[oldestIndex]->lastUsed)
				oldestIndex = i;
		}

		Entry *entry = _entries.remove_at(oldestIndex);
		_size -= entry->size;
		delete entry;
	}
}

AtariSpriteCache::Entry *AtariSpriteCache::get(const Graphics::IndexedSprite &sprite) {
	if (Entry *entry = find(sprite))
		return entry;

	const int paddedWidth = (sprite.sourceWidth + 15) & -16;
	const int sourcePitch = sprite.sourcePitch ? sprite.sourcePitch : sprite.sourceWidth;
	if (!_budget || paddedWidth <= 0 || sprite.sourceHeight <= 0 || sourcePitch < sprite.sourceWidth)
		return nullptr;
	const uint32 planarSize = paddedWidth * sprite.sourceHeight;
	const uint32 sourceSize = sprite.sourceHash ? sprite.sourceWidth * sprite.sourceHeight : 0;
	const uint32 size = planarSize + sourceSize;
	if (size > _budget)
		return nullptr;

	evictToFit(size);
	Entry *entry = new Entry;
	entry->key = sprite.cacheKey;
	entry->sourceHash = sprite.sourceHash;
	entry->pixels = sprite.pixels;
	entry->clearKey = sprite.clearKey;
	entry->opaque = !sprite.mask;
	entry->width = sprite.sourceWidth;
	entry->height = sprite.sourceHeight;
	entry->paddedWidth = paddedWidth;
	entry->size = size;
	entry->lastUsed = ++_age;
	entry->surface = new AtariSurface(paddedWidth, sprite.sourceHeight, PIXELFORMAT_CLUT8);

	Common::Array<byte> chunky;
	chunky.resize(planarSize);
	memset(chunky.data(), 0, planarSize);
	if (sprite.sourceHash)
		entry->sourceCopy.resize(sourceSize);
	for (int y = 0; y < sprite.sourceHeight; ++y) {
		const byte *source = sprite.pixels + y * sourcePitch;
		byte *destination = chunky.data() + y * paddedWidth;
		if (sprite.sourceHash)
			memcpy(entry->sourceCopy.data() + y * sprite.sourceWidth, source, sprite.sourceWidth);
		for (int x = 0; x < sprite.sourceWidth; ++x)
			if (entry->opaque || source[x] != sprite.clearKey)
				destination[x] = source[x];
	}
	entry->surface->copyRectToSurface(chunky.data(), paddedWidth, 0, 0, paddedWidth, sprite.sourceHeight);

	_entries.push_back(entry);
	_size += size;
	return entry;
}

bool AtariSpriteCache::expandKeepMask(const Graphics::IndexedSprite &sprite, int paddedWidth) {
	if (!_expandedKeepMask || _expandedKeepMask->w != paddedWidth
		|| _expandedKeepMask->h != sprite.sourceHeight) {
		delete _expandedKeepMask;
		_expandedKeepMask = new AtariSurface(paddedWidth, sprite.sourceHeight, PIXELFORMAT_CLUT8);
	}

	for (int y = 0; y < sprite.sourceHeight; ++y) {
		const byte *source = sprite.mask + y * sprite.maskPitch;
		byte *destination = static_cast<byte *>(_expandedKeepMask->getBasePtr(0, y));
		for (int x = 0; x < paddedWidth; x += 16) {
			const byte left = source[x / 8];
			const byte right = source[x / 8 + 1];
			for (int plane = 0; plane < 8; ++plane) {
				destination[x + plane * 2] = left;
				destination[x + plane * 2 + 1] = right;
			}
		}
	}

	return true;
}

bool AtariSpriteCache::draw(const Graphics::IndexedSprite &sprite, AtariSurface &destination) {
	if (!sprite.pixels || sprite.sourceWidth <= 0 || sprite.sourceHeight <= 0
		|| sprite.sourceX != 0 || sprite.sourceY != 0 || sprite.width != sprite.sourceWidth
		|| sprite.height != sprite.sourceHeight || (sprite.mask && sprite.maskPitch != ((sprite.sourceWidth + 15) & -16) / 8)
		|| (!sprite.mask && sprite.maskPitch)
		|| sprite.destX < 0 || sprite.destY < 0
		|| sprite.destX + sprite.width > destination.w || sprite.destY + sprite.height > destination.h
		|| destination.getBitsPerPixel() != 8)
		return false;

	const int paddedWidth = (sprite.sourceWidth + 15) & -16;
	if (!sprite.mask && (sprite.destX & 15 || sprite.destX + paddedWidth > destination.w))
		return false;

	Entry *entry = get(sprite);
	if (!entry)
		return false;

	const Common::Rect sourceRect(entry->paddedWidth, sprite.sourceHeight);
	if (!sprite.mask)
		return AtariBlitter::copyOpaque8(*entry->surface, sourceRect,
			*destination.surfacePtr(), sprite.destX, sprite.destY);

	Graphics::Surface mask;
	mask.init(entry->paddedWidth, sprite.sourceHeight, sprite.maskPitch,
		const_cast<byte *>(sprite.mask), PIXELFORMAT_CLUT8);

	if (sprite.destX % 16 == 0 && sprite.destX + entry->paddedWidth <= destination.w
		&& expandKeepMask(sprite, entry->paddedWidth)
		&& AtariBlitter::copyMasked8(*_expandedKeepMask, *entry->surface, sourceRect,
			*destination.surfacePtr(), sprite.destX, sprite.destY))
		return true;

	destination.drawMaskedSprite(*entry->surface, mask, destination,
		sprite.destX, sprite.destY, sourceRect);
	return true;
}
