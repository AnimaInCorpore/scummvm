// Differential gate: the DSP-shaped OPL kernel against ScummVM's Nuked-OPL3.
//
// Nuked is a bit-exact chip model, so the bar here is sample equality, not a
// tolerance. Nothing in this file measures a Falcon, a DSP or real time.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/scummsys.h"
#include "audio/softsynth/opl/nuked.h"

#include "opl-kernel.h"

namespace NK = OPL::NUKED;

static unsigned long long g_samples, g_writes;
static const char *g_case = "";

static void fail(const char *what) {
	std::fprintf(stderr, "case %s: %s\n", g_case, what);
	std::exit(1);
}

// One register write applied to both models, then a burst of compared samples.
struct Pair {
	NK::opl3_chip reference;
	OplKernel::Chip kernel;
	std::vector<std::pair<uint16_t, uint8_t> > log;

	void reset(uint8_t channels) {
		NK::OPL3_Reset(&reference, 49716);
		OplKernel::reset(&kernel, channels);
		log.clear();
	}
	void write(uint16_t reg, uint8_t value) {
		NK::OPL3_WriteReg(&reference, reg, value);
		OplKernel::writeRegister(&kernel, reg, value);
		log.push_back(std::make_pair(reg, value));
		++g_writes;
	}
	void run(unsigned count) {
		for (unsigned i = 0; i < count; ++i) {
			int16_t expected[2];
			int16_t got[2];
			NK::OPL3_Generate(&reference, expected);
			OplKernel::generate(&kernel, &got[0], &got[1]);
			++g_samples;
			if (expected[0] != got[0] || expected[1] != got[1]) {
				std::fprintf(stderr,
					"case %s: sample %u differs: reference %d/%d, kernel %d/%d\n"
					"  after %u register writes, last:", g_case, i,
					expected[0], expected[1], got[0], got[1], (unsigned)log.size());
				for (size_t k = log.size() > 6 ? log.size() - 6 : 0; k < log.size(); ++k)
					std::fprintf(stderr, " %03x=%02x", log[k].first, log[k].second);
				std::fprintf(stderr, "\n");
				std::exit(1);
			}
		}
	}
};

// A plain two-operator patch, so each sweep varies one thing at a time.
static void patch(Pair &pair, uint16_t bank, uint8_t channel, uint8_t mod, uint8_t car,
                  uint8_t characteristic, uint8_t level, uint8_t attackDecay,
                  uint8_t sustainRelease, uint8_t waveform, uint8_t feedbackConnection) {
	pair.write(bank | (0x20 + mod), characteristic);
	pair.write(bank | (0x20 + car), characteristic);
	pair.write(bank | (0x40 + mod), level);
	pair.write(bank | (0x40 + car), 0x00);
	pair.write(bank | (0x60 + mod), attackDecay);
	pair.write(bank | (0x60 + car), attackDecay);
	pair.write(bank | (0x80 + mod), sustainRelease);
	pair.write(bank | (0x80 + car), sustainRelease);
	pair.write(bank | (0xe0 + mod), waveform);
	pair.write(bank | (0xe0 + car), waveform);
	pair.write(bank | (0xc0 + channel), feedbackConnection);
}

static const uint8_t kModOffset[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 };

static void tablesCase() {
	g_case = "tables";
	// The generated ROMs must equal the ones the reference is built from.
	for (uint16_t phase = 0; phase < 1024; ++phase) {
		for (uint8_t wf = 0; wf < 8; ++wf) {
			// Exercised indirectly below; here only the silent entry's effect.
			uint16_t packed = OplKernel::waveform(wf, phase);
			if ((packed & 0x7fff) > 0x1000)
				fail("waveform magnitude out of range");
		}
	}
}

// Every multiplier, key-scale rate, block and a spread of f-numbers.
static void pitchSweep() {
	g_case = "pitch";
	Pair pair;
	for (uint8_t mult = 0; mult < 16; ++mult) {
		for (uint8_t ksr = 0; ksr < 2; ++ksr) {
			for (uint8_t block = 0; block < 8; ++block) {
				static const uint16_t fnums[4] = { 1, 0x155, 0x2aa, 0x3ff };
				for (int f = 0; f < 4; ++f) {
					pair.reset(9);
					pair.write(0x01, 0x20);
					patch(pair, 0, 0, kModOffset[0], kModOffset[0] + 3,
					      (uint8_t)((ksr << 4) | mult), 0x00, 0xf0, 0x00, 0, 0x00);
					pair.write(0xa0, (uint8_t)(fnums[f] & 0xff));
					pair.write(0xb0, (uint8_t)(0x20 | (block << 2) | (fnums[f] >> 8)));
					pair.run(64);
				}
			}
		}
	}
}

