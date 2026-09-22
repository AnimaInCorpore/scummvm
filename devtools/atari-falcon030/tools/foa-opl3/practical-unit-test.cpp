// Register-level checks of the practical kernel against the exact one: the
// places where the block-rate machine must keep the chip's semantics to the
// word, which the perceptual gate cannot see. Exits nonzero on any failure.
//
//  * pitch: every f-number, block, multiplier, vibrato depth and LFO position
//    gives the exact kernel's increment, aliased as the chip's 19-bit phase
//    aliases it and retimed to the codec rate
//  * sustain level: changing it under a running decay never lowers the
//    attenuation, and the envelope ends where the chip's does
//  * reset: a reset under a held note leaves the machine as a fresh one
//  * attack: zero and maximum rates hold mid-attack, maximum key-on is instant
//  * pause: FM state is frozen and silent, PCM still plays, resume is continuous
//  * rhythm: the lookups the DSP picks the drums' phases with give the exact
//    kernel's phases for every pair of oscillator phases and noise bits; an
//    operator's key is its channel's or its drum's, with the chip's edges;
//    the drums sound at twice a melodic operator's level
//  * waveform select enable: an OPL2's waveforms wait for register 1
//
// usage: opl-practical-unit-test
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "opl-kernel.h"
#include "opl-practical.h"

namespace {

namespace P = OplPractical;
namespace E = OplKernel;

unsigned g_failures = 0;

void fail(const char *what, long a = 0, long b = 0, long c = 0, long d = 0) {
	if (++g_failures <= 20)
		std::fprintf(stderr, "FAIL %s (%ld, %ld, %ld, %ld)\n", what, a, b, c, d);
}

struct Pair {
	E::Chip exact;
	P::Chip practical;
	P::DirectSink sink;
	P::Decoder decoder;
	uint32_t block;

