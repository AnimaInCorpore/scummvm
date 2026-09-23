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

#define FORBIDDEN_SYMBOL_EXCEPTION_FILE // atari-graphics.h's unordered_set
#define FORCE_TEXT_CONSOLE

#include "atari-graphics.h"

#include <mint/cookie.h>
#include <mint/falcon.h>
#include <mint/sysvars.h>

#include "backends/keymapper/action.h"
#include "backends/keymapper/keymap.h"
#include "backends/platform/atari/osystem_atari.h"
#include "common/config-manager.h"
#include "common/debug.h"
#include "common/textconsole.h"
#include "common/translation.h"
#include "gui/ThemeEngine.h"

#include "atari-surface.h"
#include "atari-ste-raster.h"
#include "atari-ste-scene.h"
#include "backends/platform/atari/ste-benchmark.h"
#ifdef ATARI_DSP_C2P
#include "backends/platform/atari/atari-dsp-c2p.h"
#endif

#define SCREEN_ACTIVE

static void shrinkVidelVisibleArea() {
	// Active VGA screen area consists of 960 half-lines, i.e. 480 raster lines.
	// In case of 320x240, the number is still 480 but data is fetched
	// only for 240 lines so it doesn't make a difference to us.
#ifdef SCREEN_ACTIVE
	if (g_hasSuperVidel) {
		const int vOffset = ((480 - 400) / 2) * 2;	// *2 because of half-lines

		// VDB = VBE = VDB + paddding/2
		*((volatile uint16*)0xFFFF82A8) = *((volatile uint16*)0xFFFF82A6) = *((volatile uint16*)0xFFFF82A8) + vOffset;
		// VDE = VBB = VDE - padding/2
		*((volatile uint16*)0xFFFF82AA) = *((volatile uint16*)0xFFFF82A4) = *((volatile uint16*)0xFFFF82AA) - vOffset;
	} else {
		// 31500/60.1 = 524 raster lines
		// vft = 524 * 2 + 1 = 1049 half-lines
		// 480 visible lines = 960 half-lines
		// 1049 - 960 = 89 half-lines reserved for borders
		// we want 400 visible lines = 800 half-lines
		// vft = 800 + 89 = 889 half-lines in total ~ 70.1 Hz vertical frequency
		int16 vft = *((volatile int16*)0xFFFF82A2);
		int16 vss = *((volatile int16*)0xFFFF82AC);	// vss = vft - vss_sync
		vss -= vft;	// -vss_sync
		*((volatile int16*)0xFFFF82A2) = 889;
		*((volatile int16*)0xFFFF82AC) = 889 + vss;
	}
#endif
}

static bool s_tt;
static bool s_ste;
static int s_shakeXOffset;
static int s_shakeYOffset;
static int s_aspectRatioCorrectionYOffset;
static bool s_shrinkVidelVisibleArea;
static bool s_setScreenOffsets;
static AtariSurface *s_screenSurf;
static Screen *s_steScreens[3] = {};
// The manager that has a mixing table loaded, for atari_ste_scene_room().
static AtariGraphicsManager *s_steGraphicsManager;

static Screen *findSteScreen(AtariSurface *surface) {
	for (Screen *screen : s_steScreens) {
		if (screen && screen->surf.get() == surface)
			return screen;
	}
	return nullptr;
}

static void VblHandler() {
	if (s_ste) {
		// The STE presents its screens from the raster's own VBL vector hook
		// (atari-ste-raster.S), which also runs while the TOS VBL queue is
		// locked; nothing of the Falcon/TT handling below applies.
		s_screenSurf = nullptr;
		s_setScreenOffsets = false;
		s_shrinkVidelVisibleArea = false;
		return;
	}

	// for easier querying
	static AtariSurface *surf;

	if (s_screenSurf)
		surf = s_screenSurf;

	if (s_screenSurf || s_setScreenOffsets) {
#ifdef SCREEN_ACTIVE
		const int baseY = MAX_V_SHAKE + s_shakeYOffset + s_aspectRatioCorrectionYOffset;
		uintptr p = (unsigned long)surf->getBasePtr(0, baseY);

		if (!s_tt) {
			const int bitsPerPixel = (surf->format == PIXELFORMAT_RGB121 ? 4 : 8);

			int shakeXOffset = -s_shakeXOffset;

			if (shakeXOffset >= 0) {
				p += MAX_HZ_SHAKE;
				*((volatile char *)0xFFFF8265) = shakeXOffset;
			} else {
				*((volatile char *)0xFFFF8265) = MAX_HZ_SHAKE + shakeXOffset;
			}

			// subtract 4 or 8 words if scrolling
			*((volatile short *)0xFFFF820E) = shakeXOffset == 0
			   ? (2 * MAX_HZ_SHAKE * bitsPerPixel / 8) / 2
			   : (2 * MAX_HZ_SHAKE * bitsPerPixel / 8) / 2 - bitsPerPixel;
		}

		union { byte c[4]; uintptr p; } sptr;
		sptr.p = p;

		*((volatile byte *)0xFFFF8201) = sptr.c[1];
		*((volatile byte *)0xFFFF8203) = sptr.c[2];
		*((volatile byte *)0xFFFF820D) = sptr.c[3];
#endif
		s_screenSurf = nullptr;
		s_setScreenOffsets = false;
	}

	if (s_shrinkVidelVisibleArea) {
		if (!s_tt)
			shrinkVidelVisibleArea();
		s_shrinkVidelVisibleArea = false;
	}
}

static uint32 InstallVblHandler() {
	uint32 installed = 0;
	*vblsem = 0;  // lock vbl

	for (int i = 0; i < *nvbls; ++i) {
		if (!(*_vblqueue)[i]) {
			(*_vblqueue)[i] = VblHandler;
			installed = 1;
			break;
		}
	}

	*vblsem = 1;  // unlock vbl
	return installed;
}

static uint32 UninstallVblHandler() {
	uint32 uninstalled = 0;
	*vblsem = 0;  // lock vbl

	for (int i = 0; i < *nvbls; ++i) {
		if ((*_vblqueue)[i] == VblHandler) {
			(*_vblqueue)[i] = NULL;
			uninstalled = 1;
			break;
		}
	}

	*vblsem = 1;  // unlock vbl
	return uninstalled;
}

static int  s_oldRez = -1;
static int  s_oldMode = -1;
static void *s_oldPhysbase = nullptr;
static Palette s_oldPalette;

void AtariGraphicsShutdown() {
#ifdef ATARI_DSP_C2P
	// The DMA must not write the screens once they are gone.
	AtariDspC2p::shutdown();
#endif
	if (s_ste)
		Supexec(atari_ste_raster_uninstall);
	Supexec(UninstallVblHandler);

	AtariSurfaceDeinit();

#ifdef SCREEN_ACTIVE
	if (s_oldRez != -1) {
		Setscreen(SCR_NOCHANGE, s_oldPhysbase, s_oldRez);

		EsetPalette(0, s_oldPalette.entries, s_oldPalette.tt);

		s_oldRez = -1;
	} else if (s_oldMode != -1) {
		static _RGB black[256];
		VsetRGB(0, 256, black);

		VsetScreen(SCR_NOCHANGE, s_oldPhysbase, SCR_NOCHANGE, SCR_NOCHANGE);

		if (g_hasSuperVidel) {
			// SuperVidel XBIOS does not restore those (unlike TOS/EmuTOS)
			long ssp = Super(SUP_SET);
			//*((volatile char *)0xFFFF8265) = 0;
			*((volatile short *)0xFFFF820E) = 0;
			Super(ssp);

			VsetMode(SVEXT | SVEXT_BASERES(0) | COL80 | BPS8C);	// resync to proper 640x480
		}
		VsetMode(s_oldMode);

		VsetRGB(0, s_oldPalette.entries, s_oldPalette.falcon);

		s_oldMode = -1;
	}
#endif
}