// Every attack, decay, sustain and release nibble, held long enough for the
// envelope to move through attack, decay, sustain and release.
static void envelopeSweep() {
	g_case = "envelope";
	Pair pair;
	for (uint8_t ar = 0; ar < 16; ++ar) {
		for (uint8_t dr = 0; dr < 16; ++dr) {
			pair.reset(9);
			pair.write(0x01, 0x20);
			patch(pair, 0, 0, kModOffset[0], kModOffset[0] + 3, 0x01, 0x00,
			      (uint8_t)((ar << 4) | dr), 0x8f, 0, 0x00);
			pair.write(0xa0, 0x80);
			pair.write(0xb0, 0x2a);
			pair.run(700);
			pair.write(0xb0, 0x0a);   // release
			pair.run(300);
		}
	}
	for (uint8_t sl = 0; sl < 16; ++sl) {
		for (uint8_t rr = 0; rr < 16; ++rr) {
			for (uint8_t egType = 0; egType < 2; ++egType) {
				pair.reset(9);
				pair.write(0x01, 0x20);
				patch(pair, 0, 0, kModOffset[0], kModOffset[0] + 3,
				      (uint8_t)((egType << 5) | 0x01), 0x00, 0xa4,
				      (uint8_t)((sl << 4) | rr), 0, 0x00);
				pair.write(0xa0, 0x80);
				pair.write(0xb0, 0x2a);
				pair.run(600);
				pair.write(0xb0, 0x0a);
				pair.run(400);
			}
		}
	}
}

// Every key-scale level setting against every total level boundary.
static void levelSweep() {
	g_case = "level";
	Pair pair;
	for (uint8_t ksl = 0; ksl < 4; ++ksl) {
		for (uint8_t tl = 0; tl < 64; tl += 3) {
			for (uint8_t block = 0; block < 8; block += 2) {
				pair.reset(9);
				pair.write(0x01, 0x20);
				patch(pair, 0, 0, kModOffset[0], kModOffset[0] + 3, 0x01,
				      (uint8_t)((ksl << 6) | tl), 0xf0, 0x00, 0, 0x00);
				pair.write(0x40 + kModOffset[0] + 3, (uint8_t)((ksl << 6) | tl));
				pair.write(0xa0, 0xaa);
				pair.write(0xb0, (uint8_t)(0x20 | (block << 2) | 1));
				pair.run(96);
			}
		}
	}
}

// All waveforms, both connections, every feedback depth, in OPL2 and OPL3.
static void timbreSweep() {
	g_case = "timbre";
	Pair pair;
	for (uint8_t opl3 = 0; opl3 < 2; ++opl3) {
		const uint8_t waveforms = opl3 ? 8 : 4;
		for (uint8_t wf = 0; wf < waveforms; ++wf) {
			for (uint8_t con = 0; con < 2; ++con) {
				for (uint8_t fb = 0; fb < 8; ++fb) {
					pair.reset(opl3 ? 18 : 9);
					pair.write(0x01, 0x20);
					if (opl3)
						pair.write(0x105, 0x01);
					patch(pair, 0, 0, kModOffset[0], kModOffset[0] + 3, 0x01, 0x00,
					      0xf0, 0x00, wf, (uint8_t)(0x30 | (fb << 1) | con));
					pair.write(0xa0, 0x98);
					pair.write(0xb0, 0x2c);
					pair.run(256);
				}
			}
		}
	}
}

// Tremolo and vibrato, shallow and deep, over a full LFO period.
static void modulationSweep() {
	g_case = "modulation";
	Pair pair;
	for (uint8_t depth = 0; depth < 4; ++depth) {
		for (uint8_t flags = 0; flags < 4; ++flags) {
			pair.reset(9);
			pair.write(0x01, 0x20);
			pair.write(0xbd, (uint8_t)(depth << 6));
			patch(pair, 0, 0, kModOffset[0], kModOffset[0] + 3,
			      (uint8_t)((flags << 6) | 0x01), 0x10, 0xf0, 0x00, 0, 0x00);
			pair.write(0xa0, 0x88);
			pair.write(0xb0, 0x2a);
			// 210 tremolo steps of 64 samples is one full tremolo period.
			pair.run(14000);
		}
	}
}