	Pair() : sink(&practical), block(0) {
		E::reset(&exact, 9);
		P::reset(&practical, 9);
		decoder.reset(&sink, 9);
	}
	void write(uint16_t reg, uint8_t value) {
		E::writeRegister(&exact, reg, value);
		decoder.write(block, reg, value);
	}
	void renderBlocks(int blocks) {
		decoder.flush();
		int32_t out[P::kBlockFrames];
		for (int b = 0; b < blocks; ++b, ++block)
			P::renderBlock(&practical, nullptr, out);
	}
};

int32_t retimed(uint32_t nativeIncrement) {
	const uint32_t wrapped = nativeIncrement & 0x7ffff;
	const int32_t aliased = wrapped >= 0x40000 ? (int32_t)wrapped - 0x80000 : (int32_t)wrapped;
	return (int32_t)(((int64_t)aliased * (int64_t)OPL_PRACTICAL_INC_Q16) >> 16);
}

unsigned long checkPitch() {
	unsigned long checked = 0;
	Pair p;
	p.write(0x23, 0x40);   // the carrier of channel 0, vibrato on
	for (int depth = 0; depth < 2; ++depth) {
		p.write(0xbd, (uint8_t)(depth << 6));
		for (int mult = 0; mult < 16; ++mult) {
			p.write(0x23, (uint8_t)(0x40 | mult));
			for (int block = 0; block < 8; ++block) {
				for (int fnum = 0; fnum < 1024; ++fnum) {
					p.write(0xa0, (uint8_t)(fnum & 0xff));
					p.write(0xb0, (uint8_t)((block << 2) | (fnum >> 8)));
					p.decoder.flush();
					const E::Slot &slot = p.exact.slot[3];
					for (int pos = 0; pos < 8; ++pos, ++checked) {
						p.practical.vibratoPos = pos;
						P::opBoundary(&p.practical, 0, 1);
						// an idle operator's boundary pass computes nothing: read the words
						const int32_t *w = p.practical.op[1].w;
						const int32_t inc = P::wrap24((int64_t)w[P::OP_INCBASE] + w[P::kVibratoOffset[pos]]);
						if (inc != retimed(slot.phaseIncVib[pos]))
							fail("vibrato increment", fnum, block, mult, pos);
					}
					if (p.practical.op[1].w[P::OP_INCBASE] != retimed(slot.phaseInc))
						fail("plain increment", fnum, block, mult);
				}
			}
		}
	}
	return checked;
}

// Every sustain level written at several points of a decay, for a slow, a
// medium and a fast decay rate.
unsigned long checkSustainLevel(unsigned long *comparedOut) {
	unsigned long checked = 0, compared = 0;
	static const uint8_t decayRates[3] = { 0x4, 0x8, 0xc };
	for (int rate = 0; rate < 3; ++rate) {
		for (int before = 0; before < 16; before += 5) {
			for (int after = 0; after < 16; ++after) {
				for (int wait = 2; wait <= 200; wait *= 10, ++checked) {
					Pair p;
					p.write(0x23, 0x21);
					p.write(0x63, (uint8_t)(0xf0 | decayRates[rate]));
					p.write(0x83, (uint8_t)((before << 4) | 0x4));
					p.write(0xa0, 0x41);
					p.write(0xb0, 0x32);
					int16_t left, right;
					const int nativePerBlock = 65;   // 64 codec frames, to the nearest native sample
					for (int i = 0; i < wait * nativePerBlock; ++i)
						E::generate(&p.exact, &left, &right);
					p.renderBlocks(wait);
					// The block-rate envelope runs up to a block behind the chip's, so
					// the two can be in different states at the write, or on different
					// sides of the new level; where the write then sends them is not
					// comparable, only that the attenuation never falls.
					const int newLevel = (after == 15 ? 31 : after) << 4;
					const int exactNow = p.exact.slot[3].envRaw;
					const int practicalNow = p.practical.op[1].w[P::OP_ENV] >> 12;
					const int exactSide = exactNow < newLevel ? 0 : exactNow < newLevel + 16 ? 1 : 2;
					const int practicalSide = practicalNow < newLevel ? 0 : practicalNow < newLevel + 16 ? 1 : 2;
					const bool comparable = p.exact.slot[3].envGen == p.practical.op[1].w[P::OP_STATE]
					                        && exactSide == practicalSide;
					p.write(0x83, (uint8_t)((after << 4) | 0x4));
					int32_t previous = p.practical.op[1].w[P::OP_ENV];
					for (int b = 0; b < 4000; ++b) {
						p.renderBlocks(1);
						const int32_t env = p.practical.op[1].w[P::OP_ENV];
						if (env < previous)
							fail("attenuation fell after a sustain level write", rate, before, after, wait);
						previous = env;
					}
					for (int i = 0; i < 4000 * nativePerBlock; ++i)
						E::generate(&p.exact, &left, &right);
					// Both have settled: in sustain at some level, or decayed to silence.
					const int exactEnv = p.exact.slot[3].envRaw;
					const int practicalEnv = previous >> 12;
					compared += comparable;
					if (comparable && abs(exactEnv - practicalEnv) > 16)
						fail("settled envelope differs", exactEnv, practicalEnv, before * 16 + after, wait);
				}
			}
		}
	}
	*comparedOut = compared;
	return checked;
}

unsigned checkAttackChanges() {
	unsigned checked = 0;
	for (int ksr = 0; ksr < 2; ++ksr) {
		for (int block = 0; block < 8; ++block) {
			for (int ar : { 0, 14, 15 }) {
				Pair p;
				p.write(0x23, (uint8_t)(0x21 | (ksr << 4)));
				p.write(0x63, 0x20);
				p.write(0x83, 0x04);
				p.write(0xa0, 0x41);
				const uint8_t key = (uint8_t)(0x21 | (block << 2));
				p.write(0xb0, key);
				int16_t left, right;
				for (int i = 0; i < 130; ++i)
					E::generate(&p.exact, &left, &right);
				p.renderBlocks(2);
				p.write(0x63, (uint8_t)(ar << 4));
				p.decoder.flush();
				const int rate = p.practical.op[1].w[P::OP_RATE_A];
				if (rate && rate < 60)
					continue;
				++checked;
				const int exactEnv = p.exact.slot[3].envRaw;
				const int practicalEnv = p.practical.op[1].w[P::OP_ENV];
				if (!exactEnv || !practicalEnv || p.exact.slot[3].envGen != E::kAttack
				    || p.practical.op[1].w[P::OP_STATE] != P::kAttack)
					fail("attack test did not reach a running attack", ksr, block, ar);
				for (int i = 0; i < 6500; ++i)
					E::generate(&p.exact, &left, &right);
				p.renderBlocks(100);
				if (p.exact.slot[3].envRaw != exactEnv || p.practical.op[1].w[P::OP_ENV] != practicalEnv)
					fail("attack rate change did not hold the envelope", ksr, block, ar);
				if (rate >= 60) {
					p.write(0xb0, key & ~0x20);
					for (int i = 0; i < 65; ++i)
						E::generate(&p.exact, &left, &right);
					p.renderBlocks(1);
					p.write(0xb0, key);
					for (int i = 0; i < 65; ++i)
						E::generate(&p.exact, &left, &right);
					p.renderBlocks(1);
					if (p.exact.slot[3].envRaw || p.practical.op[1].w[P::OP_ENV])
						fail("maximum-rate key-on was not instant", ksr, block, ar);
				}
			}
		}
	}
	return checked;
}

void checkPause() {
	Pair p;
	for (int o : { 0, 3 }) {
		p.write(0x20 + o, 0xe1);
		p.write(0x60 + o, 0xf4);
		p.write(0x80 + o, 0x44);
	}
	p.write(0x40, 0x10);
	p.write(0xc0, 0x06);
	p.write(0xa0, 0x41);
	p.write(0xb0, 0x32);
	p.renderBlocks(30);
	P::Chip continued = p.practical;
	P::poke(&p.practical, P::SC_PAUSED, 1);
	int32_t pcm[P::kBlockFrames], out[P::kBlockFrames], expected[P::kBlockFrames];
	for (int i = 0; i < P::kBlockFrames; ++i)
		pcm[i] = (i - 32) * 128;
	for (int b = 0; b < 100; ++b) {
		P::renderBlock(&p.practical, pcm, out);
		for (int i = 0; i < P::kBlockFrames; ++i)
			if (out[i] != 2 * pcm[i])
				fail("paused FM was not silent or changed PCM", b, i);
	}
	if (memcmp(p.practical.op, continued.op, sizeof(continued.op))
	    || memcmp(p.practical.ch, continued.ch, sizeof(continued.ch))
	    || p.practical.tremoloPhase != continued.tremoloPhase
	    || p.practical.vibratoPhase != continued.vibratoPhase)
		fail("pause advanced FM state");
	P::poke(&p.practical, P::SC_PAUSED, 0);
	bool audible = false;
	for (int b = 0; b < 20; ++b) {
		P::renderBlock(&p.practical, nullptr, out);
		P::renderBlock(&continued, nullptr, expected);
		for (int i = 0; i < P::kBlockFrames; ++i) {
			audible |= expected[i] != 0;
			if (out[i] != expected[i])
				fail("resuming changed the FM output", b, i);
		}
	}
	if (!audible)
		fail("pause test did not resume an audible voice");
}

void checkReset() {
	Pair p;
	p.write(0x01, 0x20);
	p.write(0xbd, 0xc0);
	for (int c = 0; c < 9; ++c) {
		static const uint8_t modOffset[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 };
		for (int which = 0; which < 2; ++which) {
			const uint8_t o = (uint8_t)(modOffset[c] + 3 * which);
			p.write(0x20 + o, 0xe1);
			p.write(0x40 + o, (uint8_t)(which ? 0 : 0x10));
			p.write(0x60 + o, 0xf2);
			p.write(0x80 + o, 0x24);
			p.write(0xe0 + o, (uint8_t)(c & 3));
		}
		p.write(0xc0 + c, (uint8_t)((c & 7) << 1));
		p.write(0xa0 + c, 0x41);
		p.write(0xb0 + c, 0x32);
	}
	p.renderBlocks(100);

	p.decoder.reset(&p.sink, 9, p.block);
	int32_t out[P::kBlockFrames];
	for (int b = 0; b < 8; ++b) {
		P::renderBlock(&p.practical, nullptr, out);
		for (int i = 0; i < P::kBlockFrames; ++i)
			if (out[i])
				fail("output after a reset", b, i, out[i]);
	}

	// Every host-written word and every word that keeps a voice alive equals a fresh machine's.
	P::Chip fresh;
	P::reset(&fresh, 9);
	static const int words[] = {
		P::OP_TRIG, P::OP_TRIGSEEN, P::OP_FLAGS, P::OP_STATE, P::OP_ENV, P::OP_SL, P::OP_RATE_A, P::OP_RATE_D,
		P::OP_RATE_S, P::OP_RATE_R, P::OP_TLKSL, P::OP_INCBASE, P::OP_WFBASE, P::OP_VIBDELTA, P::OP_VIBDELTA + 1,
		P::OP_VIBDELTA + 2, P::OP_VIBDELTA + 3, P::OP_VIBDELTA + 4
	};
	for (int i = 0; i < 18; ++i)
		for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); ++w)
			if (p.practical.op[i].w[words[w]] != fresh.op[i].w[words[w]])
				fail("operator word after a reset", i, words[w], p.practical.op[i].w[words[w]]);
	for (int c = 0; c < 9; ++c)
		if (p.practical.ch[c].w[P::CH_CONN] || p.practical.ch[c].w[P::CH_FBMUL] || p.practical.ch[c].w[P::CH_MODE])
			fail("channel word after a reset", c);
	if (p.practical.tremoloShift != fresh.tremoloShift)
		fail("tremolo depth after a reset");

	// And the decoder starts over: the same song again sounds the same as on a fresh pair.
	Pair q;
	Pair *pairs[2] = { &p, &q };
	int32_t outs[2][P::kBlockFrames];
	for (int n = 0; n < 2; ++n) {
		Pair &r = *pairs[n];
		r.write(0x20, 0x01); r.write(0x23, 0x01);
		r.write(0x40, 0x10); r.write(0x43, 0x00);
		r.write(0x60, 0xf2); r.write(0x63, 0xf2);
		r.write(0x80, 0x24); r.write(0x83, 0x24);
		r.write(0xc0, 0x06);
		r.write(0xa0, 0x41); r.write(0xb0, 0x32);
		r.decoder.flush();
	}
	// the LFO phases differ between the two machines; this patch uses neither
	bool audible = false;
	for (int b = 0; b < 50; ++b) {
		P::renderBlock(&p.practical, nullptr, outs[0]);
		P::renderBlock(&q.practical, nullptr, outs[1]);
		for (int i = 0; i < P::kBlockFrames; ++i) {
			audible |= outs[1][i] != 0;
			if (outs[0][i] != outs[1][i])
				fail("a song after a reset differs from the same song on a fresh machine", b, i);
		}
	}
	if (!audible)
		fail("reset test did not start an audible new song");
}

