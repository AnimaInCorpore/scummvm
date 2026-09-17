/* FCM live PCM-partial experiment. SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Wave and amplitude arithmetic adapted from Munt LA32WaveGenerator.cpp,
 * TVA.cpp, TVP.cpp and Partial.cpp:
 * Copyright (C) 2003-2009 Dean Beeler, Jerome Fisher
 * Copyright (C) 2011-2026 Dean Beeler, Jerome Fisher, Sergey V. Mikayev
 * Distributed under the GNU Lesser General Public License, version 2.1 or
 * later; see LICENSES/COPYING.LGPL in the repository root.
 * No Munt runtime dependency.
 */
#ifndef FCM_LIVE_PCM_H
#define FCM_LIVE_PCM_H

#include "runtime.h"
#include <stddef.h>
#include <string.h>

namespace FCM {

// Views borrow the immutable package. Opening validates every section CRC;
// the renderer validates the narrower supported instrument profile at note-on.
struct Bank {
	struct Section { const uint8_t *data = nullptr; uint32_t size = 0; };
	Section inst, parm, part, patch, wave, pcm, ramp, table, system;
	uint16_t exponent[4096] = {}; // Universal exp interpolation, not instrument audio.
	enum { kLinearValues = 131120 };
	const uint16_t *linearTable = nullptr;
	void makeLinearTable(uint16_t *storage) {
		for (unsigned i = 0; i < kLinearValues; ++i)
			storage[i] = i < 65536 ? exponent[i & 4095] >> (i >> 12) : 0;
		linearTable = storage;
	}

	static uint32_t crc32(const uint8_t *p, uint32_t n) {
		uint32_t crc = ~0u;
		for (uint32_t i = 0; i < n; ++i) {
			crc ^= p[i];
			for (unsigned bit = 0; bit < 8; ++bit)
				crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
		}
		return ~crc;
	}

	bool open(const uint8_t *p, uint32_t n) {
		*this = Bank();
		if (!p || n < 12 || n > 16 * 1024 * 1024 || memcmp(p, "FCM1", 4) ||
		    be16(p + 4) != 1 || be32(p + 8) != n) return false;
		uint32_t count = be16(p + 6), end = 12 + count * 16;
		if (!count || count > 64 || end > n) return false;
		Bank candidate;
		bool score = false;
		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t *entry = p + 12 + i * 16;
			uint32_t off = be32(entry + 4), bytes = be32(entry + 8);
			if (off != ((end + 3) & ~3u) || off > n || bytes > n - off) return false;
			for (uint32_t j = 0; j < i; ++j)
				if (!memcmp(entry, p + 12 + j * 16, 4)) return false;
			for (uint32_t j = end; j < off; ++j) if (p[j]) return false;
			if (crc32(p + off, bytes) != be32(entry + 12)) return false;
			const char *tags[] = {"INST", "PARM", "PART", "PTCH", "WAVE", "PCML", "RLUT", "TABL", "SYST"};
			Section *sections[] = {&candidate.inst, &candidate.parm, &candidate.part, &candidate.patch,
			                      &candidate.wave, &candidate.pcm, &candidate.ramp, &candidate.table, &candidate.system};
			for (unsigned j = 0; j < 9; ++j) if (!memcmp(entry, tags[j], 4)) {
				sections[j]->data = p + off; sections[j]->size = bytes;
			}
			if (!memcmp(entry, "SCOR", 4)) score = bytes >= 4;
			end = off + bytes;
		}
		if (end != n || !score || !candidate.inst.size || candidate.inst.size % 22 ||
		    !candidate.parm.size || candidate.parm.size % 58 ||
		    candidate.part.size != candidate.inst.size / 22 * 64 ||
		    candidate.patch.size != 1024 || candidate.wave.size != 512 ||
		    candidate.pcm.size != 524288 || candidate.ramp.size != 1024 ||
		    candidate.table.size != 2615 || candidate.system.size != 23) return false;
		// Tables are trusted only after checking the ranges used by the kernel.
		for (unsigned i = 0; i < 512; ++i)
			if (be16(candidate.table.data + i * 2) > 8191) return false;
		for (unsigned i = 0; i < 256; ++i) {
			if (candidate.table.data[2149 + i] > 128) return false;
			uint32_t step = 0, arg = i & 127;
			if (i) {
				step = 8191 - be16(candidate.table.data + ((~(arg << 6) & 511) * 2));
				step = ((step << (arg >> 3)) + 64) >> 9;
			}
			if (i & 128) ++step;
			if (be32(candidate.ramp.data + i * 4) != step) return false;
		}
		if (candidate.system.data[22] > 100) return false;
		for (unsigned i = 0; i < 4096; ++i) {
			unsigned index = i >> 3, bits = ~i & 7;
			uint16_t b = 8191 - be16(candidate.table.data + index * 2);
			uint16_t a = index ? 8191 - be16(candidate.table.data + (index - 1) * 2) : 8191;
			candidate.exponent[i] = uint16_t(b + (((a - b) * bits) >> 3));
		}
		*this = candidate;
		return true;
	}
};

