// Exact 32 kHz to Falcon codec interpolation. No instrument or audio data.
// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FCM_LIVE_RESAMPLER_H
#define FCM_LIVE_RESAMPLER_H

#include <stdint.h>

namespace FCM {

class FalconResampler {
	// 32000 / (25175000 / 512) reduces to 16384 / 25175. The complete
	// fractional-phase/advance sequence therefore repeats every 25175 frames.
	// Share its 50350 bytes across voices/instances; generate it once at open,
	// before playback. Low 15 bits reproduce floor(Q16_phase / 2), and the
	// high bit says to consume the next native sample AFTER this output.
	struct Coefficients {
		uint16_t steps[25175];
		Coefficients() {
			unsigned phase = 0;
			for (unsigned i = 0; i < 25175; ++i) {
				unsigned fraction = phase * 32768u / 25175;
				phase += 16384;
				bool advance = phase >= 25175;
				if (advance) phase -= 25175;
				steps[i] = uint16_t(fraction | (advance ? 32768 : 0));
			}
		}
	};
	const uint16_t *_steps = nullptr;
	unsigned _index = 0;
	int16_t _a = 0, _b = 0;

public:
	void open() {
		static const Coefficients coefficients;
		_steps = coefficients.steps;
		_index = 0; _a = _b = 0;
	}
	void prime(int16_t a, int16_t b) { _a = a; _b = b; }

	// Native input is a mono partial; unequal stereo is the probe's diagnostic
	// routing. The caller refills at end, retaining both interpolation samples.
	// No read-ahead past end, and no phase reset at callback/refill boundaries.
	__attribute__((noinline)) unsigned render(int16_t *out, unsigned frames,
	                                        const int16_t *&input, const int16_t *end) {
		if (!frames || input == end) return 0;
		unsigned span = 25175 - _index;
		if (span > frames) span = frames;
		const uint16_t *step = _steps + _index, *limit = step + span;
		const int16_t *cursor = input;
		int32_t a = _a, b = _b;
		do {
			uint16_t coefficient = *step++;
			int32_t sample = a + (((b - a) * int32_t(coefficient & 32767)) >> 15);
			*out++ = int16_t(sample); *out++ = int16_t(sample >> 1);
			if (coefficient & 32768) {
				a = b; b = *cursor++;
				if (cursor == end) break;
			}
		} while (step < limit);
		unsigned count = step - (_steps + _index);
		_index += count;
		if (_index == 25175) _index = 0;
		_a = int16_t(a); _b = int16_t(b); input = cursor;
		return count;
	}
};

} // namespace FCM
#endif
