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

#ifndef BACKENDS_PLATFORM_ATARI_CRITICAL_H
#define BACKENDS_PLATFORM_ATARI_CRITICAL_H

/*
 * How deep the main loop is inside a critical section: every mutex it
 * holds and every allocator call it is in. The Atari has one thread, so
 * mutexes cannot block; instead the DSP audio interrupt (atari-dsp.h),
 * which runs the AdLib driver and iMUSE's sequencing, produces only while
 * this depth is zero. That gives the interrupt-driven synthesis the same
 * exclusion the threaded backends get from real mutexes, provided every
 * state it shares with the main loop is guarded by a mutex there, which
 * is what those backends already require.
 *
 * The count is a single add or subtract on a long, so the interrupt can
 * never see a half-written value.
 */

#ifdef __cplusplus
extern "C" {
#endif
extern volatile long g_atariCriticalDepth;    /* mutexes held */
extern volatile long g_atariAllocatorDepth;   /* allocator calls in progress */
#ifdef __cplusplus
}
#endif

#define ATARI_CRITICAL_ENTER() (++g_atariCriticalDepth)
#define ATARI_CRITICAL_LEAVE() (--g_atariCriticalDepth)
#define ATARI_ALLOCATOR_ENTER() (++g_atariAllocatorDepth)
#define ATARI_ALLOCATOR_LEAVE() (--g_atariAllocatorDepth)

#ifdef __cplusplus
/** Holds the main loop inside a critical section for a scope. */
struct AtariCriticalSection {
	AtariCriticalSection() { ATARI_CRITICAL_ENTER(); }
	~AtariCriticalSection() { ATARI_CRITICAL_LEAVE(); }
};
#endif

#endif