AtariGraphicsManager::AtariGraphicsManager(OSystem_Atari *system)
	: _system(system), _pendingScreenChanges(this) {
	debug("AtariGraphicsManager()");

	enum {
		VDO_NO_ATARI_HW = 0xffff,
		VDO_ST = 0,
		VDO_STE,
		VDO_TT,
		VDO_FALCON,
		VDO_MILAN
	};

	long vdo = VDO_NO_ATARI_HW<<16;
	Getcookie(C__VDO, &vdo);
	vdo >>= 16;

	_tt = (vdo == VDO_TT);
	_ste = (vdo == VDO_STE);
	s_tt = _tt;
	s_ste = _ste;

	if (!_tt && !_ste)
		_vgaMonitor = VgetMonitor() == MON_VGA;

	// no BDF scaling please
	ConfMan.registerDefault("gui_disable_fixed_font_scaling", true);

#ifndef ATARI_STE_GAME_ONLY
	// make the standard GUI renderer default (!DISABLE_FANCY_THEMES implies anti-aliased rendering in ThemeEngine.cpp)
	// (and without DISABLE_FANCY_THEMES we can't use 640x480 themes)
	const char *standardThemeEngineName = GUI::ThemeEngine::findModeConfigName(GUI::ThemeEngine::kGfxStandard);
	if (!ConfMan.hasKey("gui_renderer"))
		ConfMan.set("gui_renderer", standardThemeEngineName);
#endif

	// make the built-in theme default to avoid long loading times
	if (!ConfMan.hasKey("gui_theme"))
		ConfMan.set("gui_theme", "builtin");

#ifndef DISABLE_FANCY_THEMES
	// make "themes" the default theme path
	if (!ConfMan.hasKey("themepath"))
		ConfMan.setPath("themepath", "themes");
#endif

	ConfMan.flushToDisk();

	// Generate RGB332/RGB121 palette for the overlay
	const Graphics::PixelFormat &format = getOverlayFormat();
#ifndef DISABLE_FANCY_THEMES
		const int overlayPaletteSize = _tt ? 16 : 256;
#else
		const int overlayPaletteSize = 16;
#endif
	for (int i = 0; i < overlayPaletteSize; i++) {
		if (_tt) {
			// Bits 15-12    Bits 11-8     Bits 7-4      Bits 3-0
			// Reserved      Red           Green         Blue
			_overlayPalette.tt[i] =  ((i >> format.rShift) & format.rMax()) << (8 + (format.rLoss - 4));
			_overlayPalette.tt[i] |= ((i >> format.gShift) & format.gMax()) << (4 + (format.gLoss - 4));
			_overlayPalette.tt[i] |= ((i >> format.bShift) & format.bMax()) << (0 + (format.bLoss - 4));
		} else {
			_overlayPalette.falcon[i].red    = ((i >> format.rShift) & format.rMax()) << format.rLoss;
			_overlayPalette.falcon[i].green |= ((i >> format.gShift) & format.gMax()) << format.gLoss;
			_overlayPalette.falcon[i].blue  |= ((i >> format.bShift) & format.bMax()) << format.bLoss;
		}
	}
	_overlayPalette.entries = overlayPaletteSize;

	if (_tt || _ste) {
		s_oldRez = Getrez();
		// EgetPalette / EsetPalette doesn't care about current resolution's number of colors
		s_oldPalette.entries = _ste ? 16 : 256;
		EgetPalette(0, s_oldPalette.entries, s_oldPalette.tt);
	} else {
		s_oldMode = VsetMode(VM_INQUIRE);
		switch (s_oldMode & NUMCOLS) {
		case BPS1:
			s_oldPalette.entries = 2;
			break;
		case BPS2:
			s_oldPalette.entries = 4;
			break;
		case BPS4:
			s_oldPalette.entries = 16;
			break;
		case BPS8:
		case BPS8C:
			s_oldPalette.entries = 256;
			break;
		default:
			s_oldPalette.entries = 0;
		}
		VgetRGB(0, s_oldPalette.entries, s_oldPalette.falcon);
	}
	s_oldPhysbase = Physbase();

	AtariSurfaceInit();

	allocateSurfaces();

	if (!Supexec(InstallVblHandler)) {
		error("VBL handler was not installed");
	}

	if (_ste)
		Supexec(atari_ste_raster_install);

#ifdef ATARI_DSP_C2P
	if (!_tt && !_ste && !g_hasSuperVidel)
		_dspDirect = AtariDspC2p::init();
#endif

	g_system->getEventManager()->getEventDispatcher()->registerObserver(this, 10, false);
}

AtariGraphicsManager::~AtariGraphicsManager() {
	debug("~AtariGraphicsManager()");

	g_system->getEventManager()->getEventDispatcher()->unregisterObserver(this);

	// this must be done here, too otherwise freeSurfaces() could release a surface
	// still accessed by the vbl handler
	if (_ste)
		Supexec(atari_ste_raster_uninstall);
	Supexec(UninstallVblHandler);

	freeSurfaces();

	AtariGraphicsShutdown();
}

bool AtariGraphicsManager::hasFeature(OSystem::Feature f) const {
	switch (f) {
	case OSystem::Feature::kFeatureAspectRatioCorrection:
		//debug("hasFeature(kFeatureAspectRatioCorrection): %d", !_tt);
		return !_tt;
	case OSystem::Feature::kFeatureCursorPalette:
		// FIXME: pretend to have cursor palette at all times, this function
		// can get (and it is) called any time, before and after showOverlay()
		// (overlay cursor uses the cross if kFeatureCursorPalette returns false
		// here too soon)
		//debug("hasFeature(kFeatureCursorPalette): %d", isOverlayVisible());
		//return isOverlayVisible();
		return true;
	default:
		return false;
	}

	// TODO: kFeatureDisplayLogFile?, kFeatureClipboardSupport, kFeatureSystemBrowserDialog
}

void AtariGraphicsManager::setFeatureState(OSystem::Feature f, bool enable) {
	if (!hasFeature(f))
		return;

	// flags must be queued in _pendingScreenChanges here

	switch (f) {
	case OSystem::Feature::kFeatureAspectRatioCorrection:
		//debug("setFeatureState(kFeatureAspectRatioCorrection): %d", enable);
		if (_aspectRatioCorrection != enable) {
			_aspectRatioCorrection = enable;

			if (_overlayState == kOverlayHidden) {
				_pendingScreenChanges.queueAspectRatioCorrection();

				if (!_pendingState.inTransaction)
					updateScreen();
			}
		}
		break;
	default:
		break;
	}
}

bool AtariGraphicsManager::getFeatureState(OSystem::Feature f) const {
	switch (f) {
	case OSystem::Feature::kFeatureAspectRatioCorrection:
		//debug("getFeatureState(kFeatureAspectRatioCorrection): %d", _aspectRatioCorrection);
		return _aspectRatioCorrection;
	case OSystem::Feature::kFeatureCursorPalette:
		//debug("getFeatureState(kFeatureCursorPalette): %d", isOverlayVisible());
		//return isOverlayVisible();
		return true;
	default:
		return false;
	}
}

bool AtariGraphicsManager::setGraphicsMode(int mode, uint flags) {
	debug("setGraphicsMode: %d, %d", mode, flags);

	_pendingState.mode = mode;

	if (!_pendingState.inTransaction)
		return endGFXTransaction() == OSystem::kTransactionSuccess;

	// this doesn't seem to be checked anywhere
	return true;
}

void AtariGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
	debug("initSize: %d, %d, %d", width, height, format ? format->bytesPerPixel : 1);

	_pendingState.width  = width;
	_pendingState.height = height;
	_pendingState.format = format ? *format : PIXELFORMAT_CLUT8;

	if (_pendingState.width == 0 || _pendingState.height == 0) {
		// special case: initSize(0,0) implies a reinit so e.g. changing graphics mode
		// from UI doesn't automatically trigger setting s_screenSurf
		_currentState.width  = _pendingState.width;
		_currentState.height = _pendingState.height;
	}

	if (!_pendingState.inTransaction)
		endGFXTransaction();
}

