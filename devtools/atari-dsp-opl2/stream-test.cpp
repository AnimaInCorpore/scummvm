/* Copyright (C) 2026 Sascha Springer
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "backends/mixer/atari/opl/stream.h"
#include "backends/mixer/atari/opl/queue.h"
#include <assert.h>
#include <stdio.h>

using namespace OPL::FalconDsp;

int main() {
	// A full queue cannot overwrite the in-flight period; repeated wraps
	// preserve FIFO order while the producer and consumer run at different rates.
	PeriodQueue queue;
	uint published = 0, consumed = 0;
	for (uint cycle = 0; cycle < 10000; ++cycle) {
		while (PeriodQueue::Period *period = queue.begin()) {
			period->count = 1;
			period->words[0] = published++;
			queue.publish();
		}
		assert(queue.size() == PeriodQueue::kSlots - 1);
		for (uint i = 0; i < 3; ++i) {
			assert(queue.front()->words[0] == consumed++);
			queue.release();
		}
	}
	while (queue.front()) {
		assert(queue.front()->words[0] == consumed++);
		queue.release();
	}
	assert(consumed == published && !queue.size());
	queue.clear();
	// Buffer boundaries must never overwrite events or silently shift a write.
	EventBuffer events;
	for (uint i = 0; i < kMaxEvents; ++i)
		events.write(i % kPeriodFrames, 0x400, -1);
	assert(events.count() == kMaxEvents && !events.overflowed());
	assert(events.data()[0].value == 0xffffff);
	events.write(0, 0x400, 0);
	assert(events.overflowed() && events.count() == kMaxEvents);
	events.clear();
	events.write(kPeriodFrames, 0x400, 0);
	assert(events.overflowed() && events.count() == 0);

	// Rational rate conversion must not drift over a long speech stream.
	for (uint rate : { 11025u, 12292u, 22050u, 44100u, 49170u }) {
		PcmClock clock;
		clock.reset(rate);
		const uint outputs = kPcmFrames * 1000;
		uint64 inputs = 0;
		for (uint i = 0; i < outputs; ++i)
			inputs += clock.advance();
		assert(inputs == (uint64)outputs * rate * kPcmDivider / kCodecClock);
	}
	assert(PcmClock::interpolate(-32768, 32767, 0) == -32768);
	assert(PcmClock::interpolate(-32768, 32767, kCodecClock) == 32767);
	// Bound the fixed-point conversion's error against exact interpolation,
	// including full-scale transitions and both signs.
	for (uint fraction = 0; fraction < kCodecClock; fraction += 173) {
		for (int direction : { -1, 1 }) {
			const int first = direction > 0 ? -32768 : 32767;
			const int second = direction > 0 ? 32767 : -32768;
			const int exact = first + (int64)(second - first) * fraction / kCodecClock;
			const int actual = PcmClock::interpolate(first, second, fraction);
			assert(actual >= -32768 && actual <= 32767 && abs(actual - exact) <= 3);
		}
	}

	// Key-on/off writes at distinct sample stamps retain both transitions.
	OplPractical::Decoder decoder;
	events.clear();
	decoder.reset(&events, 9);
	events.clear();
	decoder.write(7, 0xb0, 0x20);
	decoder.write(9, 0xb0, 0);
	decoder.flush();
	bool sawOn = false, sawOff = false;
	for (uint i = 0; i < events.count(); ++i) {
		const uint stamp = events.data()[i].address >> 12;
		sawOn |= stamp == 7;
		sawOff |= stamp == 9;
	}
	assert(sawOn && sawOff && !events.overflowed());
	puts("Falcon OPL transport checks passed");
	return 0;
}