// The three phase-bit drums, for all 1,024 x 1,024 oscillator phases and both
// noise bits: the phases the practical kernel's lookups select against the
// ones the exact kernel computes.
unsigned long checkRhythmSelection() {
	E::Chip chip;
	E::reset(&chip, 9);
	chip.rhythm = 0x20;
	unsigned long checked = 0;
	for (uint16_t hiHat = 0; hiHat < 1024; ++hiHat) {
		for (uint16_t cymbal = 0; cymbal < 1024; ++cymbal) {
			const int32_t row = P::rhythmHiHatRow(hiHat);
			const int32_t select = P::rhythmSelect(row - P::kRhythmSelect + P::rhythmCymbalColumn(cymbal));
			for (int noise = 0; noise < 4; ++noise, ++checked) {
				chip.noiseHiHat = (uint8_t)(noise >> 1);
				chip.noiseSnare = (uint8_t)(noise & 1);
				E::rhythmPhase(&chip, E::kSlotCymbal, cymbal);
				const uint16_t wantHiHat = E::rhythmPhase(&chip, E::kSlotHiHat, hiHat);
				const uint16_t wantSnare = E::rhythmPhase(&chip, E::kSlotSnare, 0);
				const uint16_t wantCymbal = E::rhythmPhase(&chip, E::kSlotCymbal, cymbal);
				// the drum table's index: the hi-hat's noise bit in bit 3, the snare's in bit 0
				const int index = ((noise >> 1) << 3) + (noise & 1) + select;
				const int pair = index >> 2, snare = index & 3;
				if (P::rhythmPhase(2 * pair) != wantHiHat || P::rhythmPhase(2 * pair + 1) != wantCymbal
				    || P::rhythmPhase(8 + snare) != wantSnare) {
					fail("rhythm phase selection", hiHat, cymbal, noise);
					return checked;
				}
			}
		}
	}
	// The noise generator: the chip's recurrence, s[n + 23] = s[n + 14] ^ s[n].
	int32_t noise = 1;
	uint8_t bits[4096];
	for (int n = 0; n < 4096; ++n) {
		bits[n] = (uint8_t)(noise & 1);
		noise = (noise >> 1) ^ ((noise & 1) ? P::kNoiseTaps : 0);
	}
	for (int n = 0; n + 23 < 4096; ++n)
		if (bits[n + 23] != (bits[n + 14] ^ bits[n])) {
			fail("noise recurrence", n);
			break;
		}
	return checked;
}