void AtariGraphicsManager::beginGFXTransaction() {
	debug("beginGFXTransaction");

	_pendingState = GraphicsState();
	_pendingState.inTransaction = true;
	_pendingScreenChanges.clearTransaction();
}

OSystem::TransactionError AtariGraphicsManager::endGFXTransaction() {
	debug("endGFXTransaction");

	_pendingState.inTransaction = false;
	_ignoreCursorChanges = false;

	int error = OSystem::TransactionError::kTransactionSuccess;
	bool hasPendingGraphicsMode = false;
	bool hasPendingSize = false;

	if (_pendingState.mode != kUnknownMode) {
		if (_pendingState.mode < kDirectRendering || _pendingState.mode > kTripleBuffering) {
			error |= OSystem::TransactionError::kTransactionModeSwitchFailed;
		} else if (_currentState.mode != _pendingState.mode) {
			hasPendingGraphicsMode = true;
		}
	}

	if (_pendingState.width > 0 && _pendingState.height > 0) {
		if (_pendingState.width > getMaximumScreenWidth() || _pendingState.height > getMaximumScreenHeight()) {
			error |= OSystem::TransactionError::kTransactionSizeChangeFailed;
		} else if (((hasPendingGraphicsMode && _pendingState.mode == kDirectRendering)
				|| (!hasPendingGraphicsMode && _currentState.mode == kDirectRendering))
			&& _pendingState.width % 16 != 0
			&& !g_hasSuperVidel) {
			warning("Engine surfaces not divisible by 16, aborting");
			error |= OSystem::TransactionError::kTransactionSizeChangeFailed;
		} else if (_currentState.width != _pendingState.width || _currentState.height != _pendingState.height) {
			hasPendingSize = true;
		}
	}

	if (_pendingState.format.bytesPerPixel != 0
		&& _pendingState.format != PIXELFORMAT_CLUT8)
		error |= OSystem::TransactionError::kTransactionFormatNotSupported;

	if (error != OSystem::TransactionError::kTransactionSuccess) {
		warning("endGFXTransaction failed: %02x", error);
		_pendingScreenChanges.clearTransaction();
		return static_cast<OSystem::TransactionError>(error);
	}

	if (hasPendingGraphicsMode)
		_currentState.mode = _pendingState.mode;

	if (hasPendingSize) {
		_currentState.width  = _pendingState.width;
		_currentState.height = _pendingState.height;
		_currentState.format = _pendingState.format;
	}

	if ((hasPendingGraphicsMode || hasPendingSize) && _currentState.isValid()) {
#ifdef ATARI_DSP_C2P
		waitDspScreen();
#endif
		int c2pWidth = _currentState.width;

		if (!g_hasSuperVidel) {
			// make sure that c2p width is always divisible by 16
			c2pWidth = (c2pWidth + 15) & -16;
		}

		Graphics::Surface *chunkySurface = _chunkySurface.surfacePtr();
		chunkySurface->init(c2pWidth, _currentState.height, c2pWidth,
			chunkySurface->getPixels(), _currentState.format);

		const int xOffset = (c2pWidth - _currentState.width) / 2;

		_chunkySurfaceOffsetted.init(_currentState.width, _currentState.height, c2pWidth,
			chunkySurface->getBasePtr(xOffset, 0), _currentState.format);

		_screen[kFrontBuffer]->reset(c2pWidth, _currentState.height, _chunkySurfaceOffsetted);
		if (_currentState.mode > kSingleBuffering) {
			_screen[kBackBuffer1]->reset(c2pWidth, _currentState.height, _chunkySurfaceOffsetted);
			_screen[kBackBuffer2]->reset(c2pWidth, _currentState.height, _chunkySurfaceOffsetted);
		}

		{
			Common::Event event = {};
			event.type = Common::EVENT_MOUSEMOVE;
			event.mouse = _screen[kFrontBuffer]->cursor.getPosition();
			g_system->getEventManager()->pushEvent(event);
		}

		if (hasPendingSize)
			_pendingScreenChanges.queueVideoMode();

		_pendingScreenChanges.setScreenSurface(_screen[kFrontBuffer]->surf.get());

		_palette.clear();
		// TODO: maybe we could update real start/num values
		_palette.entries = 256;
		_pendingScreenChanges.queuePalette();

		if (_overlayState == kOverlayIgnoredHide)
			_overlayState = kOverlayHidden;

		_ignoreHideOverlay = false;
		_pendingScreenChanges.queueAll();
	} else {
		// clear any queued transaction changes from feature flags (e.g. aspect ratio correction)
		_pendingScreenChanges.clearTransaction();
	}

	_pendingState = GraphicsState();

	// apply new screen changes
	updateScreen();

	return OSystem::kTransactionSuccess;
}

void AtariGraphicsManager::setPalette(const byte *colors, uint start, uint num) {
	//debug("setPalette: %d, %d", start, num);

	if (_tt) {
		uint16 *pal = &_palette.tt[start];
		for (uint i = 0; i < num; ++i) {
			// Bits 15-12    Bits 11-8     Bits 7-4      Bits 3-0
			// Reserved      Red           Green         Blue
			pal[i]  = ((colors[i * 3 + 0] >> 4) & 0x0f) << 8;
			pal[i] |= ((colors[i * 3 + 1] >> 4) & 0x0f) << 4;
			pal[i] |= ((colors[i * 3 + 2] >> 4) & 0x0f);
		}
	} else {
		_RGB *pal = &_palette.falcon[start];
		for (uint i = 0; i < num; ++i) {
			pal[i].red   = colors[i * 3 + 0];
			pal[i].green = colors[i * 3 + 1];
			pal[i].blue  = colors[i * 3 + 2];
		}
	}

	if (_ste) {
		// On the STE the palette is baked into the quantized frame, so a palette
		// change invalidates every buffer's converted pixels, not just the ones
		// the engine marked dirty.
		for (int i : { kFrontBuffer, kBackBuffer1, kBackBuffer2 }) {
			if (_screen[i])
				_screen[i]->fullRedraw = true;
		}
	}

	_pendingScreenChanges.queuePalette();
}

void AtariGraphicsManager::grabPalette(byte *colors, uint start, uint num) const {
	//debug("grabPalette: %d, %d", start, num);

	if (_tt) {
		const uint16 *pal = &_palette.tt[start];
		for (uint i = 0; i < num; ++i) {
			// Bits 15-12    Bits 11-8     Bits 7-4      Bits 3-0
			// Reserved      Red           Green         Blue
			*colors++ = ((pal[i] >> 8) & 0x0f) << 4;
			*colors++ = ((pal[i] >> 4) & 0x0f) << 4;
			*colors++ = ((pal[i]     ) & 0x0f) << 4;
		}
	} else {
		const _RGB *pal = &_palette.falcon[start];
		for (uint i = 0; i < num; ++i) {
			*colors++ = pal[i].red;
			*colors++ = pal[i].green;
			*colors++ = pal[i].blue;
		}
	}
}

void AtariGraphicsManager::copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) {
	//debug("copyRectToScreen: %d, %d, %d(%d), %d", x, y, w, pitch, h);
	_system->updateAudio();

	Graphics::Surface &dstSurface = *lockScreen();

	bool directRendering = _currentState.mode == kDirectRendering;
#ifdef ATARI_DSP_C2P
	// The DSP mode keeps a chunky source surface and converts it into the
	// displayed Atari surface asynchronously.
	directRendering &= !_dspDirect;
#endif

	addDirtyRectToScreens(
		dstSurface,
		x, y, w, h,
		directRendering);

	if (directRendering && !g_hasSuperVidel && !_ste) {
		copyRectToAtariSurface(
			*_screen[kFrontBuffer]->offsettedSurf,
			(const byte *)buf, pitch, x, y, w, h);
	} else {
		dstSurface.copyRectToSurface(buf, pitch, x, y, w, h);
	}
}