// One logarithmic ROM oscillator at the MT-32's native 32000 Hz. No waveform
// cache, rendered attack, sustain loop editing, allocation or floating point.
struct PCMWave {
	const uint8_t *words = nullptr, *exp = nullptr;
	const uint16_t *exponent = nullptr;
	const uint16_t *linearTable = nullptr;
	uint32_t length = 0, position = 0;
	bool loop = false, active = false;

	uint16_t interpolateExp(uint16_t fraction) const {
		unsigned index = fraction >> 3, bits = ~fraction & 7;
		uint16_t b = 8191 - be16(exp + index * 2);
		uint16_t a = index ? 8191 - be16(exp + (index - 1) * 2) : 8191;
		return uint16_t(b + (((a - b) * bits) >> 3));
	}

	int16_t linear(uint16_t word, uint32_t amp) const {
		uint32_t value = ((32787u - (word & 32767)) << 1) + (amp >> 10);
		if (value > 65535) value = 65535;
		int16_t sample = interpolateExp(uint16_t(value & 4095)) >> (value >> 12);
		return word & 32768 ? -sample : sample;
	}

	int16_t next(uint32_t amp, uint16_t pitch) {
		if (!active) return 0;
		uint32_t index = position >> 8, fraction = (position & 255) >> 1;
		int32_t a = linear(be16(words + index * 2), amp), b = 0;
		if (index + 1 < length) b = linear(be16(words + (index + 1) * 2), amp);
		else if (loop) b = linear(be16(words), amp);
		uint32_t step = uint32_t(interpolateExp(~pitch & 4095)) << (pitch >> 12);
		position += step >> 9;
		if (position >= length * 256) {
			if (loop) position %= length * 256;
			else { active = false; return 0; } // Munt suppresses the terminal sample.
		}
		return int16_t(a + (((b - a) * int32_t(fraction)) >> 7));
	}

	int16_t fastLinear(uint16_t word, uint32_t attenuation) const {
		int16_t sample = linearTable[attenuation - ((word & 32767) << 1)];
		return word & 32768 ? -sample : sample;
	}
	static uint16_t wordAt(const uint8_t *p) {
		uint16_t value;
		__builtin_memcpy(&value, p, sizeof(value)); // Word load on the big-endian 030.
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		value = __builtin_bswap16(value);
#endif
		return value;
	}

#ifdef __m68k__
	// The interior of a ROM wave needs no wrap/end branch. Keep this loop
	// within the 030 instruction cache; the C++ path below handles boundaries.
	void renderInterior(int16_t *out, unsigned count, uint32_t amp, int32_t delta, uint32_t step) {
		uint32_t pos = position;
		int16_t *end = out + count;
		__asm__ volatile(
			"1:\n\t"
			"move.l %[amp],%%d7\n\t"
			"lsr.l #8,%%d7\n\t"
			"lsr.l #2,%%d7\n\t"
			"add.l #65574,%%d7\n\t"
			"move.l %[pos],%%d4\n\t"
			"lsr.l #8,%%d4\n\t"
			"move.w (%[words],%%d4.l*2),%%d5\n\t"
			"move.w 2(%[words],%%d4.l*2),%%d6\n\t"
			"move.l %%d5,%%d4\n\t"
			"and.l #32767,%%d4\n\t"
			"add.l %%d4,%%d4\n\t"
			"neg.l %%d4\n\t"
			"add.l %%d7,%%d4\n\t"
			"move.w (%[table],%%d4.l*2),%%d4\n\t"
			"tst.w %%d5\n\t"
			"bpl.s 2f\n\t"
			"neg.w %%d4\n\t"
			"2: move.w %%d4,%%d5\n\t"
			"move.l %%d6,%%d4\n\t"
			"and.l #32767,%%d4\n\t"
			"add.l %%d4,%%d4\n\t"
			"neg.l %%d4\n\t"
			"add.l %%d7,%%d4\n\t"
			"move.w (%[table],%%d4.l*2),%%d4\n\t"
			"tst.w %%d6\n\t"
			"bpl.s 3f\n\t"
			"neg.w %%d4\n\t"
			"3: move.w %%d4,%%d6\n\t"
			"sub.w %%d5,%%d6\n\t"
			"moveq #0,%%d4\n\t"
			"move.b %[pos],%%d4\n\t"
			"lsr.w #1,%%d4\n\t"
			"muls.w %%d6,%%d4\n\t"
			"asr.l #7,%%d4\n\t"
			"add.w %%d5,%%d4\n\t"
			"move.w %%d4,(%[out])+\n\t"
			"add.l %[delta],%[amp]\n\t"
			"add.l %[step],%[pos]\n\t"
			"cmpa.l %[end],%[out]\n\t"
			"bne.s 1b\n\t"
			: [out] "+&a" (out), [pos] "+&d" (pos), [amp] "+&d" (amp)
			: [words] "a" (words), [table] "a" (linearTable), [end] "a" (end),
			  [delta] "d" (delta), [step] "d" (step)
			: "d4", "d5", "d6", "d7", "cc", "memory");
		position = pos;
	}
#endif

