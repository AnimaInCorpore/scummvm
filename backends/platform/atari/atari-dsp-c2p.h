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

#ifndef BACKENDS_PLATFORM_ATARI_DSP_C2P_H
#define BACKENDS_PLATFORM_ATARI_DSP_C2P_H

#ifdef ATARI_DSP_C2P

#include "common/scummsys.h"

// The Falcon030's chunky-to-planar conversion on the DSP, in the background:
// DMA playback streams the 8-bit chunky screen into the DSP's SSI receiver
// and DMA record writes the SSI transmitter's 8-plane words into the planar
// screen, both at 25.175 MHz in handshake mode, so the DSP paces the
// transfers and no word is lost however busy the 68030 is. The kernel is
// devtools/atari-falcon030/tools/ssi-dma-c2p/dsp/hsc2p.asm; the DSP and the
// sound DMA belong to it, so the build plays no sound.
//
// One screen at a time: start() returns at once, and the 68030 runs the
// game while the DMA and the DSP convert directly into the displayed planar
// screen. Scanout can see the conversion in progress. The chunky pixels must
// stay put until busy() turns false; finish() clears the data cache after DMA.
namespace AtariDspC2p {

// Boots the kernel and takes the sound system; false leaves both alone.
bool init();
void shutdown();
bool available();

// Converts lines of groupsPerLine 16-pixel groups from chunky (contiguous,
// groupsPerLine * 16 bytes a line) into planar lines of padWords zero words,
// the line's plane words and padWords zero words, back to back from planar.
// Both buffers must be in ST-RAM and 16-byte aligned.
bool start(const byte *chunky, int lines, int groupsPerLine, int padWords, byte *planar);
bool busy();
// After busy() turned false: makes the planar lines visible to the 68030.
// False if the kernel did not finish the screen; the driver is then off.
bool finish();
// Blocks until the screen in flight is done, then finish()es it.
void wait();

} // namespace AtariDspC2p

#endif

#endif