Graphics::Surface *AtariGraphicsManager::lockScreen() {
	//debug("lockScreen");

#ifdef ATARI_DSP_C2P
	if (_dspDirect && _currentState.mode == kDirectRendering)
		return &_chunkySurfaceOffsetted;
#endif
	return !_ste && _currentState.mode == kDirectRendering
		? _screen[kFrontBuffer]->offsettedSurf->surfacePtr()
		: &_chunkySurfaceOffsetted;
}

void AtariGraphicsManager::unlockScreen() {
	const Graphics::Surface &dstSurface = *lockScreen();

	//debug("unlockScreen: %d x %d", dstSurface.w, dstSurface.h);

	bool directRendering = _currentState.mode == kDirectRendering;
#ifdef ATARI_DSP_C2P
	directRendering &= !_dspDirect;
#endif
	addDirtyRectToScreens(
		dstSurface,
		0, 0, dstSurface.w, dstSurface.h,
		directRendering);
}

void AtariGraphicsManager::fillScreen(uint32 col) {
	//debug("fillScreen: %d", col);

	Graphics::Surface &dstSurface = *lockScreen();
	bool directRendering = _currentState.mode == kDirectRendering;
#ifdef ATARI_DSP_C2P
	directRendering &= !_dspDirect;
#endif

	addDirtyRectToScreens(
		dstSurface,
		0, 0, dstSurface.w, dstSurface.h,
		directRendering);

	dstSurface.fillRect(Common::Rect(dstSurface.w, dstSurface.h), col);
}

void AtariGraphicsManager::fillScreen(const Common::Rect &r, uint32 col) {
	//debug("fillScreen: %dx%d %d", r.width(), r.height(), col);

	Graphics::Surface &dstSurface = *lockScreen();
	bool directRendering = _currentState.mode == kDirectRendering;
#ifdef ATARI_DSP_C2P
	directRendering &= !_dspDirect;
#endif

	addDirtyRectToScreens(
		dstSurface,
		r.left, r.top, r.width(), r.height(),
		directRendering);

	dstSurface.fillRect(r, col);
}

void AtariGraphicsManager::updateScreen() {
	//debug("updateScreen");
	_system->updateAudio();

	// avoid falling into the debugger (screen may not not initialized yet)
	Common::setErrorHandler(nullptr);

	Screen *workScreen = nullptr;
	Graphics::Surface *srcSurface = nullptr;
	if (_overlayState == kOverlayVisible || _overlayState == kOverlayIgnoredHide) {
		workScreen = _screen[kOverlayBuffer];
		if (!isOverlayDirectRendering())
			srcSurface = _overlaySurface.surfacePtr();
	} else {
		switch (_currentState.mode) {
		case kDirectRendering:
			workScreen = _screen[kFrontBuffer];
#ifdef ATARI_DSP_C2P
			// Keep the engine's single-buffer rendering model, but use the
			// chunky surface as DSP input before it writes to the front page.
			if (_dspDirect)
				srcSurface = _chunkySurface.surfacePtr();
#endif
			break;
		case kSingleBuffering:
			workScreen = _screen[kFrontBuffer];
			srcSurface = _chunkySurface.surfacePtr();
			break;
		case kTripleBuffering:
#ifdef ATARI_DSP_C2P
			// The DSP build writes into the displayed surface as it converts.
			workScreen = _dspDirect ? _screen[kFrontBuffer] : _screen[kBackBuffer1];
#else
			workScreen = _screen[kBackBuffer1];
#endif
			srcSurface = _chunkySurface.surfacePtr();
			break;
		default:
			warning("Unknown graphics mode %d", _currentState.mode);
		}
	}

	// The STE always receives the finished chunky frame. This keeps direct
	// rendering as an engine option without letting 8-bit indices be written
	// directly into the four-plane screen surface.
	if (_ste && _overlayState == kOverlayHidden)
		srcSurface = _chunkySurface.surfacePtr();

	assert(workScreen);

	bool screenUpdated;
#ifdef ATARI_DSP_C2P
	if (!updateScreenDsp(workScreen, srcSurface, screenUpdated))
#endif
		screenUpdated = updateScreenInternal(workScreen, srcSurface);

#ifdef SCREEN_ACTIVE
	// this assume that the screen surface is not going to be used yet
	_pendingScreenChanges.applyBeforeVblLock(*workScreen);
#endif

	set_sysvar_to_short(vblsem, 0);  // lock vbl

#ifdef ATARI_DSP_C2P
	const bool tripleBufferFlip = !_dspDirect;
#else
	const bool tripleBufferFlip = true;
#endif
	if (screenUpdated
		&& _overlayState == kOverlayHidden
		&& tripleBufferFlip
		&& _currentState.mode == kTripleBuffering) {
		// Triple buffer:
		// - alternate BACK_BUFFER1 and BACK_BUFFER2
		// - present BACK_BUFFER1 (as BACK_BUFFER2)
		// - check if BACK_BUFFER2 has been displayed, if so, switch
		//   BACK_BUFFER2 and FRONT_BUFFER and make previous BACK_BUFFER2 work screen

		const bool presented = _ste ? (atari_ste_pending_screen == nullptr) : (s_screenSurf == nullptr);
		if (presented) {
			// BACK_BUFFER2 has been set; guard it from overwriting while presented
			Screen *tmp = _screen[kBackBuffer2];
			_screen[kBackBuffer2] = _screen[kFrontBuffer];
			_screen[kFrontBuffer] = tmp;
		}

		// swap back buffers
		Screen *tmp = _screen[kBackBuffer1];
		_screen[kBackBuffer1] = _screen[kBackBuffer2];
		_screen[kBackBuffer2] = tmp;

		// queue BACK_BUFFER2 with the most recent frame content
		_pendingScreenChanges.setScreenSurface(_screen[kBackBuffer2]->surf.get());
		// BACK_BUFFER1 is now current (work) buffer
	}

#ifdef SCREEN_ACTIVE
	_pendingScreenChanges.applyAfterVblLock(*workScreen);
#endif

	if (_pendingScreenChanges.screenSurface()) {
		if (_ste) {
			// Consumed by the raster's VBL hook. The palette has to be in
			// place before the screen address marks the pair as pending.
			AtariSurface *surf = _pendingScreenChanges.screenSurface();
			Screen *screen = findSteScreen(surf);
			const bool sceneLive = screen && screen->stePaletteGeneration != 0
				&& _overlayState == kOverlayHidden;
			atari_ste_pending_palette = sceneLive ? screen->stePalettes() : nullptr;
			atari_ste_pending_screen2 = sceneLive && screen->mixSurf() ? screen->mixSurf()->getBasePtr(0, 0) : nullptr;
			atari_ste_pending_screen = surf->getBasePtr(0, 0);
		} else {
			s_screenSurf = _pendingScreenChanges.screenSurface();
		}
		_pendingScreenChanges.setScreenSurface(nullptr);
	}
	if (_pendingScreenChanges.aspectRatioCorrectionYOffset().second)
		s_aspectRatioCorrectionYOffset = _pendingScreenChanges.aspectRatioCorrectionYOffset().first;
	if (_pendingScreenChanges.screenOffsets().second)
		s_setScreenOffsets = _pendingScreenChanges.screenOffsets().first;
	if (_pendingScreenChanges.shrinkVidelVisibleArea().second)
		s_shrinkVidelVisibleArea = _pendingScreenChanges.shrinkVidelVisibleArea().first;

	set_sysvar_to_short(vblsem, 1);  // unlock vbl
	_system->updateAudio();

	//debug("end of updateScreen");
}

