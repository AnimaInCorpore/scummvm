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

#ifndef BACKENDS_PLATFORM_ATARI_DSP_OPL_H
#define BACKENDS_PLATFORM_ATARI_DSP_OPL_H

#ifdef ATARI_DSP_OPL

#include "audio/fmopl.h"
#include "backends/platform/atari/atari-dsp.h"
#include "devtools/atari-falcon030/tools/foa-opl3/opl-practical.h"

/**
 * An OPL2 whose synthesis runs on the Falcon030's DSP.
 *
 * The AdLib driver above it is unchanged: it writes registers and asks for
 * a 250 Hz timer. Register writes go through the practical kernel's decoder
 * into parameter events for the DSP's operator records, stamped with the
 * 48-frame block of the period being produced; the timer callbacks run
 * inside period production, on the audio clock, so a callback's writes land
 * in the block that corresponds to its time. AtariMixerManager owns the
 * transport, whose interrupt calls producePeriod() once per 768-frame
 * period, so the callbacks, and with them iMUSE's sequencing, run in
 * interrupt context (see atari-dsp.h for what that requires).
 */
class AtariDspOPL : public ::OPL::OPL {
public:
	static ::OPL::OPL *create();
	static AtariDspOPL *instance() { return s_instance; }

	~AtariDspOPL() override;

	bool init() override;
	void reset() override;
	void write(int a, int v) override;
	void writeReg(int r, int v) override;
	void setCallbackFrequency(int timerFrequency) override;

	/**
	 * Moves the writes made between periods into this one. They are kept
	 * apart from the instance because the last of them, the reset a closing
	 * OPL leaves the kernel with, has no instance left to deliver it. Only
	 * for a period produced while the main loop is outside every critical
	 * section, as the writers run inside one.
	 */
	static void flushPending(AtariDspAudio *audio, AtariDspAudio::Period *period);

	/** Runs the timer callbacks that fall in this period and collects their writes into it. */
	void producePeriod(AtariDspAudio::Period *period);

protected:
	void startCallbacks(int timerFrequency) override;
	void stopCallbacks() override;

private:
	struct EventSink : OplPractical::Sink {
		AtariDspOPL *owner;
		void write(uint32 block, uint16 address, int32 value) override;
	};

	explicit AtariDspOPL(AtariDspAudio *audio);
	void emit(uint32 block, uint16 address, int32 value);

	AtariDspAudio *_audio;
	AtariDspAudio::Period *_period;
	EventSink _sink;
	OplPractical::Decoder *_decoder;
	int _address;
	bool _running;
	uint32 _framesPerTick16;   // codec frames per callback, 16.16
	uint32 _nextTick16;        // next callback's frame within the period, 16.16
	uint32 _block;             // block of the period the current writes belong to
	bool _resetting;           // the decoder's reset is being emitted

	void resetDecoder(uint32 atBlock);
	void noteLost();

	// Writes between periods wait here for the next one. A song start from
	// the main loop can write several hundred registers while the interrupt
	// is refused production. Losing one is recovered, not fatal: the loss is
	// noted and the next period resends the decoder's shadow. The cap leaves
	// a fresh period room for a full queue, that resend, the master gain and
	// the period's own writes, so the resend can never be what overflows.
	enum { kPendingMax = 1024 };
	static uint32 s_pending[kPendingMax * 2];
	static uint32 s_pendingCount;

	// An event never reached the kernel, so the next period resends the
	// decoder's shadow - the machine's own words too if it was a reset's.
	// Static like the queue, which can outlive the instance.
	static bool s_resync, s_resyncMachine;

	static AtariDspOPL *s_instance;
};

#endif

#endif
