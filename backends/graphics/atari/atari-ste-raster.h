/* ScummVM - Graphic Adventure Engine
 *
 * Atari STE beam-timed palette raster (see atari-ste-raster.S).
 *
 * One packed schedule describes every palette write of a frame:
 *
 *   words 0..15    palette of display lines 0 and 1, written during the
 *                  vertical blank
 *   words 16..31   written in the right border of line 1: the palette
 *                  line 2 starts with
 *   then, for every line y = 2 .. lines-1, 48 words:
 *     16 words     written across the left third of line y (group 1)
 *     16 words     written across the middle third of line y (group 2)
 *     16 words     written in the right border of line y: the palette
 *                  line y+1 starts with; for the last raster line this is
 *                  the palette of all remaining, unrastered lines
 *
 * The write positions of groups 1 and 2 are those of the Spectrum 512
 * schedule in ste/raster.s (Spectrum512Painter), so a host-side slot model
 * for that schedule applies unchanged from line 2 on. A schedule that only
 * wants one palette per line repeats the line's own palette in groups 1
 * and 2 and puts the next line's palette in group 3.
 */

#ifndef BACKENDS_GRAPHICS_ATARI_STE_RASTER_H
#define BACKENDS_GRAPHICS_ATARI_STE_RASTER_H

#define ATARI_STE_RASTER_MIN_LINES 16
#define ATARI_STE_RASTER_MAX_LINES 200
#define ATARI_STE_RASTER_HEADER_WORDS 32
#define ATARI_STE_RASTER_LINE_WORDS 48

#ifdef __cplusplus

#include "common/scummsys.h"

extern "C" {

// Schedule the raster currently plays; taken from the pending pair below.
extern volatile const uint16 *atari_ste_display_palette;

// Presentation handshake. The engine stores the palette first and then the
// screen address, which marks the pair as pending; the VBL hook consumes it
// (honouring the TOS vblsem lock) and clears the screen address.
extern volatile const uint16 *atari_ste_pending_palette;
extern volatile const void *atari_ste_pending_screen;

// Colour mixing: the second field of the pending screen, stored before
// atari_ste_pending_screen (null for a single field). While a pair is shown
// the VBL hook alternates the two fields every frame and never arms the
// raster. It writes palette words 0..15 for the first field and 32..47 for
// the second; with atari_ste_mix_split set, a Timer B interrupt after
// atari_ste_raster_lines lines installs words 16..31 or 48..63 respectively
// for the lines below (the verb bar).
extern volatile const void *atari_ste_pending_screen2;
extern volatile uint16 atari_ste_mix_split;

// Display lines the raster covers (lines 0 .. n-1); the schedule layout
// above depends on it. Set before the first frame is presented.
extern volatile uint16 atari_ste_raster_lines;
// Non-zero: switch the display to 60 Hz at install (restored at uninstall).
extern volatile uint16 atari_ste_raster_60hz;

// Non-zero arms the per-line Timer-B loop. Off by default: the frame then
// keeps one stable 16-colour palette (lines 0/1). The beam-timed loop is still
// under bring-up; see devtools/atari-ste/raster-test/timerb-raster-test.s.
extern volatile uint16 atari_ste_raster_enable;

// Statistics.
extern volatile uint32 atari_ste_vbl_count;
extern volatile uint32 atari_ste_raster_frames;      // frames the timed loop ran
extern volatile uint32 atari_ste_raster_resynced;    // frames that had to start a line late
extern volatile uint32 atari_ste_raster_skipped;     // frames left with the line-0 palette only
extern volatile uint32 atari_ste_raster_lost_ticks;  // 200 Hz ticks masked away; getMillis() adds them

// Screen address and schedule the loop used in the last frame (for dumps).
extern volatile const void *atari_ste_raster_last_screen;
extern volatile const uint16 *atari_ste_raster_last_palette;

// Both via Supexec().
long atari_ste_raster_install(void);
long atari_ste_raster_uninstall(void);

}

#endif

#endif