	// Keep the oscillator loop separate from event/envelope and resampling
	// loops so those paths do not evict it from the 68030's 256-byte I-cache.
	__attribute__((noinline)) void render(int16_t *out, unsigned count, uint32_t amp, int32_t ampDelta, uint16_t pitch) {
		uint32_t step = (uint32_t(exponent[~pitch & 4095]) << (pitch >> 12)) >> 9;
#ifdef __m68k__
		while (count && active) {
			uint32_t edge = (length - 1) * 256;
			uint32_t endEdge = length * 256 - step;
			if (edge > endEdge) edge = endEdge;
			if (position < edge) {
				unsigned span = count;
				uint32_t untilEdge = step ? (edge - position - 1) / step + 1 : count;
				if (span > untilEdge) span = untilEdge;
				renderInterior(out, span, amp, ampDelta, step);
				out += span; count -= span; amp += uint32_t(ampDelta) * span;
			} else { *out++ = next(amp, pitch); --count; amp += ampDelta; }
		}
		memset(out, 0, count * sizeof(*out));
#else
		for (unsigned i = 0; i < count; ++i, amp += ampDelta) {
			if (!active) { out[i] = 0; continue; }
			uint32_t index = position >> 8, fraction = (position & 255) >> 1;
			uint32_t attenuation = 65574 + (amp >> 10);
			int32_t a = fastLinear(wordAt(words + index * 2), attenuation), b = 0;
			if (index + 1 < length) b = fastLinear(wordAt(words + (index + 1) * 2), attenuation);
			else if (loop) b = fastLinear(wordAt(words), attenuation);
			position += step;
			if (position >= length * 256) {
				if (loop) position %= length * 256;
				else { active = false; out[i] = 0; continue; }
			}
			out[i] = int16_t(a + (((b - a) * int32_t(fraction)) >> 7));
		}
#endif
	}
};

// Single melodic partial with normal mixing, flat pitch envelope and no LFO.
// Unsupported instruments fail at note-on. This is deliberately not a MIDI
// device: voice allocation, iMUSE routing, reverb and synthetic LA waves remain
// separate implementation gates. Volume and expression use MT-32 0..100 units.
class PCMVoice {
	const Bank *_bank = nullptr;
	const uint8_t *_p = nullptr, *_patch = nullptr;
	PCMWave _wave;
	Ramp _amp;
	int _phase = 7, _target = 0, _basic = 0, _keyTime = 0;
	uint16_t _pitch = 0, _basePitch = 0;
	uint8_t _key = 0, _velocity = 0;
	bool _noSustain = false;