// Random writes to the rhythm register and the three channels' key bits:
// each of the six operators is keyed when the chip's slot is, and sees a
// key-on edge when the chip's slot does.
unsigned checkRhythmKeys() {
	static const uint8_t slots[6] = { 12, 15, 13, 16, 14, 17 };   // the chip's, in record order 12..17
	Pair p;
	uint32_t seed = 12345, edges[6] = { 0, 0, 0, 0, 0, 0 };
	unsigned writes = 0;
	for (; writes < 4000; ++writes) {
		seed = seed * 1664525u + 1013904223u;
		const uint8_t value = (uint8_t)(seed >> 16);
		uint8_t before[6];
		for (int i = 0; i < 6; ++i)
			before[i] = p.exact.slot[slots[i]].key;
		if (seed & 0x0300)
			p.write(0xbd, (uint8_t)((value & 0x1f) | ((seed & 0x0c00) ? 0x20 : 0x00)));
		else
			p.write((uint16_t)(0xb6 + (seed >> 12) % 3), (uint8_t)(value & 0x3f));
		p.decoder.flush();
		for (int i = 0; i < 6; ++i) {
			const uint8_t after = p.exact.slot[slots[i]].key;
			edges[i] += after && !before[i];
			const int32_t *w = p.practical.op[12 + i].w;
			if (((w[P::OP_FLAGS] & 1) != 0) != (after != 0))
				fail("rhythm key", writes, i);
			if ((uint32_t)w[P::OP_TRIG] != edges[i])
				fail("rhythm key-on edges", writes, i, w[P::OP_TRIG], edges[i]);
		}
		if ((p.practical.rhythm != 0) != ((p.exact.rhythm & 0x20) != 0))
			fail("rhythm mode", writes);
	}
	p.decoder.reset(&p.sink, 9, p.block);
	if (p.practical.rhythm)
		fail("rhythm mode after a reset");

	// Twice the level: the tom-tom against the same operator as a melodic
	// modulator under the additive connection, both at no attenuation.
	int32_t peaks[2] = { 0, 0 };
	for (int rhythm = 0; rhythm < 2; ++rhythm) {
		Pair q;
		q.write(0x01, 0x20);
		for (uint8_t op = 0x12; op <= 0x15; op += 3) {
			q.write(0x20 + op, 0x21);
			q.write(0x40 + op, 0x00);
			q.write(0x60 + op, op == 0x12 ? 0xf0 : 0x00);   // the carrier never attacks: it stays silent
			q.write(0x80 + op, 0x0f);
		}
		q.write(0xc8, 0x01);
		q.write(0xa8, 0x57);
		q.write(0xb8, (uint8_t)(rhythm ? 0x09 : 0x29));
		q.write(0xbd, (uint8_t)(rhythm ? 0x24 : 0x00));
		q.decoder.flush();
		int32_t out[P::kBlockFrames];
		for (int b = 0; b < 40; ++b) {
			P::renderBlock(&q.practical, nullptr, out);
			for (int i = 0; i < P::kBlockFrames; ++i)
				if (out[i] > peaks[rhythm])
					peaks[rhythm] = out[i];
		}
	}
	if (!peaks[0] || peaks[1] < 2 * peaks[0] - 2 || peaks[1] > 2 * peaks[0] + 2)
		fail("the tom-tom is not twice a melodic operator", peaks[0], peaks[1]);
	return writes;
}

