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

#ifndef BACKENDS_PLATFORM_ATARI_DSP_H
#define BACKENDS_PLATFORM_ATARI_DSP_H

#ifdef ATARI_DSP_OPL

#include "common/scummsys.h"

/**
 * The Falcon030 DSP as the audio output: the practical OPL kernel of
 * devtools/atari-falcon030/tools/foa-opl3 owns the codec, synthesizes the
 * AdLib voices in blocks and adds the host's PCM mix, and the 68030 feeds
 * it one 480-frame period at a time through the host port.
 *
 * A period is a payload of parameter events for the kernel's operator and
 * channel records, stamped with the block (of 15) they land in, plus 160
 * mono samples at a third of the 32,780 Hz codec rate. The DSP acknowledges
 * a payload before rendering it, so the 68030 stays one period ahead; a
 * period that arrives late repeats the previous one and is counted.
 *
 * Delivery runs from an MFP Timer A interrupt at about 1 kHz: announce,
 * wait for the kernel's READY, blast the words paced on the port, take the
 * acknowledgement. The main loop only produces periods, up to sixteen
 * ahead, so a stall in the game's loop of up to about 230 ms costs no
 * period; a longer one repeats a period per 14.6 ms it lasts.
 */
class AtariDspAudio {
public:
	enum {
		kCodecRateHz = 32780,        // 25.175 MHz / 4 / 192, rounded
		kPeriodFrames = 480,
		kPeriodBlocks = 15,
		kBlockFrames = 32,
		kPcmPerPeriod = 160,
		kPcmRateHz = 10927,          // a third of the codec rate, rounded
		kMaxEvents = 2048,           // the kernel's table holds 4,096; a period never needs half
		kPayloadWords = 1 + 2 * kMaxEvents + 1 + kPcmPerPeriod,
		kPeriods = 16                // produced ahead: 234 ms of audio, 264 KB
	};

	struct Period {
		uint32 words[kPayloadWords];   // count, events, PCM flag, samples
		uint32 eventCount;
		volatile bool inFlight;
	};

	AtariDspAudio();
	~AtariDspAudio();

	/** Locks the DSP, boots the kernel and uploads its tables. */
	bool boot();
	/** Routes the DSP's SSI to the DAC and starts the stream. */
	bool startStream();
	void stopStream();
	void shutdown();

	bool isStreaming() const { return _streaming; }

	/** A period buffer to fill, or nullptr while every buffer is in flight. */
	Period *beginPeriod();
	/** Adds a kernel parameter event to the period being filled. */
	void addEvent(Period *period, uint32 block, uint16 address, uint32 value);
	/** Sets the period's PCM: 160 signed 16-bit samples, or nullptr for silence. */
	void setPcm(Period *period, const int16 *samples);
	/** Queues the period for delivery. */
	void submit(Period *period);
	/** Delivery runs from the interrupt; this only refreshes the cached counters. */
	void poll();

	uint32 periodsSubmitted() const { return _submitted; }
	uint32 protocolErrors() const;
	/** The kernel's counters, periods rendered and periods rendered late, as of its last acknowledgement. */
	bool queryCounters(uint32 &rendered, uint32 &late);

private:
	bool exchange(uint32 command, uint32 &reply);
	bool uploadWords(bool ySpace, uint32 address, const uint32 *words, uint32 count);
	bool uploadTables();

	Period _periods[kPeriods];
	int _filling;
	bool _booted;
	bool _streaming;
	uint32 _submitted;
	uint32 _rendered;
	uint32 _late;
};

#endif

#endif
