/* ScummVM - Graphic Adventure Engine
 * Copyright (C) 2026 Sascha Springer
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BACKENDS_MIXER_ATARI_OPL_STREAM_H
#define BACKENDS_MIXER_ATARI_OPL_STREAM_H

#include "common/scummsys.h"
#include "opl-practical.h"

namespace OPL {
namespace FalconDsp {

// Exact Falcon clock ratios, rather than rounded output-rate constants.
enum { kCodecClock = 25175000, kCodecDivider = 512,
	kPeriodFrames = 768, kPcmFrames = 192, kPcmDivider = 2048,
	kMaxEvents = 2048 };

struct WireEvent {
	uint32 address;
	uint32 value;
};

/** Ordered parameter writes for one DSP period. Overflow is a hard failure. */
class EventBuffer : public OplPractical::Sink {
public:
	EventBuffer() : _count(0), _overflow(false) {}
	void clear() { _count = 0; _overflow = false; }
	void write(uint32 frame, uint16 address, int32 value) override {
		if (_count == kMaxEvents || frame >= kPeriodFrames) {
			_overflow = true;
			return;
		}
		_events[_count].address = OplPractical::packEvent(frame, address);
		_events[_count++].value = (uint32)value & 0xffffff;
	}
	const WireEvent *data() const { return _events; }
	uint count() const { return _count; }
	bool overflowed() const { return _overflow; }

private:
	WireEvent _events[kMaxEvents];
	uint _count;
	bool _overflow;
};

/** Rational PCM sample clock. The source mixer keeps its existing rate. */
class PcmClock {
public:
	PcmClock() : _phase(0), _step(0) {}
	void reset(uint rate) { _phase = 0; _step = rate * kPcmDivider; }
	uint fraction() const { return _phase; }
	uint advance() {
		_phase += _step;
		uint consumed = 0;
		while (_phase >= kCodecClock) {
			_phase -= kCodecClock;
			++consumed;
		}
		return consumed;
	}
	static int32 interpolate(int32 first, int32 second, uint fraction) {
		if (fraction >= kCodecClock)
			return second;
		// Q15 interpolation: at most three 16-bit sample units of error.
		// The 68030 has a widening multiply; a 64-bit divide per PCM sample
		// instead consumed a significant part of the period's CPU budget.
		const uint weight = ((uint64)fraction * 5590366u) >> 32;
		return first + ((second - first) * (int32)weight >> 15);
	}

private:
	uint32 _phase, _step;
};

} // namespace FalconDsp
} // namespace OPL

#endif