// An OPL2 holds a waveform selection back until register 1 enables it, and
// takes it back when the enable clears; an OPL3 has no such bit.
void checkWaveformEnable() {
	Pair p;
	p.write(0xe0, 0x02);
	p.decoder.flush();
	if (p.practical.op[0].w[P::OP_WFBASE] != P::kWaveBase)
		fail("waveform played before the enable", p.practical.op[0].w[P::OP_WFBASE]);
	p.write(0x01, 0x20);
	p.decoder.flush();
	if (p.practical.op[0].w[P::OP_WFBASE] != P::kWaveBase + 2 * 1024)
		fail("held waveform not played once enabled", p.practical.op[0].w[P::OP_WFBASE]);
	p.write(0x01, 0x00);
	p.decoder.flush();
	if (p.practical.op[0].w[P::OP_WFBASE] != P::kWaveBase)
		fail("waveform played after the enable cleared", p.practical.op[0].w[P::OP_WFBASE]);

	P::Chip chip;
	P::reset(&chip, 18);
	P::DirectSink sink(&chip);
	P::Decoder opl3;
	opl3.reset(&sink, 18);
	opl3.write(0, 0xe0, 0x02);
	opl3.flush();
	if (chip.op[0].w[P::OP_WFBASE] != P::kWaveBase + 2 * 1024)
		fail("an OPL3 has no waveform select enable", chip.op[0].w[P::OP_WFBASE]);
}

} // namespace

int main() {
	const unsigned long pitches = checkPitch();
	unsigned long settled = 0;
	const unsigned long levels = checkSustainLevel(&settled);
	const unsigned attacks = checkAttackChanges();
	checkPause();
	checkReset();
	const unsigned long selections = checkRhythmSelection();
	const unsigned rhythmWrites = checkRhythmKeys();
	checkWaveformEnable();
	std::printf("{\"increments_checked\": %lu, \"sustain_level_cases\": %lu,"
	            " \"sustain_level_cases_settled_alike\": %lu, \"attack_change_cases\": %u,"
	            " \"rhythm_phase_selections\": %lu, \"rhythm_key_writes\": %u, \"failures\": %u}\n",
	            pitches, levels, settled, attacks, selections, rhythmWrites, g_failures);
	return g_failures ? 1 : 0;
}