void AtariGraphicsManager::setShakePos(int shakeXOffset, int shakeYOffset) {
	//debug("setShakePos: %d, %d", shakeXOffset, shakeYOffset);

	if (_tt) {
		// as TT can't horizontally shake anything, do it at least vertically
		s_shakeYOffset = (shakeYOffset == 0 && shakeXOffset != 0) ? shakeXOffset : shakeYOffset;
	} else {
		s_shakeXOffset = shakeXOffset;
		s_shakeYOffset = shakeYOffset;
	}

	_pendingScreenChanges.queueShakeScreen();
}

void AtariGraphicsManager::showOverlay(bool inGUI) {
	debug("showOverlay (state: %d, inGUI: %d)", _overlayState, inGUI);

	if (_overlayState == kOverlayVisible)
		return;

	if (_overlayState == kOverlayIgnoredHide) {
		_overlayState = kOverlayVisible;
		return;
	}

#ifdef ATARI_DSP_C2P
	waitDspScreen();
#endif

	if (_currentState.mode == kDirectRendering) {
		_screen[kFrontBuffer]->cursor.flushBackground(Common::Rect(), true);
	}

	_pendingScreenChanges.setScreenSurface(_screen[kOverlayBuffer]->surf.get());

	// cursor is reset before calling showOverlay()

	_screen[kOverlayBuffer]->reset(getOverlayWidth(), getOverlayHeight(), *lockOverlay());

	_overlayState = kOverlayVisible;

	if (!_pendingScreenChanges.empty()) {
		warning("showOverlay: _pendingScreenChanges is %02x", _pendingScreenChanges.get());
	}
	_pendingScreenChanges.queueAll();
	updateScreen();
}

void AtariGraphicsManager::hideOverlay() {
	debug("hideOverlay (ignore: %d, state: %d)", _ignoreHideOverlay, _overlayState);

	assert(_overlayState != kOverlayIgnoredHide);

	if (_overlayState == kOverlayHidden)
		return;

	if (_ignoreHideOverlay) {
		_overlayState = kOverlayIgnoredHide;
		return;
	}

	// In triple buffering, BACK_BUFFER2 contains the next desired frame. The
	// DSP direct-screen path stays on the displayed front buffer instead.
	int screenToShow = _currentState.mode == kTripleBuffering ? kBackBuffer2 : kFrontBuffer;
#ifdef ATARI_DSP_C2P
	if (_dspDirect)
		screenToShow = kFrontBuffer;
#endif
	_pendingScreenChanges.setScreenSurface(_screen[screenToShow]->surf.get());

	// reset cursor as its srcSurface has been just changed so wait for cursor surface to be updated
	Cursor::setSurface(nullptr, 0, 0, 0, 0, 0);

	_overlayState = kOverlayHidden;

	if (!_pendingScreenChanges.empty()) {
		warning("hideOverlay: _pendingScreenChanges is %02x", _pendingScreenChanges.get());
	}
	_pendingScreenChanges.queueAll();
	updateScreen();
}

Graphics::PixelFormat AtariGraphicsManager::getOverlayFormat() const {
#ifndef DISABLE_FANCY_THEMES
	return (_tt || _ste) ? PIXELFORMAT_RGB121 : PIXELFORMAT_RGB332;
#else
	return PIXELFORMAT_RGB121;
#endif
}

void AtariGraphicsManager::clearOverlay() {
	if (isOverlayDirectRendering())
		return;

	debug("clearOverlay");

	if (!isOverlayVisible())
		return;

	const Graphics::Surface &sourceSurface =
		_currentState.mode == kDirectRendering ? *_screen[kFrontBuffer]->offsettedSurf : _chunkySurfaceOffsetted;

	const bool upscale = _overlaySurface.w / sourceSurface.w >= 2 && _overlaySurface.h / sourceSurface.h >= 2;

	const int w = upscale ? sourceSurface.w * 2 : sourceSurface.w;
	const int h = upscale ? sourceSurface.h * 2 : sourceSurface.h;

	const int hzOffset = (_overlaySurface.w - w) / 2;
	const int vOffset  = (_overlaySurface.h - h) / 2;

	const int srcPadding = sourceSurface.pitch - sourceSurface.w;
	const int dstPadding = hzOffset * 2 + (upscale ? _overlaySurface.pitch : 0);

	// Transpose from game palette to RGB332/RGB121 (overlay palette)
	const byte *src = (const byte*)sourceSurface.getPixels();
	byte *dst = (byte *)_overlaySurface.getBasePtr(hzOffset, vOffset);

	// for TT: 8/4/0 + (xLoss - 4) + xShift
	static const int rShift = (_tt ? (8 - 4) : 0)
		+ _overlaySurface.format.rLoss - _overlaySurface.format.rShift;
	static const int gShift = (_tt ? (4 - 4) : 0)
		+ _overlaySurface.format.gLoss - _overlaySurface.format.gShift;
	static const int bShift = (_tt ? (0 - 4) : 0)
		+ _overlaySurface.format.bLoss - _overlaySurface.format.bShift;

	static const int rMask = _overlaySurface.format.rMax() << _overlaySurface.format.rShift;
	static const int gMask = _overlaySurface.format.gMax() << _overlaySurface.format.gShift;
	static const int bMask = _overlaySurface.format.bMax() << _overlaySurface.format.bShift;

	for (int y = 0; y < sourceSurface.h; y++) {
		for (int x = 0; x < sourceSurface.w; x++) {
			byte pixel;

			if (_tt) {
				// Bits 15-12    Bits 11-8     Bits 7-4      Bits 3-0
				// Reserved      Red           Green         Blue
				const uint16 &col = _palette.tt[*src++];
				pixel = ((col >> rShift) & rMask)
					  | ((col >> gShift) & gMask)
					  | ((col >> bShift) & bMask);
			} else {
				const _RGB &col = _palette.falcon[*src++];
				pixel = ((col.red   >> rShift) & rMask)
					  | ((col.green >> gShift) & gMask)
					  | ((col.blue  >> bShift) & bMask);
			}

			if (upscale) {
				*(dst + _overlaySurface.pitch) = pixel;
				*dst++ = pixel;
				*(dst + _overlaySurface.pitch) = pixel;
			}
			*dst++ = pixel;
		}

		src += srcPadding;
		dst += dstPadding;
	}

	// top rect
	memset(_overlaySurface.getBasePtr(0, 0), 0, vOffset * _overlaySurface.pitch);
	// bottom rect
	memset(_overlaySurface.getBasePtr(0, _overlaySurface.h - vOffset), 0, vOffset * _overlaySurface.pitch);
	// left rect
	_overlaySurface.fillRect(Common::Rect(0, vOffset, hzOffset, _overlaySurface.h - vOffset), 0);
	// right rect
	_overlaySurface.fillRect(Common::Rect(_overlaySurface.w - hzOffset, vOffset, _overlaySurface.w, _overlaySurface.h - vOffset), 0);

	_screen[kOverlayBuffer]->addDirtyRect(_overlaySurface, 0, 0, _overlaySurface.w, _overlaySurface.h, false);
}

void AtariGraphicsManager::grabOverlay(Graphics::Surface &surface) const {
	debug("grabOverlay: %d(%d), %d", surface.w, surface.pitch, surface.h);

	if (isOverlayDirectRendering()) {
		memset(surface.getPixels(), 0, surface.h * surface.pitch);
	} else {
		assert(surface.w >= _overlaySurface.w);
		assert(surface.h >= _overlaySurface.h);
		assert(surface.format.bytesPerPixel == _overlaySurface.format.bytesPerPixel);

		surface.copyRectToSurface(_overlaySurface.rawSurface(), 0, 0, Common::Rect(_overlaySurface.w, _overlaySurface.h));
	}
}

