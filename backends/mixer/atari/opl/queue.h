/* Copyright (C) 2026 Sascha Springer
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BACKENDS_MIXER_ATARI_OPL_QUEUE_H
#define BACKENDS_MIXER_ATARI_OPL_QUEUE_H

#include "stream.h"

namespace OPL {
namespace FalconDsp {

// One producer (main loop), one consumer (Timer A). Publishing the tail is
// the last operation; a slot stays immutable until the consumer releases it.
class PeriodQueue {
public:
	enum { kSlots = 18, kAhead = 16, kWords = 2 + 2 * kMaxEvents + kPcmFrames };
	struct Period {
		uint32 words[kWords];
		uint count;
		bool speech, audible;
	};
	PeriodQueue() : _head(0), _tail(0) {}
	uint size() const { return (_tail + kSlots - _head) % kSlots; }
	Period *begin() { return size() == kSlots - 1 ? nullptr : &_periods[_tail]; }
	void publish() {
		asm volatile("" ::: "memory");
		_tail = (_tail + 1) % kSlots;
	}
	const Period *front() const { return _head == _tail ? nullptr : &_periods[_head]; }
	void release() {
		asm volatile("" ::: "memory");
		_head = (_head + 1) % kSlots;
	}
	void clear() { _head = _tail = 0; } // Only while delivery is disabled.

private:
	Period _periods[kSlots];
	volatile uint _head, _tail;
};

} // namespace FalconDsp
} // namespace OPL
#endif