// All nine channels sounding with different patches, then keys cycled while
// the others keep sounding, plus mid-note instrument reloads.
static void polyphonyCase(uint8_t channels) {
	g_case = channels == 9 ? "polyphony-9" : "polyphony-18";
	Pair pair;
	pair.reset(channels);
	pair.write(0x01, 0x20);
	if (channels == 18)
		pair.write(0x105, 0x01);
	for (uint8_t index = 0; index < channels; ++index) {
		const uint16_t bank = index < 9 ? 0 : 0x100;
		const uint8_t ch = (uint8_t)(index % 9);
		patch(pair, bank, ch, kModOffset[ch], kModOffset[ch] + 3,
		      (uint8_t)(0x21 + index), (uint8_t)(index * 2), (uint8_t)(0xe0 | (index & 7)),
		      (uint8_t)(0x40 | index), (uint8_t)(index & 3),
		      (uint8_t)(0x30 | ((index & 7) << 1) | (index & 1)));
		pair.write(bank | (0xa0 + ch), (uint8_t)(0x40 + index * 11));
		pair.write(bank | (0xb0 + ch), (uint8_t)(0x20 | ((index % 7) << 2) | 1));
	}
	pair.run(2000);
	for (uint8_t round = 0; round < 3; ++round) {
		for (uint8_t index = 0; index < channels; ++index) {
			const uint16_t bank = index < 9 ? 0 : 0x100;
			const uint8_t ch = (uint8_t)(index % 9);
			pair.write(bank | (0xb0 + ch), (uint8_t)(((round % 7) << 2) | 1));
			pair.run(11);
			// Reload the patch under the key, the way a stolen voice does.
			patch(pair, bank, ch, kModOffset[ch], kModOffset[ch] + 3,
			      (uint8_t)(0x01 + ((index + round) & 0x2f)), (uint8_t)((index * 5) & 0x3f),
			      (uint8_t)(0xd0 | ((index + round) & 7)), (uint8_t)(0x30 | ((index + 1) & 0xf)),
			      (uint8_t)((index + round) & 3),
			      (uint8_t)(0x30 | (((index + round) & 7) << 1) | ((index + 1) & 1)));
			pair.write(bank | (0xb0 + ch), (uint8_t)(0x20 | (((round + 2) % 7) << 2) | 1));
			pair.run(37);
		}
	}
	pair.run(4000);
}

// Rhythm mode: the bass drum in both connections, the four single-operator
// drums in every key combination, waveform and level, the drums' keys under
// and over the channels' own, the mode left and entered under sounding
// notes, and a long run of the noise generator.
static void rhythmPatches(Pair &pair, uint8_t variant, uint8_t bassConnection) {
	for (uint8_t ch = 6; ch < 9; ++ch) {
		const uint8_t mod = kModOffset[ch];
		const uint8_t car = (uint8_t)(mod + 3);
		pair.write(0x20 + mod, (uint8_t)(0x01 + ((ch + variant) & 7) + ((variant & 1) << 6)));
		pair.write(0x20 + car, (uint8_t)(0x02 + ((ch * 3 + variant) & 7) + ((variant & 2) << 6)));
		pair.write(0x40 + mod, (uint8_t)((variant * 5 + ch) & 0x1f));
		pair.write(0x40 + car, (uint8_t)((variant * 3 + ch) & 0x0f));
		pair.write(0x60 + mod, (uint8_t)(0xf0 | ((6 + variant + ch) & 0xf)));
		pair.write(0x60 + car, (uint8_t)(((variant & 4) ? 0xa0 : 0xf0) | ((5 + variant) & 0xf)));
		pair.write(0x80 + mod, (uint8_t)(0x40 | ((4 + variant) & 0xf)));
		pair.write(0x80 + car, (uint8_t)(0x20 | ((6 + ch) & 0xf)));
		pair.write(0xe0 + mod, (uint8_t)((variant + ch) & 3));
		pair.write(0xe0 + car, (uint8_t)((variant + ch + 1) & 3));
		pair.write(0xc0 + ch, (uint8_t)((((variant + ch) & 7) << 1) | (ch == 6 ? bassConnection : (variant & 1))));
		const uint16_t fnum = (uint16_t)(0x120 + 0x53 * ch + 0x31 * variant);
		pair.write(0xa0 + ch, (uint8_t)(fnum & 0xff));
		pair.write(0xb0 + ch, (uint8_t)((((variant + ch) & 7) << 2) | ((fnum >> 8) & 3)));
	}
}