	static int bias(unsigned point, unsigned level, int key) {
		static const uint8_t factors[] = {255,187,137,100,74,54,40,29,21,15,10,5,0};
		int distance = point & 64 ? key + 31 - int(point) : int(point) + 33 - key;
		return distance > 0 ? (distance * factors[level]) >> 5 : 0;
	}
	void ramp(int target, int increment, int phase) {
		_target = uint8_t(target); _phase = phase;
		_amp.start(uint8_t(target), uint8_t(increment), _bank->ramp.data);
	}
	void nextPhase() {
		int next = _phase + 1;
		if (next == 7) { _phase = 7; return; }
		const uint8_t *tva = _p + 41;
		// Control ROM 1.07 keeps the zero-level envelope quirk.
		bool zero = tva[16] == 0 && next == 4;
		int target = zero ? 0 : _basic, increment = 0;
		if (!zero) {
			if (next == 5 || next == 6) {
				if (!tva[16]) { _phase = 7; return; }
				if (_noSustain) { next = 6; target = 0; increment = tva[12] ? -int(tva[12]) : 1; }
				else target += tva[16];
			} else target += tva[13 + _phase];
		}
		if ((next != 5 && next != 6) || zero) {
			int time = tva[8 + _phase];
			if (next == 1) {
				time -= (int(_velocity) - 64) >> (6 - tva[7]);
				if (time <= 0 && tva[8 + _phase]) time = 1;
			} else time -= _keyTime;
			if (time > 0) {
				int delta = target - _target;
				bool down = delta <= 0;
				if (down) {
					if (!delta) { delta = -1; if (--target < 0) { delta = 1; target = -target; } }
					delta = -delta;
				}
				increment = _bank->table.data[2149 + uint8_t(delta)] - time;
				if (increment <= 0) increment = 1;
				if (down) increment |= 128;
			} else increment = target >= _target ? 255 : 127;
		}
		ramp(target, increment, next);
	}

public:
	bool active() const { return _phase != 7 && _wave.active; }
	uint16_t pitch() const { return _pitch; }
	void stop() { _phase = 7; _wave.active = false; }

	bool noteOn(const Bank &bank, unsigned program, unsigned key, unsigned velocity,
	            unsigned volume = 100, unsigned expression = 100) {
		// Rejection does not disturb an already sounding note.
		if (!bank.patch.data || program >= 128 || key > 127 || !velocity || velocity > 127 ||
		    volume > 100 || expression > 100) return false;
		const uint8_t *patch = bank.patch.data + program * 8;
		if (patch[0] > 1 || patch[1] > 63 || patch[2] > 48 || patch[3] > 100 || patch[4] > 24) return false;
		unsigned instrument = patch[0] * 64 + patch[1];
		if (instrument >= bank.inst.size / 22) return false;
		const uint8_t *inst = bank.inst.data + instrument * 22;
		unsigned mask = inst[12];
		if (!mask || mask > 15 || (mask & (mask - 1)) || inst[13] > 1) return false;
		unsigned slot = 0;
		while (!(mask & (1u << slot))) ++slot;
		const uint8_t *desc = bank.part.data + (instrument * 4 + slot) * 16;
		unsigned param = be16(desc + 6), wave = be16(desc + 8);
		if (be16(desc) != instrument || desc[2] != slot || desc[3] != 2 || desc[4] != 0 ||
		    desc[5] != (slot ^ 1) || be16(desc + 10) != 1 || be16(desc + 12) != inst[13] ||
		    be16(desc + 14) || param != be16(inst + 14 + slot * 2) ||
		    param >= bank.parm.size / 58 || wave >= 128) return false;
		const uint8_t *p = bank.parm.data + param * 58;
		if (p[0] > 96 || p[1] > 100 || p[2] > 16 || p[3] > 1 || p[5] != wave || p[21] != 0 ||
		    p[24] > 30) return false;
		for (unsigned i = 15; i < 20; ++i) if (p[i] != 50) return false;
		const uint8_t *tva = p + 41;
		if (tva[0] > 100 || tva[1] > 100 || tva[2] > 127 || tva[3] > 12 ||
		    tva[4] > 127 || tva[5] > 12 || tva[6] > 4 || tva[7] > 4) return false;
		for (unsigned i = 8; i < 17; ++i) if (tva[i] > 100) return false;
		const uint8_t *w = bank.wave.data + wave * 4;
		uint32_t address = w[0] * 2048u, length = 2048u << ((w[1] >> 4) & 7);
		if (address > bank.pcm.size / 2 || length > bank.pcm.size / 2 - address) return false;

		_bank = &bank; _p = p; _patch = patch; _key = key; _velocity = velocity; _noSustain = inst[13] != 0;
		_wave.words = bank.pcm.data + address * 2; _wave.exp = bank.table.data;
		_wave.exponent = bank.exponent;
		_wave.linearTable = bank.linearTable;
		_wave.length = length; _wave.position = 0; _wave.loop = (w[1] & 128) != 0; _wave.active = true;
		static const int16_t keyfollow[] = {-8192,-4096,-2048,0,1024,2048,3072,4096,5120,6144,7168,8192,10240,12288,16384,8198,8226};
		int distance = int(key) - 60, absDistance = distance < 0 ? -distance : distance;
		int pitch = (absDistance * 4096 + 6) / 12;
		if (distance < 0) pitch = -pitch;
		pitch = (pitch * keyfollow[p[2]]) >> 13;
		pitch += (int(p[0]) - 36) * 4096 / 12 + (int(p[1]) - 50) * 4096 / 1200;
		pitch += (int(patch[2]) - 24) * 4096 / 12 + (int(patch[3]) - 50) * 4096 / 1200;
		pitch += (w[3] << 8) | w[2]; // Original WAVE bytes store LSB first.
		_basePitch = uint16_t(pitch); // GEN0 base-pitch overflow.
		_pitch = _basePitch > 59392 ? 59392 : _basePitch;
		// This first profile uses reset master tuning (440 Hz) and modulation 0.
		_basic = 155;
		const uint8_t *tab = bank.table.data;
		_basic -= tab[2405 + bank.system.data[22]];
		_basic -= tab[2048 + volume] + tab[2048 + expression];
		int biasAmp = bias(tva[2], tva[3], key) + bias(tva[4], tva[5], key);
		_basic -= biasAmp > 255 ? 255 : biasAmp;
		_basic -= tab[2048 + tva[0]];
		// The ROM saturates intermediate subtractions before velocity gain.
		if (_basic < 0) _basic = 0;
		int sensitivity = int(tva[1]) - 50;
		_basic -= (sensitivity < 0 ? -sensitivity : sensitivity) - ((sensitivity * (int(velocity) - 64) * 4) >> 8);
		if (_basic < 0) _basic = 0;
		if (_basic > 155) _basic = 155;
		_basic -= p[24] >> 1;
		if (_basic < 0) _basic = 0;
		_keyTime = tva[6] ? (int(key) - 60) >> (5 - tva[6]) : 0;
		_amp = Ramp();
		ramp(_basic + (tva[8] ? 0 : tva[13]), 255, tva[8] ? 0 : 1);
		return true;
	}

