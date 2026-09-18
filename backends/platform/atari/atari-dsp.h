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
 * it one 768-frame period at a time through the host port.
 *
 * A period is a payload of parameter events for the kernel's operator and
 * channel records, stamped with the block (of 12) they land in, plus 192
 * mono samples at a quarter of the 49,170 Hz codec rate. The DSP acknowledges
 * a payload before rendering it, so the 68030 stays one period ahead; a
 * period that arrives late repeats the previous one and is counted.
 *
 * Production and delivery both run from an MFP Timer A interrupt at about
 * 1 kHz, so neither depends on the game's loop. Delivery takes one
 * protocol step per tick: announce the head period, blast it once the
 * kernel is ready, take the acknowledgement. Production runs whenever
 * fewer than kProduceAhead periods are queued and the interrupted code is
 * outside every critical section (atari-critical.h: no mutex held, not
 * inside the allocator): the handler drops back to the program's
 * interrupt level so delivery and the system's interrupts keep running,
 * switches to its own stack, saves the FPU state, and calls the producer
 * the mixer manager registered. That producer runs the OPL's timer callbacks, which are the
 * AdLib driver and iMUSE's sequencing, and attaches the PCM chunk the
 * main loop mixed ahead. A stall of the game's loop therefore no longer
 * touches the music; it only drains the PCM ring, after which speech and
 * effects are silent until the loop runs again. When the main loop holds
 * a critical section for longer than the queue lasts (a resource load
 * holds SCUMM's resource mutex for over 100 ms), or when a production
 * call itself runs long (iMUSE re-parses a track at a jump, hundreds of
 * milliseconds here), the handler submits extension periods without
 * callbacks instead: the voices carry on and the sequencer slips by a
 * period, where the kernel would otherwise loop the last period it had.
 *
 * The producer runs in interrupt context: it must not do I/O or call the
 * OS. It may allocate, since the allocator counts as a critical section.
 */
class AtariDspAudio {
public:
	enum {
		kCodecRateHz = 49170,        // 25.175 MHz / 256 / 2, rounded: within 1.1% of the chip's own rate
		kPeriodFrames = 768,
		kPeriodBlocks = 12,
		kBlockFrames = 64,
		kPcmPerPeriod = 192,
		kPcmRateHz = 12292,          // a quarter of the codec rate, rounded
		kMaxEvents = 2048,           // the kernel's table holds 4,096; a period never needs half
		kPayloadWords = 1 + 2 * kMaxEvents + 1 + kPcmPerPeriod,
		kProduceAhead = 4,           // periods queued ahead of delivery: 62 ms of tolerance
		kExtendBelow = 2,            // refused production extends while fewer than this are queued
		kPeriods = kProduceAhead + 2 // buffers: the queue, one in flight, one being filled
	};

	struct Period {
		uint32 words[kPayloadWords];   // count, events, PCM flag, samples
		uint32 eventCount;
		volatile bool inFlight;
	};

	/**
	 * Fills one period from the interrupt; false when it could not. With
	 * runCallbacks false it is an extension period: the PCM chunk and the
	 * volume, but no timer callbacks, so the kernel has something to render
	 * while the main loop holds a critical section too long or while a
	 * production call itself runs long. The synth's voices carry on and the
	 * sequencer's timeline slips by one period.
	 */
	typedef bool (*Producer)(void *context, bool runCallbacks);

	AtariDspAudio();
	~AtariDspAudio();

	/** Locks the DSP, boots the kernel and uploads its tables. */
	bool boot();
	/** Routes the DSP's SSI to the DAC and starts the stream. */
	bool startStream();
	void stopStream();
	void shutdown();

	bool isStreaming() const { return _streaming; }

	/** Registers the producer the interrupt calls; nullptr stops production. */
	void setProducer(Producer producer, void *context);

	// The producer's tools, for interrupt context only.
	/**
	 * A period buffer to fill, or nullptr while every buffer is in flight.
	 * An extension is taken by a tick nested inside a production call and
	 * never returns the buffer that call is filling.
	 */
	Period *beginPeriod(bool extension);
	/** Adds a kernel parameter event to the period being filled. */
	/** Adds an event to the period; false, and nothing added, once the period is full. */
	bool addEvent(Period *period, uint32 block, uint16 address, uint32 value);
	/** Sets the period's PCM: 160 signed 16-bit samples, or nullptr for silence. */
	void setPcm(Period *period, const int16 *samples);
	/** Queues the period for delivery. */
	void submit(Period *period);

	/** Refreshes the cached counters from the kernel's last acknowledgement. */
	void poll();

	uint32 periodsSubmitted() const { return _submitted; }
	/** Periods submitted and not yet acknowledged by the kernel. */
	uint32 periodsQueued() const;
	/**
	 * The interrupt's account of production, in 1 ms ticks: the longest
	 * streak of ticks a due period was refused, how many refusals were a
	 * mutex held by the main loop, how many the allocator and how many an
	 * interrupted handler, how many extension periods were submitted, the
	 * longest single production call, and the ticks with nothing queued,
	 * total and longest streak.
	 */
	void productionStats(uint32 &refusedStreakMax, uint32 &refusedMutex, uint32 &refusedAllocator,
	                     uint32 &refusedLevel, uint32 &extended, uint32 &produceMax,
	                     uint32 &emptyTicks, uint32 &emptyStreakMax) const;
	uint32 protocolErrors() const;
	/**
	 * The kernel's counters as of its last acknowledgement: periods rendered, and periods the
	 * transmitter played without a fresh one - caught mid-render, or replayed while no period came.
	 */
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
