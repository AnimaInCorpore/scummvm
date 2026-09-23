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
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define FORCE_TEXT_CONSOLE

#include "atari-cursor.h"

#include "atari-supervidel.h"
#include "atari-surface.h"
//#include "common/debug.h"

bool Cursor::_globalSurfaceChanged;

byte Cursor::_palette[256*3];

const byte *Cursor::_buf;
int Cursor::_width;
int Cursor::_height;
int Cursor::_hotspotX;
int Cursor::_hotspotY;
uint32 Cursor::_keycolor;

Graphics::Surface Cursor::_surface;
Graphics::Surface Cursor::_surfaceMask;

Cursor::~Cursor() {
	_savedBackground.free();
	_steBackground.free();
	// beware, called multiple times (they have to be destroyed before
	// AtariSurfaceDeinit() is called)
	if (_surface.getPixels())
		_surface.free();
	if (_surface.getPixels())
		_surfaceMask.free();
}

void Cursor::update() {
	if (!_buf) {
		_outOfScreen = true;
		_savedRect = _offsettedDstRect = _alignedDstRect = Common::Rect();
		return;
	}

	if (!_visible || (!_positionChanged && !_surfaceChanged))
		return;

	_srcRect = Common::Rect(_width, _height);

	_dstRect = Common::Rect(
		_x - _hotspotX,	// left
		_y - _hotspotY,	// top
		_x - _hotspotX + _width,	// right
		_y - _hotspotY + _height);	// bottom

	_outOfScreen = !_boundingSurf->clip(_srcRect, _dstRect);

	if (!_outOfScreen) {
		assert(_srcRect.width() == _dstRect.width());
		assert(_srcRect.height() == _dstRect.height());

		const int dstBitsPerPixel = _screenSurf->getBitsPerPixel();
		const int xOffset         = (_screenSurf->w - _boundingSurf->w) / 2;

		// non-direct rendering never uses 4bpp but maybe in the future ...
		_savedRect = AtariSurface::alignRect(
			_dstRect.left * dstBitsPerPixel / 8,	// fake 4bpp by 8bpp's x/2
			_dstRect.top,
			_dstRect.right * dstBitsPerPixel / 8,	// fake 4bpp by 8bpp's width/2
			_dstRect.bottom);

		// these are used only in flushBackground() for comparison with rects
		// passed by Screen::addDirtyRect (shifted by the same offset)
		_offsettedDstRect = Common::Rect(
			_dstRect.left + xOffset,
			_dstRect.top,
			_dstRect.right + xOffset,
			_dstRect.bottom);
		_alignedDstRect = AtariSurface::alignRect(_offsettedDstRect);
	}
}

void Cursor::updatePosition(int deltaX, int deltaY) {
	//debug("Cursor::updatePosition: %d, %d", deltaX, deltaX);

	if (deltaX == 0 && deltaY == 0)
		return;

	_x += deltaX;
	_y += deltaY;

	if (_x < 0)
		_x = 0;
	else if (_x >= _boundingSurf->w)
		_x = _boundingSurf->w - 1;

	if (_y < 0)
		_y = 0;
	else if (_y >= _boundingSurf->h)
		_y = _boundingSurf->h - 1;

	_positionChanged = true;
}

/* static */ void Cursor::setSurface(const void *buf, int w, int h, int hotspotX, int hotspotY, uint32 keycolor) {
	if (w == 0 || h == 0 || buf == nullptr) {
		_buf = nullptr;
		return;
	}

	_buf = (const byte *)buf;
	_width = w;
	_height = h;
	_hotspotX = hotspotX;
	_hotspotY = hotspotY;
	_keycolor = keycolor;

	_globalSurfaceChanged = true;
}

/* static */ void Cursor::setPalette(const byte *colors, uint start, uint num) {
	memcpy(&_palette[start * 3], colors, num * 3);

	_globalSurfaceChanged = true;
}