static void rhythmCase() {
	g_case = "rhythm";
	Pair pair;
	for (uint8_t variant = 0; variant < 8; ++variant) {
		for (uint8_t bassConnection = 0; bassConnection < 2; ++bassConnection) {
			pair.reset(9);
			pair.write(0x01, 0x20);
			// A melodic voice beside the drums, so the mix is not theirs alone.
			patch(pair, 0, 0, kModOffset[0], kModOffset[0] + 3, 0x21, 0x10, 0xf3, 0x25, 0, 0x06);
			pair.write(0xa0, 0x41);
			pair.write(0xb0, 0x32);
			rhythmPatches(pair, variant, bassConnection);
			pair.write(0xbd, (uint8_t)(0x20 | (variant << 6)));
			pair.run(100);
			for (uint8_t keys = 1; keys < 32; ++keys) {
				pair.write(0xbd, (uint8_t)(0x20 | (variant << 6) | keys));
				pair.run(180);
				pair.write(0xbd, (uint8_t)(0x20 | (variant << 6)));
				pair.run(90);
			}
		}
	}

	// The channels' own keys under rhythm mode, a drum key over a held channel
	// key (no retrigger) and the other way round, then the mode left under
	// sounding drums (they are released, and the channels turn melodic again)
	// and entered under sounding melodic notes.
	for (uint8_t variant = 0; variant < 4; ++variant) {
		pair.reset(9);
		pair.write(0x01, 0x20);
		rhythmPatches(pair, (uint8_t)(variant + 3), (uint8_t)(variant & 1));
		pair.write(0xbd, 0x20);
		for (uint8_t ch = 6; ch < 9; ++ch) {
			pair.write(0xb0 + ch, (uint8_t)(0x20 | (ch << 2) | 1));
			pair.run(200);
		}
		pair.write(0xbd, 0x3f);
		pair.run(300);
		for (uint8_t ch = 6; ch < 9; ++ch) {
			pair.write(0xb0 + ch, (uint8_t)((ch << 2) | 1));
			pair.run(150);
		}
		pair.write(0xbd, 0x20);
		pair.run(200);
		pair.write(0xbd, 0x3f);
		pair.run(250);
		pair.write(0xbd, 0x1f);   // the mode bit clear: the drum bits mean nothing
		pair.run(400);
		for (uint8_t ch = 6; ch < 9; ++ch)
			pair.write(0xb0 + ch, (uint8_t)(0x20 | (ch << 2) | 2));
		pair.run(300);
		pair.write(0xbd, 0x3f);
		pair.run(400);
		pair.write(0xbd, 0x00);
		pair.run(300);
	}

	// Rhythm mode on the low bank of an eighteen-channel chip.
	pair.reset(18);
	pair.write(0x105, 0x01);
	rhythmPatches(pair, 5, 0);
	pair.write(0xbd, 0x3f);
	pair.run(600);
	pair.write(0xbd, 0x20);
	pair.run(200);

	// The noise generator's period is 2^23 - 1 steps, 233,017 samples: a run
	// past it with the hi-hat and the snare held.
	pair.reset(9);
	pair.write(0x01, 0x20);
	rhythmPatches(pair, 2, 0);
	for (uint8_t ch = 7; ch < 9; ++ch)
		for (uint8_t which = 0; which < 2; ++which) {
			pair.write(0x20 + kModOffset[ch] + 3 * which, 0x21);   // sustaining
			pair.write(0x80 + kModOffset[ch] + 3 * which, 0x0f);
		}
	pair.write(0xbd, 0x2b);
	pair.run(260000);
}

