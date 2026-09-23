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

#include "backends/graphics/atari/atari-blitter.h"

#include <mint/osbind.h>
#include <mint/trap14.h>

#include "graphics/surface.h"

namespace {

constexpr uintptr kBlitterBase = 0xffff8a00;
constexpr byte kBlitterBusy = 0x80;
constexpr byte kHopSource = 2;
constexpr byte kOpSource = 3;
constexpr byte kOpSourceAndDestination = 1;
constexpr byte kOpSourceOrDestination = 7;

struct BlitterRegisters {
	volatile uint16 halftone[16];
	volatile int16 sourceXIncrement;
	volatile int16 sourceYIncrement;
	volatile uint32 sourceAddress;
	volatile uint16 endMask1;
	volatile uint16 endMask2;
	volatile uint16 endMask3;
	volatile int16 destinationXIncrement;
	volatile int16 destinationYIncrement;
	volatile uint32 destinationAddress;
	volatile uint16 wordsPerLine;
	volatile uint16 lines;
	volatile byte halftoneOperation;
	volatile byte logicalOperation;
	volatile byte control;
	volatile byte skew;
};

static volatile BlitterRegisters &blitter = *reinterpret_cast<volatile BlitterRegisters *>(kBlitterBase);
static int s_restoreMode = -1;
static bool s_enabled = false;

struct BlitRequest {
	uint32 sourceAddress;
	uint32 destinationAddress;
	int16 sourceYIncrement;
	int16 destinationYIncrement;
	uint16 wordsPerLine;
	uint16 lines;
	byte operation;
};

static BlitRequest s_request;

static int16 blitmode(int16 mode) {
	return (int16)trap_14_ww((short)64, (short)mode);
}

static void waitForIdle() {
	while (blitter.control & kBlitterBusy) {
	}
}

static long waitForIdleSuper() {
	waitForIdle();
	return 0;
}

static long executeBlitSuper() {
	waitForIdle();
	blitter.sourceXIncrement = 2;
	blitter.sourceYIncrement = s_request.sourceYIncrement;
	blitter.sourceAddress = s_request.sourceAddress;
	blitter.endMask1 = 0xffff;
	blitter.endMask2 = 0xffff;
	blitter.endMask3 = 0xffff;
	blitter.destinationXIncrement = 2;
	blitter.destinationYIncrement = s_request.destinationYIncrement;
	blitter.destinationAddress = s_request.destinationAddress;
	blitter.wordsPerLine = s_request.wordsPerLine;
	blitter.lines = s_request.lines;
	blitter.halftoneOperation = kHopSource;
	blitter.logicalOperation = s_request.operation;
	blitter.skew = 0;
	blitter.control = kBlitterBusy;
	waitForIdle();
	return 1;
}

static bool copyOperation8(const Graphics::Surface &source, const Common::Rect &sourceRect,
						Graphics::Surface &destination, int destX, int destY, byte operation) {
	if (!s_enabled || sourceRect.isEmpty() || sourceRect.left & 15 || sourceRect.width() & 15
		|| destX & 15 || sourceRect.top < 0 || sourceRect.bottom > source.h
		|| destX < 0 || destY < 0 || destX + sourceRect.width() > destination.w
		|| destY + sourceRect.height() > destination.h)
		return false;

	const int width = sourceRect.width();
	const byte *sourcePixels = static_cast<const byte *>(source.getBasePtr(sourceRect.left, sourceRect.top));
	byte *destinationPixels = static_cast<byte *>(destination.getBasePtr(destX, destY));

	// The Blitter does not apply the X increment after the final word of a
	// line, so the Y increment must include that final word advance.
	s_request.sourceAddress = (uint32)(uintptr)sourcePixels;
	s_request.destinationAddress = (uint32)(uintptr)destinationPixels;
	s_request.sourceYIncrement = source.pitch - width + 2;
	s_request.destinationYIncrement = destination.pitch - width + 2;
	s_request.wordsPerLine = width / 2;
	s_request.lines = sourceRect.height();
	s_request.operation = operation;
	return Supexec(executeBlitSuper) != 0;
}

} // End of anonymous namespace

namespace AtariBlitter {

bool init() {
	if (s_restoreMode != -1)
		return s_enabled;

	s_restoreMode = blitmode(-1);
	const int16 mode = blitmode(1);
	s_enabled = (mode & 3) == 3;
	return s_enabled;
}

void deinit() {
	if (s_restoreMode == -1)
		return;

	if (s_enabled)
		Supexec(waitForIdleSuper);
	blitmode(s_restoreMode & 1);
	s_restoreMode = -1;
	s_enabled = false;
}

bool copyOpaque8(const Graphics::Surface &source, const Common::Rect &sourceRect,
				 Graphics::Surface &destination, int destX, int destY) {
	return copyOperation8(source, sourceRect, destination, destX, destY, kOpSource);
}

bool copyMasked8(const Graphics::Surface &keepMask, const Graphics::Surface &source,
				 const Common::Rect &sourceRect, Graphics::Surface &destination,
				 int destX, int destY) {
	if (keepMask.w != sourceRect.width() || keepMask.h != sourceRect.height())
		return false;

	const Common::Rect maskRect(keepMask.w, keepMask.h);
	return copyOperation8(keepMask, maskRect, destination, destX, destY, kOpSourceAndDestination)
		&& copyOperation8(source, sourceRect, destination, destX, destY, kOpSourceOrDestination);
}

} // End of namespace AtariBlitter