/* static */ void Cursor::convertSurfaceTo(const Graphics::PixelFormat &format) {
	static int rShift, gShift, bShift;
	static int rMask, gMask, bMask;

	const int cursorWidth = g_hasSuperVidel ? _width : ((_width + 15) & (-16));
	const int cursorHeight = _height;
	const bool isCLUT8 = format.isCLUT8();

	if (_surface.w != cursorWidth || _surface.h != cursorHeight || _surface.format != format) {
		if (!isCLUT8 && _surface.format != format) {
			rShift = format.rLoss - format.rShift;
			gShift = format.gLoss - format.gShift;
			bShift = format.bLoss - format.bShift;

			rMask = format.rMax() << format.rShift;
			gMask = format.gMax() << format.gShift;
			bMask = format.bMax() << format.bShift;
		}

		// always 8-bit as this is both 8-bit src and 4-bit dst for C2P
		_surface.create(cursorWidth, cursorHeight, format);
		assert(_surface.pitch == _surface.w);
		// always 8-bit or 1-bit
		_surfaceMask.create(g_hasSuperVidel ? _surface.w : _surface.w / 8, _surface.h, PIXELFORMAT_CLUT8);
		_surfaceMask.w = _surface.w;
	}

	const byte *src = _buf;
	byte *dst = (byte *)_surface.getPixels();
	byte *dstMask = (byte *)_surfaceMask.getPixels();
	uint16 *dstMask16 = (uint16 *)_surfaceMask.getPixels();
	const int dstPadding = _surface.w - _width;

	uint16 mask16 = 0xffff;
	uint16 invertedBit = 0x7fff;

	for (int j = 0; j < _height; ++j) {
		for (int i = 0; i < _width; ++i) {
			const uint32 color = *src++;

			if (color != _keycolor) {
				if (!isCLUT8) {
					// Convert CLUT8 to RGB332/RGB121 palette
					*dst++ =  ((_palette[color*3 + 0] >> rShift) & rMask)
							 | ((_palette[color*3 + 1] >> gShift) & gMask)
							 | ((_palette[color*3 + 2] >> bShift) & bMask);
				} else {
					*dst++ = color;
				}

				if (g_hasSuperVidel)
					*dstMask++ = 0xff;
				else
					mask16 &= invertedBit;
			} else {
				*dst++ = 0x00;

				if (g_hasSuperVidel)
					*dstMask++ = 0x00;
			}

			if (!g_hasSuperVidel && invertedBit == 0xfffe) {
				*dstMask16++ = mask16;
				mask16 = 0xffff;
			}

			// ror.w #1,invertedBit
			invertedBit = (invertedBit >> 1) | (invertedBit << (sizeof (invertedBit) * 8 - 1));
		}

		if (dstPadding) {
			assert(!g_hasSuperVidel);

			// this is at most 15 pixels
			memset(dst, 0x00, dstPadding);
			dst += dstPadding;

			*dstMask16++ = mask16;
			mask16 = 0xffff;
			invertedBit = 0x7fff;
		}
	}
}

Common::Rect Cursor::flushBackground(const Common::Rect &alignedRect, const Common::Rect &writtenRect,
									 bool directRendering) {
	if (_savedRect.isEmpty())
		return _savedRect;

	// Only the pixels actually written replace the cursor. Direct rendering
	// merges partial 16-pixel blocks, so the aligned rect may cover cursor
	// pixels that stay on screen.
	if (!writtenRect.isEmpty() && writtenRect.contains(_offsettedDstRect)) {
		// better would be _visibilityChanged but update() ignores it
		_positionChanged = true;

		_savedRect = Common::Rect();
	} else if (alignedRect.isEmpty() || alignedRect.intersects(_alignedDstRect)) {
		// better would be _visibilityChanged but update() ignores it
		_positionChanged = true;

		if (directRendering)
			restoreBackground();
		else
			return _alignedDstRect;
	}

	return Common::Rect();
}