	void noteOff(unsigned key) {
		if (key == _key && active() && _phase < 6)
			ramp(0, _p[53] ? -int(_p[53]) : 1, 6);
	}

	bool bend(unsigned value) {
		if (!_patch || value > 16383) return false;
		int delta = _p[3] ? ((int(value) - 8192) * (_patch[4] * 683)) >> 14 : 0;
		uint16_t pitch = uint16_t(int(_basePitch) + delta);
		_pitch = pitch > 59392 ? 59392 : pitch;
		return true; // Applied at this sample; Munt polls pitch on its MCU timer.
	}

	int16_t next() {
		if (!active()) return 0;
		uint32_t amp = 67117056u - _amp.advance(1);
		if (_amp.checkInterrupt()) nextPhase();
		return _wave.next(amp, _pitch);
	}

	void render(int16_t *out, unsigned count) {
		if (!_wave.linearTable) {
			for (unsigned i = 0; i < count; ++i) out[i] = next();
			return;
		}
		while (count) {
			if (!active()) { memset(out, 0, count * sizeof(*out)); return; }
			unsigned span = count;
			int32_t delta = 0;
			uint32_t amp = 67117056u - _amp.current;
			if (_amp.remaining > 1) {
				if (span >= _amp.remaining) span = _amp.remaining - 1;
				delta = _amp.down ? int32_t(_amp.step) : -int32_t(_amp.step);
				amp += delta;
			} else if (_amp.remaining == 1) {
				// The last ramp sample uses the post-advance value, just like
				// next(). Keep this case separate so steady state stays block-based.
				const uint32_t current = _amp.advance(1);
				_wave.render(out, 1, 67117056u - current, 0, _pitch);
				++out;
				--count;
				if (_amp.checkInterrupt()) nextPhase();
				continue;
			} else if (_amp.delay) {
				if (span > _amp.delay) span = _amp.delay;
			} else if (_amp.step) {
				// Munt reasserts the envelope interrupt every eight steady-state
				// samples. Bound the block so phase transitions remain exact.
				if (span > 8) span = 8;
			}
			_wave.render(out, span, amp, delta, _pitch);
			_amp.advance(span);
			out += span; count -= span;
			if (_amp.checkInterrupt()) nextPhase();
		}
	}
};

} // namespace FCM
#endif