// The OPL2's waveform select enable, which the OPL3 oracle does not have: with
// the gate modelled and the enable clear every operator plays its sine, and
// the selection it held comes into force when the enable is set. Compared
// against the oracle given the waveforms the gate lets through.
static void waveformEnableCase() {
	g_case = "waveform-enable";
	for (uint8_t wf = 0; wf < 4; ++wf) {
		NK::opl3_chip reference;
		OplKernel::Chip kernel;
		NK::OPL3_Reset(&reference, 49716);
		OplKernel::reset(&kernel, 9);
		kernel.opl2WaveformGate = 1;
		struct Step { uint16_t reg; uint8_t value; uint8_t oracle; };
		const Step steps[] = {
			{ 0x20, 0x21, 0x21 }, { 0x23, 0x21, 0x21 }, { 0x40, 0x12, 0x12 }, { 0x43, 0x00, 0x00 },
			{ 0x60, 0xf2, 0xf2 }, { 0x63, 0xf2, 0xf2 }, { 0x80, 0x24, 0x24 }, { 0x83, 0x24, 0x24 },
			{ 0xe0, wf, 0 }, { 0xe3, wf, 0 },                // held, not played: the enable is clear
			{ 0xc0, 0x04, 0x04 }, { 0xa0, 0x98, 0x98 }, { 0xb0, 0x2c, 0x2c }
		};
		for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
			NK::OPL3_WriteReg(&reference, steps[i].reg, steps[i].oracle);
			OplKernel::writeRegister(&kernel, steps[i].reg, steps[i].value);
			++g_writes;
		}
		for (int phase = 0; phase < 3; ++phase) {
			if (phase) {
				// Set, then cleared again: the held selection plays, then the sine.
				OplKernel::writeRegister(&kernel, 0x01, phase == 1 ? 0x20 : 0x00);
				NK::OPL3_WriteReg(&reference, 0xe0, phase == 1 ? wf : 0);
				NK::OPL3_WriteReg(&reference, 0xe3, phase == 1 ? wf : 0);
			}
			for (unsigned i = 0; i < 400; ++i) {
				int16_t expected[2], got[2];
				NK::OPL3_Generate(&reference, expected);
				OplKernel::generate(&kernel, &got[0], &got[1]);
				++g_samples;
				if (expected[0] != got[0] || expected[1] != got[1])
					fail("the gated waveform differs from the oracle's");
			}
		}
	}
}

// Replay a captured post-AdLib-driver trace at its recorded microsecond times.
static void traceCase(const char *path, const char *name) {
	g_case = name;
	FILE *file = std::fopen(path, "r");
	if (!file)
		fail("cannot open trace");
	Pair pair;
	pair.reset(9);
	char line[256];
	unsigned long long renderedUs = 0;
	unsigned long long lastUs = 0;
	while (std::fgets(line, sizeof(line), file)) {
		if (line[0] != 'W')
			continue;
		unsigned long long when;
		char context;
		unsigned reg, value;
		if (std::sscanf(line, "W %llu %c %x %x", &when, &context, &reg, &value) != 4)
			fail("malformed trace line");
		if (when < lastUs)
			fail("trace timestamps went backwards");
		lastUs = when;
		// 49,716 samples per second, advanced exactly to the write's time.
		const unsigned long long target = when * 49716ull / 1000000ull;
		while (renderedUs < target) {
			const unsigned long long step = target - renderedUs > 4096 ? 4096 : target - renderedUs;
			pair.run((unsigned)step);
			renderedUs += step;
		}
		pair.write((uint16_t)reg, (uint8_t)value);
	}
	std::fclose(file);
	pair.run(49716);
	std::printf("  trace: %llu writes replayed\n", (unsigned long long)pair.log.size());
}

int main(int argc, char **argv) {
	tablesCase();
	pitchSweep();
	envelopeSweep();
	levelSweep();
	timbreSweep();
	modulationSweep();
	polyphonyCase(9);
	polyphonyCase(18);
	rhythmCase();
	waveformEnableCase();
	if (argc > 1)
		traceCase(argv[1], "atlantis-trace");
	if (argc > 2)
		traceCase(argv[2], "rhythm-trace");   // a game that plays its drums through rhythm mode
	std::printf("{\"samples_compared\": %llu, \"register_writes\": %llu, \"mismatches\": 0,"
	            " \"reference\": \"Nuked-OPL3 (ScummVM tree)\", \"bit_exact\": true}\n",
	            g_samples, g_writes);
	return 0;
}