void AtariGraphicsManager::copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) {
	//debug("copyRectToOverlay: %d, %d, %d(%d), %d", x, y, w, pitch, h);

	Graphics::Surface &dstSurface = *lockOverlay();

	// if true, SuperVidel is not present
	const bool directRendering = isOverlayDirectRendering();

	_screen[kOverlayBuffer]->addDirtyRect(
		dstSurface,
		x, y, w, h,
		directRendering);

	if (directRendering) {
		copyRectToAtariSurface(
			*_screen[kOverlayBuffer]->offsettedSurf,
			(const byte *)buf, pitch, x, y, w, h);
	} else {
		dstSurface.copyRectToSurface(buf, pitch, x, y, w, h);
	}
}

Graphics::Surface *AtariGraphicsManager::lockOverlay() {
	//debug("lockOverlay");

	return isOverlayDirectRendering()
		? _screen[kOverlayBuffer]->offsettedSurf->surfacePtr()
		: _overlaySurface.surfacePtr();
}

bool AtariGraphicsManager::showMouse(bool visible) {
	//debug("showMouse: %d; ignored: %d", visible, _ignoreCursorChanges);

	if (_ignoreCursorChanges)
		return visible;

	bool lastOverlay, lastFront, lastBack1 = false;

	// TODO: cursor.flushBackground() if !visible
	lastOverlay = _screen[kOverlayBuffer]->cursor.setVisible(visible);
	lastFront   = _screen[kFrontBuffer]->cursor.setVisible(visible);

	if (_currentState.mode == kTripleBuffering) {
		lastBack1 = _screen[kBackBuffer1]->cursor.setVisible(visible);
		_screen[kBackBuffer2]->cursor.setVisible(visible);
	}

	if (isOverlayVisible())
		return lastOverlay;
	else if (_currentState.mode <= kSingleBuffering)
		return lastFront;
	else
		return lastBack1;
}

void AtariGraphicsManager::warpMouse(int x, int y) {
	//debug("warpMouse: %d, %d", x, y);

	if (isOverlayVisible()) {
		_screen[kOverlayBuffer]->cursor.setPosition(x, y);
	} else if (_currentState.mode <= kSingleBuffering) {
		_screen[kFrontBuffer]->cursor.setPosition(x, y);
	} else {
		_screen[kBackBuffer1]->cursor.setPosition(x, y);
		_screen[kBackBuffer2]->cursor.setPosition(x, y);
		_screen[kFrontBuffer]->cursor.setPosition(x, y);
	}
}

void AtariGraphicsManager::setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY, uint32 keycolor,
										  const Graphics::PixelFormat *format, const byte *mask, frac_t scaleX, frac_t scaleY) {
	//debug("setMouseCursor: %d, %d, %d, %d, %d, %d; ignored: %d",
	//	w, h, hotspotX, hotspotY, keycolor, format ? format->bytesPerPixel : 1, _ignoreCursorChanges);

	if (_ignoreCursorChanges)
		return;

	if (mask)
		warning("AtariGraphicsManager::setMouseCursor: Masks are not supported");

	if (format)
		assert(*format == PIXELFORMAT_CLUT8);

	Cursor::setSurface(buf, (int)w, (int)h, hotspotX, hotspotY, keycolor);

	_screen[kOverlayBuffer]->cursor.setSurfaceChanged();
	_screen[kFrontBuffer]->cursor.setSurfaceChanged();

	if (_currentState.mode == kTripleBuffering) {
		_screen[kBackBuffer1]->cursor.setSurfaceChanged();
		_screen[kBackBuffer2]->cursor.setSurfaceChanged();
	}
}

void AtariGraphicsManager::setCursorPalette(const byte *colors, uint start, uint num) {
	debug("setCursorPalette: %d, %d", start, num);

	Cursor::setPalette(colors, start, num);

	// cursor palette is supported only in the overlay
	_screen[kOverlayBuffer]->cursor.setSurfaceChanged();
}

void AtariGraphicsManager::updateMousePosition(int deltaX, int deltaY) {
	//debug("updateMousePosition: %d, %d", deltaX, deltaY);

	if (isOverlayVisible()) {
		_screen[kOverlayBuffer]->cursor.updatePosition(deltaX, deltaY);
	} else if (_currentState.mode <= kSingleBuffering) {
		_screen[kFrontBuffer]->cursor.updatePosition(deltaX, deltaY);
	} else {
		_screen[kBackBuffer1]->cursor.updatePosition(deltaX, deltaY);
		_screen[kBackBuffer2]->cursor.updatePosition(deltaX, deltaY);
		_screen[kFrontBuffer]->cursor.updatePosition(deltaX, deltaY);
	}
}

bool AtariGraphicsManager::notifyEvent(const Common::Event &event) {
	switch (event.type) {
	case Common::EVENT_RETURN_TO_LAUNCHER:
		if (isOverlayVisible()) {
			debug("Return to launcher from overlay");
			// clear work screen: this is needed if *next* game shows an error upon startup
			fillScreen(0);

			_ignoreHideOverlay = true;
			// gui manager would want to hide overlay, set game cursor etc
			_ignoreCursorChanges = true;
			return false;
		}
		break;

	case Common::EVENT_CUSTOM_BACKEND_ACTION_START:
		switch ((CustomEventAction) event.customType) {
		case kActionToggleAspectRatioCorrection:
			if (hasFeature(OSystem::Feature::kFeatureAspectRatioCorrection)) {
				_aspectRatioCorrection = !_aspectRatioCorrection;

				if (_overlayState == kOverlayHidden) {
					_pendingScreenChanges.queueAspectRatioCorrection();

					updateScreen();
				}
				return true;
			}
			break;
		}
		break;

	default:
		return false;
	}

	return false;
}

Common::Keymap *AtariGraphicsManager::getKeymap() const {
	Common::Keymap *keymap = new Common::Keymap(Common::Keymap::kKeymapTypeGlobal, "atari-graphics", _("Graphics"));
	Common::Action *act;

	if (hasFeature(OSystem::kFeatureAspectRatioCorrection)) {
		act = new Common::Action("ASPT", _("Toggle aspect ratio correction"));
		act->addDefaultInputMapping("C+A+a");
		act->setCustomBackendActionEvent(kActionToggleAspectRatioCorrection);
		keymap->addAction(act);
	}

	return keymap;
}

