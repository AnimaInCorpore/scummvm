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

#ifdef ATARI_DSP_C2P

#define FORCE_TEXT_CONSOLE
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "backends/platform/atari/atari-dsp-c2p.h"

#include <mint/falcon.h>
#include <mint/osbind.h>

#include "common/debug.h"
#include "common/textconsole.h"

#include "backends/platform/atari/dsp-c2p-image.h"

namespace {

// Kernel commands, in the command word's top byte; see hsc2p.asm.
enum {
	kCmdPing = 0x010000,
	kCmdArm = 0x020000,
	kCmdGeometry = 0x030000,	// lines << 4 | pad words
	kCmdSync = 0x040000,		// groups per line; starts the screen
	kCmdState = 0x050000,		// 0 idle, 1 converting, 2 done
	kCmdGroups = 0x060000,		// groups converted this screen
	kReplyPing = 0x485332
};

enum {
	kStateDone = 2
};

const int kDspAbility = 3;
// A 320x200 screen takes about 120 ms under Hatari; a stuck one is off.
const uint32 kTimeoutTicks = 400;	// 200 Hz: 2 s

bool s_dspLocked;
bool s_soundLocked;
bool s_available;
bool s_inFlight;
uint32 s_expectedGroups;
uint32 s_startTick;
uint32 s_screens;
uint32 s_ticksTotal;

uint32 exchange(uint32 command) {
	long out = (long)command;
	long reply = 0;
	Dsp_BlkUnpacked(&out, 1, &reply, 1);
	return (uint32)reply & 0xffffff;
}

long readHz200Super() {
	return *(volatile long *)0x4baL;
}

uint32 ticks() {
	return (uint32)Supexec(readHz200Super);
}

// DMA record writes memory behind the data cache's back.
long clearDataCacheSuper() {
	__asm__ __volatile__(
		"\tmovec	%%cacr,%%d0\n"
		"\tbset		#11,%%d0\n"
		"\tmovec	%%d0,%%cacr\n"
		: : : "d0", "cc", "memory");
	return 0;
}

bool recording() {
	return (Buffoper(-1) & SB_REC_ENA) != 0;
}

void releaseSound() {
	if (!s_soundLocked)
		return;
	// Hand the DAC back to DMA playback.
	Buffoper(0);
	Dsptristate(DSP_TRISTATE, DSP_TRISTATE);
	Settracks(0, 0);
	Devconnect(DMAPLAY, DAC, CLK25M, CLK50K, NO_SHAKE);
	Unlocksnd();
	s_soundLocked = false;
}

void disable(const char *why) {
	warning("AtariDspC2p: %s; converting on the 68030 from now on", why);
	s_available = false;
	s_inFlight = false;
	releaseSound();
}

} // namespace

namespace AtariDspC2p {

bool init() {
	if (s_available)
		return true;
	if (Dsp_Lock() != 0) {
		warning("AtariDspC2p: the DSP is locked by another program");
		return false;
	}
	s_dspLocked = true;
	if (Dsp_Reserve(16, 16) < 0) {
		warning("AtariDspC2p: Dsp_Reserve failed");
		shutdown();
		return false;
	}
	Dsp_ExecBoot(kAtariDspC2pBoot, ATARI_DSP_C2P_BOOT_WORDS, kDspAbility);
	const uint32 ping = exchange(kCmdPing);
	if (ping != kReplyPing) {
		warning("AtariDspC2p: the kernel answered %06x to a ping", ping);
		shutdown();
		return false;
	}
	if (Locksnd() != 1) {
		warning("AtariDspC2p: Locksnd failed");
		shutdown();
		return false;
	}
	s_soundLocked = true;

	// The matrix state outlives whatever set it: pin every route. Four
	// tracks each way fill the frame, so one screen word rides every slot.
	Buffoper(0);
	Sndstatus(SND_RESET);
	Soundcmd(ADDERIN, MATIN);
	Setmode(STEREO16);
	Settracks(3, 3);
	Setmontracks(0);
	Dsptristate(DSP_ENABLE, DSP_ENABLE);
	Devconnect(DMAPLAY, DSPRECV, CLK25M, CLK50K, HANDSHAKE);
	Devconnect(DSPXMIT, DMAREC, CLK25M, CLK50K, HANDSHAKE);
	exchange(kCmdArm);
	// Let the new connection settle.
	const uint32 armed = ticks();
	while (ticks() - armed < 40)
		;

	s_available = true;
	s_inFlight = false;
	s_screens = s_ticksTotal = 0;
	debug("AtariDspC2p: kernel booted, %d words; screens convert on the DSP", ATARI_DSP_C2P_BOOT_WORDS);
	return true;
}

void shutdown() {
	if (s_inFlight)
		wait();
	if (s_screens)
		debug("AtariDspC2p: %u screens, avg %u ms start to observed completion",
			s_screens, s_ticksTotal * 5 / s_screens);
	s_available = false;
	releaseSound();
	if (s_dspLocked) {
		Dsp_Unlock();
		s_dspLocked = false;
	}
}

bool available() {
	return s_available;
}

bool start(const byte *chunky, int lines, int groupsPerLine, int padWords, byte *planar) {
	if (!s_available || s_inFlight)
		return false;
	assert(lines > 0 && lines < 4096 && groupsPerLine > 0 && groupsPerLine < 256 && padWords >= 0 && padWords < 16);

	const uint32 lineBytes = (groupsPerLine * 8 + 2 * padWords) * 2;
	exchange(kCmdGeometry + ((uint32)lines << 4) + padWords);
	// The DSP asks for the first word before the DMA runs.
	exchange(kCmdSync + groupsPerLine);
	Setbuffer(SR_PLAY, chunky, chunky + lines * groupsPerLine * 16);
	Setbuffer(SR_RECORD, planar, planar + lines * lineBytes);
	Buffoper(SB_PLA_ENA | SB_REC_ENA);

	s_expectedGroups = (uint32)lines * groupsPerLine;
	s_startTick = ticks();
	s_inFlight = true;
	return true;
}

bool busy() {
	if (!s_inFlight)
		return false;
	if (!recording())
		return false;
	if (ticks() - s_startTick > kTimeoutTicks) {
		Buffoper(0);
		disable("a screen timed out");
		return false;
	}
	return true;
}

bool finish() {
	if (!s_inFlight)
		return s_available;
	s_inFlight = false;
	Supexec(clearDataCacheSuper);
	s_ticksTotal += ticks() - s_startTick;
	++s_screens;

	const uint32 state = exchange(kCmdState);
	const uint32 groups = exchange(kCmdGroups);
	if (state != kStateDone || groups != s_expectedGroups) {
		warning("AtariDspC2p: the kernel ended a screen in state %u after %u of %u groups",
			state, groups, s_expectedGroups);
		disable("the kernel lost a screen");
		return false;
	}
	if (s_screens == 1 || (s_screens & 63) == 0)
		debug("AtariDspC2p: %u screens, avg %u ms start to observed completion",
			s_screens, s_ticksTotal * 5 / s_screens);
	return true;
}

void wait() {
	while (busy())
		;
	finish();
}

} // namespace AtariDspC2p

#endif
