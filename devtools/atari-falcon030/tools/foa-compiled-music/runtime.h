// Experimental FCM control runtime. No allocation, floating point or Munt dependency.
#ifndef FALCON_COMPILED_MUSIC_RUNTIME_H
#define FALCON_COMPILED_MUSIC_RUNTIME_H

#include <stdint.h>

namespace FCM {

inline uint16_t be16(const uint8_t *p) {
	return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}
inline uint32_t be32(const uint8_t *p) {
	return (uint32_t(be16(p)) << 16) | be16(p + 2);
}

// A current value plus one affine segment and its delayed completion signal.
// Semantics follow Munt's LA32Ramp (LGPL-2.1-or-later); RLUT compiles the
// increment conversion. Envelope stage selection remains the live controller's
// responsibility. The state is copyable for a save-state adapter.
struct Ramp {
	uint32_t current = 0, target = 0, step = 0, remaining = 0;
	uint8_t delay = 0;
	bool down = false, interrupt = false;

	void start(uint8_t targetByte, uint8_t increment, const uint8_t *lut) {
		target = uint32_t(targetByte) << 18;
		step = be32(lut + uint32_t(increment) * 4);
		down = (increment & 128) != 0;
		delay = 0;
		interrupt = false;
		if (!step) {
			remaining = 0;
		} else if ((down && current <= target) || (!down && current >= target)) {
			remaining = 1;
		} else {
			uint32_t distance = down ? current - target : target - current;
			remaining = (distance - 1) / step + 1;
		}
	}

	uint32_t advance(uint32_t samples) {
		if (remaining && samples) {
			if (samples < remaining) {
				uint32_t delta = step * samples; // bounded by target distance
				current = down ? current - delta : current + delta;
				remaining -= samples;
				return current;
			}
			samples -= remaining;
			remaining = 0;
			current = target;
			delay = 7;
		}
		if (delay && samples) {
			if (samples >= delay) {
				samples -= delay;
				delay = 0;
				interrupt = true;
			} else {
				delay -= uint8_t(samples);
				samples = 0;
			}
		}
		// Munt reasserts completion every eight samples until the controller
		// starts another ramp, even if the previous interrupt was consumed.
		if (step && !remaining && !delay && samples) {
			if (samples >= 8) interrupt = true;
			uint8_t phase = uint8_t(samples % 8);
			if (phase) delay = 8 - phase;
		}
		return current;
	}

	bool checkInterrupt() {
		bool result = interrupt;
		interrupt = false;
		return result;
	}
};

// Each instance belongs to one iMUSE player/selected track. Seeking only finds
// an event position: iMUSE must still scan/reconstruct its controller state and
// handle active notes according to its own jump semantics.
struct Track {
	const uint8_t *cue = 0;
	uint32_t cueBytes = 0, records = 0, count = 0, cursor = 0;

	bool open(const uint8_t *data, uint32_t size, uint16_t index) {
		if (size < 8 || index >= be16(data + 4)) return false;
		uint32_t table = 8u + be16(data + 6);
		uint32_t entry = table + uint32_t(index) * 8;
		if (entry > size || size - entry < 8) return false;
		uint32_t n = be32(data + entry), offset = be32(data + entry + 4);
		if (offset > size || n > (size - offset) / 12) return false;
		cue = data; cueBytes = size; records = offset; count = n; cursor = 0;
		return true;
	}

	const uint8_t *peek() const {
		return cursor < count ? cue + records + cursor * 12 : 0;
	}

	void seek(uint32_t tick) {
		uint32_t lo = 0, hi = count;
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2;
			if (be32(cue + records + mid * 12) < tick) lo = mid + 1;
			else hi = mid;
		}
		cursor = lo;
	}
};

} // namespace FCM
#endif