void AtariGraphicsManager::allocateSurfaces() {
	const Graphics::PixelFormat screenFormat = _ste ? PIXELFORMAT_RGB121 : PIXELFORMAT_CLUT8;
	int steScheduleWords = 0;
	bool steMix = false;

	if (_ste) {
		// Display lines the beam-timed palette raster covers; the lines below
		// (the SCUMM verb interface in Monkey Island and Atlantis) share one
		// palette and cost the engine no CPU time.
		ConfMan.registerDefault("ste_raster_lines", 144);
		ConfMan.registerDefault("ste_raster_60hz", false);
		// The beam-timed per-line palette loop (verified pixel-exact by
		// devtools/atari-ste/raster-test/timerb-raster-test.s). Set ste_raster=false
		// in the INI to fall back to one stable 16-colour palette per frame.
		ConfMan.registerDefault("ste_raster", true);

		const int lines = CLIP<int>(ConfMan.getInt("ste_raster_lines"),
			ATARI_STE_RASTER_MIN_LINES, ATARI_STE_RASTER_MAX_LINES);
		atari_ste_raster_lines = lines;
		atari_ste_raster_60hz = ConfMan.getBool("ste_raster_60hz") ? 1 : 0;
		atari_ste_raster_enable = ConfMan.getBool("ste_raster") ? 1 : 0;
		steScheduleWords = AtariSteSceneRenderer::scheduleWords(lines);

		_steSceneRenderer = new AtariSteSceneRenderer();
		_steSceneRenderer->setSceneLines(lines);

		// Temporal colour mixing is the default STE display: a table from
		// devtools/atari-ste/tools/monkey-flicker-compare.mjs maps every
		// source colour to a pair of palette entries, and the VBL hook
		// alternates the two fields of each buffer. Without ste_mix_lut the
		// table is MIX\DUAL20.BIN next to the program, whose two palettes
		// alternate; an empty ste_mix_lut, or a table that cannot be loaded,
		// keeps the per-line raster. Without ste_mix_pattern, one-palette
		// tables use the checkerboard. Graphics are set up before a game domain
		// is active, so the keys belong in [scummvm].
		ConfMan.registerDefault("ste_mix_lut", "MIX/DUAL20.BIN");
		const Common::Path mixLut = ConfMan.getPath("ste_mix_lut");
		if (!mixLut.empty()) {
			const Common::String pattern = ConfMan.get("ste_mix_pattern");
			const AtariSteSceneRenderer::MixPattern mixPattern =
				pattern == "alternate" ? AtariSteSceneRenderer::kMixAlternate
				: pattern == "static" ? AtariSteSceneRenderer::kMixStatic
				: AtariSteSceneRenderer::kMixChecker;
			steMix = _steSceneRenderer->loadMix(mixLut, mixPattern);
			if (steMix) {
				if (_steSceneRenderer->mixDual() && !pattern.empty() && mixPattern != AtariSteSceneRenderer::kMixAlternate)
					warning("STE mix: two palettes can only alternate per field");
				atari_ste_raster_enable = 0;
				atari_ste_mix_split = _steSceneRenderer->mixSplit() ? 1 : 0;
				// A table holds one room's colours, so the engine's room
				// number picks a better one when the game has it; see
				// steSetRoom().
				_steMixPattern = mixPattern;
				_steMixDefault = mixLut;
				_steMixLoaded = mixLut;
				s_steGraphicsManager = this;
			} else {
				warning("STE mix: keeping the per-line raster");
			}
		}
	}

	for (int i : { kFrontBuffer, kBackBuffer1, kBackBuffer2 }) {
		_screen[i] = new Screen(_tt, _ste, getMaximumScreenWidth(), getMaximumScreenHeight(), screenFormat, &_palette, steScheduleWords, steMix);
		s_steScreens[i] = _screen[i];
	}
	_screen[kOverlayBuffer] = new Screen(_tt, _ste, getOverlayWidth(), getOverlayHeight(), getOverlayFormat(), &_overlayPalette, 0);
	// initial position
	_screen[kOverlayBuffer]->cursor.setPosition(getOverlayWidth() / 2, getOverlayHeight() / 2);

	_chunkySurface.create(getMaximumScreenWidth(), getMaximumScreenHeight(), PIXELFORMAT_CLUT8);
	_chunkySurfaceOffsetted = *_chunkySurface.surfacePtr();
	_overlaySurface.create(getOverlayWidth(), getOverlayHeight(), getOverlayFormat());
}

#ifdef ATARI_STE_GAME_ONLY
// Called by the engine (scumm/room.cpp) once a room is set up.
void atari_ste_scene_room(int room) {
	if (s_steGraphicsManager)
		s_steGraphicsManager->steSetRoom(room);
}
#endif

void AtariGraphicsManager::steSetRoom(int room) {
	if (!_ste || !_steSceneRenderer || !_steSceneRenderer->mixEnabled() || _steMixDefault.empty())
		return;

	// One table per room, next to the default one, named after the room. A
	// room without its own table falls back to the default table rather than
	// to the table of the room before it.
	Common::Path path = _steMixDefault.getParent().appendComponent(Common::String::format("R%03d.BIN", room));
	if (!Common::FSNode(path).exists())
		path = _steMixDefault;
	if (path == _steMixLoaded)
		return;

	if (!_steSceneRenderer->loadMix(path, _steMixPattern)) {
		// loadMix() leaves mixing off when it fails, so put the table that
		// was displaying back; the raster is not available at this point.
		if (!_steSceneRenderer->loadMix(_steMixLoaded, _steMixPattern))
			error("STE mix: lost the mixing table of room %d", room);
		return;
	}

	_steMixLoaded = path;
	atari_ste_mix_split = _steSceneRenderer->mixSplit() ? 1 : 0;
	// The pairs of every converted pixel are stale.
	for (int i : { kFrontBuffer, kBackBuffer1, kBackBuffer2 }) {
		if (_screen[i])
			_screen[i]->fullRedraw = true;
	}
	debug("STE mix: room %d uses %s", room, path.toString(Common::Path::kNativeSeparator).c_str());
}

void AtariGraphicsManager::freeSurfaces() {
#ifdef ATARI_DSP_C2P
	waitDspScreen();
#endif
	for (int i : { kFrontBuffer, kBackBuffer1, kBackBuffer2, kOverlayBuffer }) {
		delete _screen[i];
		_screen[i] = nullptr;
	}

	_chunkySurfaceOffsetted.init(0, 0, 0, nullptr, Graphics::PixelFormat());
	_chunkySurface.free();
	_overlaySurface.free();
	if (s_steGraphicsManager == this)
		s_steGraphicsManager = nullptr;
	delete _steSceneRenderer;
	_steSceneRenderer = nullptr;
	for (Screen *&screen : s_steScreens)
		screen = nullptr;
}

void AtariGraphicsManager::addDirtyRectToScreens(const Graphics::Surface &dstSurface, int x, int y, int w, int h, bool directRendering) {
	_screen[kFrontBuffer]->addDirtyRect(dstSurface, x, y, w, h, directRendering);

	if (_currentState.mode > kSingleBuffering) {
		_screen[kBackBuffer1]->addDirtyRect(dstSurface, x, y, w, h, directRendering);
		_screen[kBackBuffer2]->addDirtyRect(dstSurface, x, y, w, h, directRendering);
	}
}

#ifdef ATARI_DSP_C2P
bool AtariGraphicsManager::updateScreenDsp(Screen *dstScreen, const Graphics::Surface *srcSurface, bool &updated) {
	if (_dspScreen && _dspScreen != dstScreen)
		waitDspScreen();

	if (_dspScreen) {
		// The DMA record writes into the displayed surface while the game runs.
		// Scanout can therefore see a partially converted frame.
		if (AtariDspC2p::busy()) {
			_dspFrameWanted = true;
			updated = false;
			return true;
		}
		_dspScreen = nullptr;
		if (AtariDspC2p::finish()) {
			// The conversion covered the cursor: draw it again.
			LockSuperBlitter();
			Cursor &cursor = dstScreen->cursor;
			cursor.update();
			if (cursor.isVisible())
				cursor.draw();
			UnlockSuperBlitter();
			// The surface was already displayed while the DSP wrote it.
			updated = false;
			return true;
		}
		// The driver gave up: the 68030 converts everything again.
		dstScreen->addDirtyRect(_chunkySurfaceOffsetted, 0, 0, _chunkySurfaceOffsetted.w, _chunkySurfaceOffsetted.h, false);
		return false;
	}

	_dspFrameWanted = false;
	if (!AtariDspC2p::available()
		|| _overlayState != kOverlayHidden
		|| (_currentState.mode != kTripleBuffering && !(_dspDirect && _currentState.mode == kDirectRendering))
		|| srcSurface != _chunkySurface.surfacePtr())
		return false;
	// A cursor that moved over an unchanged frame is cheaper on the 68030.
	if (dstScreen->dirtyRects.empty() && !dstScreen->fullRedraw)
		return false;

	// Whole lines only: the planar lines are the chunky surface's, with
	// equal margins either side, in 8 bitplanes.
	const AtariSurface &surf = *dstScreen->surf;
	const AtariSurface &dst = *dstScreen->offsettedSurf;
	const int marginBytes = (surf.pitch - srcSurface->w) / 2;
	const byte *view = (const byte *)dst.getPixels();
	if (dst.getBitsPerPixel() != 8
		|| dst.w != srcSurface->w
		|| srcSurface->pitch != srcSurface->w
		|| srcSurface->w % 16 != 0
		|| marginBytes < 0 || marginBytes % 2 != 0 || marginBytes / 2 > 15
		|| surf.pitch != srcSurface->w + 2 * marginBytes
		|| (view - (const byte *)surf.getPixels()) % surf.pitch != marginBytes)
		return false;

	// Whatever the game draws from now on is dirty again, for the next one.
	dstScreen->clearDirtyRects();
	if (!AtariDspC2p::start((const byte *)srcSurface->getPixels(), srcSurface->h, srcSurface->w / 16,
			marginBytes / 2, const_cast<byte *>(view) - marginBytes)) {
		dstScreen->addDirtyRect(_chunkySurfaceOffsetted, 0, 0, _chunkySurfaceOffsetted.w, _chunkySurfaceOffsetted.h, false);
		return false;
	}
	_dspScreen = dstScreen;
	updated = false;
	return true;
}