void Cursor::saveBackground() {
	if (_savedRect.isEmpty())
		return;

	// as this is used only for direct rendering, we don't need to worry about offsettedSurf
	// having different dimensions than the source surface
	const Graphics::Surface &dstSurface = *_screenSurf;

	//debug("Cursor::saveBackground: %d %d %d %d", _savedRect.left, _savedRect.top, _savedRect.width(), _savedRect.height());

	// save native bitplanes or pixels, so it must be a Graphics::Surface to copy from
	if (_savedBackground.w != _savedRect.width()
		|| _savedBackground.h != _savedRect.height()
		|| _savedBackground.format != dstSurface.format) {
		_savedBackground.create(_savedRect.width(), _savedRect.height(), dstSurface.format);
	}

	_savedBackground.copyRectToSurface(dstSurface, 0, 0, _savedRect);
}

void Cursor::draw() {
	AtariSurface &dstSurface  = *_screenSurf;
	const int dstBitsPerPixel = dstSurface.getBitsPerPixel();

	//debug("Cursor::draw: %d %d %d %d", _dstRect.left, _dstRect.top, _dstRect.width(), _dstRect.height());

	if (_globalSurfaceChanged) {
		convertSurfaceTo(dstSurface.format);

		if (!g_hasSuperVidel) {
			// C2P in-place
			AtariSurface surf;
			surf.w = _surface.w;
			surf.h = _surface.h;
			surf.pitch = _surface.pitch * dstBitsPerPixel / 8;	// 4bpp is not byte per pixel anymore
			surf.setPixels(_surface.getPixels());
			surf.format = _surface.format;

			surf.copyRectToSurface(
				_surface,
				0, 0,
				Common::Rect(_surface.w, _surface.h));
		}

		_globalSurfaceChanged = false;
	}

	dstSurface.drawMaskedSprite(
		_surface, _surfaceMask, *_boundingSurf,
		_dstRect.left, _dstRect.top,
		_srcRect);

	_visibilityChanged = _positionChanged = _surfaceChanged = false;
}

Common::Rect Cursor::steUpdate(bool &changed) {
	update();

	changed = isChanged();
	_visibilityChanged = _positionChanged = _surfaceChanged = false;

	return isVisible() ? _dstRect : Common::Rect();
}

void Cursor::steDraw(Graphics::Surface &frame) {
	_steRect = isVisible() ? _dstRect : Common::Rect();

	if (_steRect.isEmpty())
		return;

	// The frame is the engine's 8-bit surface and the cursor's colours are
	// its palette indices, so they are copied as they are; the conversion
	// gives them the same pairs as every other pixel of that colour.
	if (_steBackground.w != _steRect.width()
		|| _steBackground.h != _steRect.height()
		|| _steBackground.format != frame.format) {
		_steBackground.create(_steRect.width(), _steRect.height(), frame.format);
	}

	_steBackground.copyRectToSurface(frame, 0, 0, _steRect);

	for (int y = 0; y < _steRect.height(); ++y) {
		const byte *src = _buf + (_srcRect.top + y) * _width + _srcRect.left;
		byte *dst = (byte *)frame.getBasePtr(_steRect.left, _steRect.top + y);

		for (int x = 0; x < _steRect.width(); ++x) {
			if (src[x] != _keycolor)
				dst[x] = src[x];
		}
	}
}

void Cursor::steRestore(Graphics::Surface &frame) {
	if (_steRect.isEmpty())
		return;

	frame.copyRectToSurface(
		_steBackground,
		_steRect.left, _steRect.top,
		Common::Rect(_steRect.width(), _steRect.height()));

	_steRect = Common::Rect();
}

void Cursor::restoreBackground() {
	if (_savedRect.isEmpty())
		return;

	assert(_savedBackground.getPixels());

	//debug("Cursor::restoreBackground: %d %d %d %d", _savedRect.left, _savedRect.top, _savedRect.width(), _savedRect.height());

	// as this is used only for direct rendering, we don't need to worry about offsettedSurf
	// having different dimensions than the source surface
	Graphics::Surface &dstSurface = *_screenSurf->surfacePtr();

	// restore native bitplanes or pixels, so it must be a Graphics::Surface to copy to
	dstSurface.copyRectToSurface(
		_savedBackground,
		_savedRect.left, _savedRect.top,
		Common::Rect(_savedBackground.w, _savedBackground.h));

	_savedRect = Common::Rect();
}