void AtariGraphicsManager::pollDspScreen() {
	if (_dspScreen ? AtariDspC2p::busy() : !_dspFrameWanted)
		return;
	updateScreen();
}

void AtariGraphicsManager::waitDspScreen() {
	if (!_dspScreen)
		return;
	AtariDspC2p::wait();
	// Keep this surface eligible for a fresh conversion after the forced wait.
	_dspScreen->addDirtyRect(_chunkySurfaceOffsetted, 0, 0, _chunkySurfaceOffsetted.w, _chunkySurfaceOffsetted.h, false);
	_dspScreen = nullptr;
}
#endif

bool AtariGraphicsManager::updateScreenInternal(Screen *dstScreen, const Graphics::Surface *srcSurface) {
	//debug("updateScreenInternal");

	const Screen::DirtyRects &dirtyRects = dstScreen->dirtyRects;
	auto &dstSurface                     = *dstScreen->offsettedSurf;
	Cursor &cursor                       = dstScreen->cursor;

	bool updated = false;

	LockSuperBlitter();

	if (_ste && _overlayState == kOverlayHidden && srcSurface) {
		// The STE converts the engine's finished frame, so the mouse cursor is
		// drawn into it around the conversion instead of being blitted into the
		// four-plane screen. Each buffer remembers the rectangle its last
		// conversion covered, so a cursor that moved is repainted in all of
		// them, whichever frame each one last saw.
		bool cursorChanged = false;
		const Common::Rect cursorRect = cursor.steUpdate(cursorChanged);
		const int cursorOffset = (dstSurface.w - _chunkySurfaceOffsetted.w) / 2;
		const Common::Rect alignedCursorRect = cursorRect.isEmpty()
			? Common::Rect()
			: AtariSurface::alignRect(
				cursorRect.left + cursorOffset, cursorRect.top,
				cursorRect.right + cursorOffset, cursorRect.bottom);

		if (!dstScreen->fullRedraw && (cursorChanged || alignedCursorRect != dstScreen->steCursorRect)) {
			if (!dstScreen->steCursorRect.isEmpty())
				dstScreen->dirtyRects.insert(dstScreen->steCursorRect);
			if (!alignedCursorRect.isEmpty())
				dstScreen->dirtyRects.insert(alignedCursorRect);
		}

		dstScreen->steCursorRect = alignedCursorRect;

		// Keep both extents of this buffer's accumulated dirty rectangles.
		// Collapsing them to dirty rows makes a narrow actor repaint all 320
		// pixels of every affected line. Full redraws still convert everything.
		if (!dstScreen->fullRedraw && dirtyRects.empty()) {
			UnlockSuperBlitter();
			return false;
		}

#ifdef ATARI_STE_GAME_ONLY
		atari_ste_last_source = srcSurface->getPixels();
		atari_ste_last_palette = _palette.falcon;
		if (atari_ste_bench_state.enabled) {
			atari_ste_bench_state.rendererActive = 1;
			++atari_ste_bench_state.conversions;
			atari_ste_bench_mark(3);
		}
#endif
		// The cursor is part of the frame only while it is converted.
		cursor.steDraw(_chunkySurfaceOffsetted);
		if (dstScreen->mixSurf())
			_steSceneRenderer->convertMix(*srcSurface, *dstScreen->offsettedSurf, *dstScreen->mixSurf(),
				_palette.falcon, dstScreen->stePalettes(), dstScreen->stePaletteGeneration,
				dstScreen->fullRedraw ? nullptr : &dirtyRects);
		else
			_steSceneRenderer->convert(*srcSurface, *dstScreen->offsettedSurf,
				_palette.falcon, dstScreen->stePalettes(), dstScreen->stePaletteGeneration,
				dstScreen->fullRedraw ? nullptr : &dirtyRects);
		cursor.steRestore(_chunkySurfaceOffsetted);
#ifdef ATARI_STE_GAME_ONLY
		if (atari_ste_bench_state.enabled) {
			atari_ste_bench_state.rendererActive = 0;
			atari_ste_bench_mark(4);
		}
#endif
		dstScreen->clearDirtyRects();
		UnlockSuperBlitter();

		static uint32 steUpdates;
		if ((++steUpdates & 63) == 0) {
			debug("STE raster: updates %lu, vbl %lu, frames %lu, resynced %lu, skipped %lu, lost ticks %lu, millis %lu",
				(unsigned long)steUpdates, (unsigned long)atari_ste_vbl_count,
				(unsigned long)atari_ste_raster_frames, (unsigned long)atari_ste_raster_resynced,
				(unsigned long)atari_ste_raster_skipped, (unsigned long)atari_ste_raster_lost_ticks,
				(unsigned long)g_system->getMillis());
		}
		return true;
	}

	if (cursor.isChanged()) {
		const Common::Rect cursorBackgroundRect = cursor.flushBackground(Common::Rect(), srcSurface == nullptr);
		if (!cursorBackgroundRect.isEmpty()) {
			dstSurface.copyRectToSurface(*srcSurface, cursorBackgroundRect.left, cursorBackgroundRect.top, cursorBackgroundRect);
			updated |= true;
		}
	}

	// update cursor rects and visibility flag (if out of screen)
	cursor.update();

	const bool drawCursor = cursor.isVisible() && (dstScreen->fullRedraw || cursor.isChanged());

	if (srcSurface) {
		for (auto it = dirtyRects.begin(); it != dirtyRects.end(); ++it) {
#ifdef ATARI_FALCON_GAME_ONLY
			// Bound the interval without audio servicing during chunky-to-
			// planar conversion. Each batch preserves the dirty x extent.
			for (int top = it->top; top < it->bottom; top += 16) {
				Common::Rect batch(it->left, top, it->right, MIN<int>(top + 16, it->bottom));
				dstSurface.copyRectToSurface(*srcSurface, batch.left, batch.top, batch);
				_system->updateAudio();
			}
#else
			dstSurface.copyRectToSurface(*srcSurface, it->left, it->top, *it);
#endif
		}
		updated |= !dirtyRects.empty();
	} else if (drawCursor) {
		cursor.saveBackground();
	}

	if (drawCursor) {
		cursor.draw();
		updated |= true;
	}

	dstScreen->clearDirtyRects();

	UnlockSuperBlitter();

	return updated;
}

void AtariGraphicsManager::copyRectToAtariSurface(AtariSurface &dstSurface,
												  const byte *buf, int pitch, int x, int y, int w, int h) {
	const Common::Rect rect = AtariSurface::alignRect(x, y, x + w, y + h);

	// TODO: mask the unaligned parts and copy the rest
	buf -= (x - rect.left);	// HACK: this assumes pointer to a complete buffer

	dstSurface.copyRectToSurface(buf, pitch, rect.left, rect.top, rect.width(), rect.height());
}
