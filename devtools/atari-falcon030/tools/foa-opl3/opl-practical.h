// Practical (approximate) two-operator OPL kernel for the Falcon DSP56001,
// as a host reference.
//
// The exact kernel in opl-kernel.h costs about twice the DSP's budget because
// the chip's envelope advances every sample. This kernel gives that up: it
// renders at the Falcon codec's 49,169.92 Hz in operator-major blocks of
// 64 frames, advances every envelope and the LFO once per block, and applies
// register writes at block boundaries. Everything else keeps the chip's
// arithmetic: the 1,024-step waveforms, the log-domain envelope in the same
// 0.1875 dB units, the feedback and modulation depth, f-number pitch.
//
// Rhythm mode is the chip's too, block-rate like the rest: the bass drum and
// the tom-tom are ordinary operators at twice the level, and the hi-hat, the
// snare and the cymbal, which play a few fixed phases picked by two
// oscillators' phase bits and the noise, are one table of sixteen sums a
// block and one lookup a frame. The noise generator has the chip's
// polynomial, stepped twice a frame where the chip steps it 36 times.
//
// Two halves share this file:
//
//  * Decoder: the register-level side, which runs on the 68030. It keeps the
//    chip's register image and turns each write into parameter words for the
//    DSP's operator and channel records, emitted only when they change.
//  * Chip: the DSP side, written as the memory-image machine dsp/oplrt.asm
//    implements, in the arithmetic that assembly executes (24-bit words,
//    48-bit phase, mpy truncation), so its frames can be compared word for
//    word against the DSP's.
//
// The quality of the approximation is scored by practical-test.cpp and
// practical-gate.py against the exact kernel; the DSP's exactness against
// this reference by rt-bench-gate.py.
#ifndef FOA_OPL_PRACTICAL_H
#define FOA_OPL_PRACTICAL_H

#include <stdint.h>
#include <string.h>

#include "opl-kernel.h"
#include "opl-practical-tables.h"

namespace OplPractical {

enum { kChannels = 18, kSlots = 36, kBlockFrames = OPL_PRACTICAL_BLOCK_FRAMES, kWaveforms = 4 };

// ------------------------------------------------- DSP memory layout (words)
//
// Shared with dsp/oplrt.asm and the bench fixture. External X holds the
// records and the gain table; external Y holds the phase low words (as the
// L-space partner of each record's first word) and the waveform tables.
enum {
	kScalarBase = 0x0000,        // internal X scalars the host may write
	kGainTable = 0x0200,         // X external, 512 words, 2^(-envOut/32)
	kOpBase = 0x0400,            // X external operator records
	kOpStride = 32,
	kChannelBase = 0x0900,       // X external channel records
	kChannelStride = 8,
	kAttackTable = 0x0a00,       // X external, 64 words
	kDecayTable = 0x0a40,        // X external, 64 words
	kVibratoTable = 0x0a80,      // X external, 8 words: the record offset of each LFO position's delta
	kRtParams = 0x0b00,          // X external, 4 words per operator: INC, GAIN, GAINMOD, WFBASE
	kRhythmSelect = 0x0880,      // X external, 32 words: drum table offset by phase bits
	kRhythmPhases = 0x08a0,      // X external, 12 words: the phases the three phase-bit drums play
	kRhythmCymbal = 0x0c00,      // Y external, 1,024 words: select column by the cymbal's phase
	kRhythmHiHat = 0x1c00,       // X external, 1,024 words: select row address by the hi-hat's phase
	kRhythmDrumTable = 0x00a0,   // Y internal, 16 words: this block's drum sums
	kSsiBufferA = 0x1000,        // X external, 1,024 interleaved words (production)
	kSsiBufferB = 0x1800,
	kWaveBase = 0x1000,          // Y external, 1,024 words per waveform
	kFrameBase = 0x2000,         // X external, bench output, 4,096 words
	kFrameLimit = 0x3000,
	kEventBase = 0x3000,         // X external, bench events: (block << 16 | address), value
	kEventLimit = 0x4000
};

// Operator record, in the order the DSP's block-boundary pass walks it.
// The host writes TRIG through WFBASE except STATE and ENV; the DSP owns
// the rest.
enum {
	OP_PHASE = 0,        // L word: X holds the integer index, Y the fraction
	OP_TRIG = 1,         // host: key-on edge count
	OP_TRIGSEEN = 2,     // last applied key-on count
	OP_FLAGS = 3,        // host: bit 0 key, bit 1 tremolo, bit 2 vibrato
	OP_STATE = 4,        // 0 attack, 1 decay, 2 sustain, 3 release
	OP_ENV = 5,          // attenuation units << 12
	OP_SL = 6,           // host: sustain level << 4 << 12
	OP_RATE_A = 7,       // host: effective 6-bit rates, 0 = hold
	OP_RATE_D = 8,
	OP_RATE_S = 9,
	OP_RATE_R = 10,
	OP_TLKSL = 11,       // host: (tl << 2) + scaled key-scale level
	OP_INCBASE = 12,     // host: increment without vibrato
	OP_WFBASE = 14,      // host: waveform table base in Y
	OP_GAIN = 15,        // gain of a mix-bound product, 24-bit fraction of half the gain
	OP_GAINMOD = 16,     // gain of a modulation-ring product, gain >> 7
	OP_GAINFB = 17,      // gain of a feedback-history product
	OP_INC = 18,         // phase increment for this block (x1 of the mac)
	OP_VIBDELTA = 19     // host: five words, the increment's offset at each distinct
	                     // vibrato displacement: none (always zero), +half, +full, -half, -full
};

// The record offset the vibrato LFO selects at each of its eight positions.
// The chip displaces the f-number by integer shifts of its top three bits
// before the block and multiplier apply, so the five displaced increments
// are the decoder's to compute; the depth bit is folded in there too.
static const int32_t kVibratoOffset[8] = {
	OP_VIBDELTA, OP_VIBDELTA + 1, OP_VIBDELTA + 2, OP_VIBDELTA + 1,
	OP_VIBDELTA, OP_VIBDELTA + 3, OP_VIBDELTA + 4, OP_VIBDELTA + 3
};

enum { CH_MODE = 0, CH_CONN = 1, CH_FBMUL = 2 };   // FBMUL = 2^(7 + fb), 0 for no feedback

// Scalars in internal X the host writes.
enum {
	SC_TREMOLO_SHIFT = 0x0010, SC_PAUSED = 0x0011, SC_CHANNELS = 0x0012, SC_MASTER_GAIN = 0x0013,
	SC_RHYTHM = 0x0030           // nonzero: channels six to eight are the rhythm section
};

enum EnvelopeState { kAttack = 0, kDecay = 1, kSustain = 2, kRelease = 3 };

enum ChannelMode {
	kModeSkip = 0, kModeCarrierOnly = 1, kModeFmFeedback = 2, kModeFmPlain = 3,
	kModeAddFeedback = 4, kModeAddPlain = 5, kModeModOnlyFeedback = 6, kModeModOnlyPlain = 7,
	// rhythm mode: the bass drum's three shapes, the tom-tom, and channel
	// seven standing for the hi-hat, the snare and the cymbal together
	kModeBassCarrier = 8, kModeBassFmFeedback = 9, kModeBassFmPlain = 10, kModeTom = 11,
	kModeDrums = 12, kModeDrumsSilent = 13
};

// The rhythm section's operators, by record index.
enum {
	kOpBassModulator = 12, kOpBassCarrier = 13, kOpHiHat = 14, kOpSnare = 15, kOpTom = 16, kOpCymbal = 17,
	kChannelBass = 6, kChannelDrums = 7, kChannelTom = 8
};

// The noise generator: the chip's x^23 + x^14 + 1 as a right-shifting Galois
// register, which one shift and a conditional exclusive-or step.
enum { kNoiseTaps = 0x400100, kNoiseBits = 0x000009 };

// ------------------------------------------------------ DSP arithmetic

// The data ALU multiplies two 24-bit operands into a 48-bit product shifted
// left once; A1 is the high word. For an integer times a fraction that is
// the integer part of the scaled value, truncated toward minus infinity.
static inline int32_t mpyHi(int32_t x, int32_t y) {
	return (int32_t)(((int64_t)x * (int64_t)y * 2) >> 24);
}

// mpyr: the same product rounded to nearest, a tie to even (the DSP's
// convergent rounding), which leaves A0 clear.
static inline int32_t mpyrHi(int32_t x, int32_t y) {
	int64_t p = (int64_t)x * (int64_t)y * 2 + 0x800000;
	if (!(p & 0xffffff))
		p &= ~(int64_t)0x1000000;
	return (int32_t)(p >> 24);
}

static inline int32_t clamp24(int64_t v) {
	if (v > 0x7fffff)
		return 0x7fffff;
	if (v < -0x800000)
		return -0x800000;
	return (int32_t)v;
}

static inline int32_t wrap24(int64_t v) {
	v &= 0xffffff;
	return (int32_t)(v >= 0x800000 ? v - 0x1000000 : v);
}

// A linear waveform sample: the exact chip's output for this phase at zero
// attenuation, times 256, against gains of half their value (kOplGain), so
// that no attenuation is the fraction 0.5 exactly. A 24-bit fraction cannot
// hold 1.0, and 0.999... truncates every positive product one short: at
// full level the modulator would then index the carrier one table step low
// through its positive half. A mix-bound product is sample * 128 * gain, so
// a mix of nine channels stays inside 24 bits and one arithmetic shift left
// lands the 16-bit result in the top of the word; a modulation-ring product
// needs the chip's own units (gain >> 7), and a feedback product the chip's
// (out0 + out1) >> (9 - fb) depth, gain * 2^(fb - 16). The products
// truncate toward minus infinity, as the chip's shifted exponential and
// its one's complement negation do.
static inline int32_t waveSample(uint8_t wf, uint16_t phase) {
	const uint16_t packed = OplKernel::waveform(wf, phase);
	const int32_t negate = (packed & 0x8000) ? -1 : 0;
	uint32_t level = packed & 0x7fff;
	if (level > 0x1fff)
		level = 0x1fff;
	const int32_t out = (int32_t)(kOplExp[level & 0xff] >> (level >> 8)) ^ negate;
	return out * 256;
}

// ---------------------------------------------------- rhythm mode's tables
//
// On the chip the hi-hat, the snare and the cymbal do not play their own
// phase. Bits 2, 3, 7 and 8 of the hi-hat's and bits 3 and 5 of the cymbal's
// combine into one bit, and with the noise that picks each drum's phase from
// two or four fixed ones. The host uploads the combination as lookups: the
// hi-hat's phase gives a row of the select table, the cymbal's a column, and
// the entry is the drum table offset of the combined bit and hi-hat bit 8.

static inline int32_t rhythmHiHatRow(uint16_t phase) {
	const int32_t code = (((phase >> 2) ^ (phase >> 7)) & 1) | (((phase >> 3) & 1) << 1) | (((phase >> 8) & 1) << 2);
	return kRhythmSelect + 4 * code;
}

static inline int32_t rhythmCymbalColumn(uint16_t phase) {
	return ((phase >> 3) & 1) | (((phase >> 5) & 1) << 1);
}

// Entry [4 * row code + column]: (combined << 2) | (hi-hat bit 8 << 1).
static inline int32_t rhythmSelect(int index) {
	const int32_t a = index >> 2 & 1, hiHat3 = index >> 3 & 1, hiHat8 = index >> 4 & 1;
	const int32_t cymbal3 = index & 1, cymbal5 = index >> 1 & 1;
	const int32_t combined = a | (hiHat3 ^ cymbal5) | (cymbal3 ^ cymbal5);
	return (combined << 2) | (hiHat8 << 1);
}

// The phases, in the order the drum table is built: the hi-hat's and the
// cymbal's in pairs by (noise << 1 | combined), then the snare's by
// (hi-hat bit 8 << 1 | noise).
static inline int32_t rhythmPhase(int index) {
	if (index >= 8) {
		const int32_t hiHat8 = (index - 8) >> 1, noise = (index - 8) & 1;
		return (hiHat8 << 9) | ((hiHat8 ^ noise) << 8);
	}
	const int32_t noise = index >> 2 & 1, combined = index >> 1 & 1;
	if (index & 1)
		return (combined << 9) | 0x80;
	return (combined << 9) | ((combined ^ noise) ? 0xd0 : 0x34);
}

// ------------------------------------------------------------- the machine

struct Op {
	int32_t w[kOpStride];
	uint64_t phase;   // 48-bit index.fraction, the X:Y pair of OP_PHASE
};

struct Channel {
	int32_t w[kChannelStride];
	int32_t hist0;    // older feedback product, internal Y
	int32_t hist1;    // newer feedback product, internal X
};

struct Chip {
	Op op[kSlots];
	Channel ch[kChannels];
	int32_t modRing[kBlockFrames];
	int32_t mixRing[kBlockFrames];
	int32_t tremoloPhase, vibratoPhase;
	int32_t tremolo;      // this block's tremolo attenuation
	int32_t vibratoPos;
	int32_t tremoloShift, channels;
	int32_t masterGain;   // fraction applied to the FM mix before the PCM
	int32_t paused;       // transport-owned: freeze and silence FM, still mix PCM
	int32_t rhythm;       // host: channels six to eight are the rhythm section
	int32_t noise;        // the noise generator's 23 bits
	int32_t hiHatInc, cymbalInc;   // this block's increments of the two phase-bit oscillators
	int32_t drumTable[16];         // this block's drum sums, by (noise, combined, bit 8, noise)
	uint32_t block;
};

static inline int slotOfChannel(int channel, int which) { return 2 * channel + which; }

static inline void reset(Chip *chip, int channels) {
	memset(chip, 0, sizeof(*chip));
	chip->channels = channels;
	chip->tremoloShift = 4;
	chip->masterGain = 0x7fffff;
	chip->noise = 1;
	for (int i = 0; i < kSlots; ++i) {
		chip->op[i].w[OP_STATE] = kRelease;
		chip->op[i].w[OP_ENV] = 0x1ff << 12;
		chip->op[i].w[OP_WFBASE] = kWaveBase;
	}
}

// A host word landing in the DSP's X memory image.
static inline void poke(Chip *chip, uint16_t address, int32_t value) {
	if (address >= kOpBase && address < kOpBase + kSlots * kOpStride) {
		Op &op = chip->op[(address - kOpBase) / kOpStride];
		op.w[(address - kOpBase) % kOpStride] = value;
		if ((address - kOpBase) % kOpStride == OP_PHASE)
			op.phase = 0;
		return;
	}
	if (address >= kChannelBase && address < kChannelBase + kChannels * kChannelStride) {
		chip->ch[(address - kChannelBase) / kChannelStride].w[(address - kChannelBase) % kChannelStride] = value;
		return;
	}
	switch (address) {
	case SC_TREMOLO_SHIFT: chip->tremoloShift = value; return;
	case SC_CHANNELS: chip->channels = value; return;
	case SC_MASTER_GAIN: chip->masterGain = value; return;
	case SC_PAUSED: chip->paused = value; return;
	case SC_RHYTHM: chip->rhythm = value; return;
	default: return;
	}
}

// ----------------------------------------------------- block boundary pass

static void opBoundary(Chip *chip, int channel, int which) {
	Op &op = chip->op[slotOfChannel(channel, which)];
	Channel &ch = chip->ch[channel];
	int32_t *w = op.w;

	// An operator with its key up, no key-on pending and a silent envelope
	// stays silent whatever else it holds: its gains are zero and nothing
	// it would compute is observable, so the pass leaves it untouched.
	if (w[OP_TRIG] == w[OP_TRIGSEEN] && !(w[OP_FLAGS] & 1) && w[OP_ENV] == (0x1ff << 12)) {
		w[OP_GAIN] = w[OP_GAINMOD] = w[OP_GAINFB] = 0;
		return;
	}

	if (w[OP_TRIG] != w[OP_TRIGSEEN]) {
		w[OP_TRIGSEEN] = w[OP_TRIG];
		w[OP_STATE] = kAttack;
		op.phase = 0;
		if (which == 0)
			ch.hist0 = ch.hist1 = 0;
		// Maximum rate is instant only on a key-on edge. Selecting that
		// rate during an existing attack holds the chip's current envelope.
		if (w[OP_RATE_A] >= 60)
			w[OP_ENV] = 0;
	}
	if (!(w[OP_FLAGS] & 1) && w[OP_STATE] != kRelease)
		w[OP_STATE] = kRelease;

	int32_t env = w[OP_ENV];
	switch (w[OP_STATE]) {
	case kAttack:
		if (w[OP_RATE_A] > 0 && w[OP_RATE_A] < 60)
			env = mpyHi(env, (int32_t)kOplAttackBlock[w[OP_RATE_A]]);
		if (!env || (w[OP_RATE_A] > 0 && w[OP_RATE_A] < 60 && env < OPL_PRACTICAL_ATTACK_DONE)) {
			env = 0;
			w[OP_STATE] = kDecay;
		}
		break;
	case kDecay:
		// The chip leaves decay when the envelope's top five bits equal the
		// sustain level, not when it is past it: a level lowered under a
		// running decay is never met again, and the decay runs on to silence.
		if (env < w[OP_SL]) {
			env += (int32_t)kOplDecayBlock[w[OP_RATE_D]];
			if (env >= w[OP_SL]) {
				env = w[OP_SL];
				w[OP_STATE] = kSustain;
			}
		} else if (env < w[OP_SL] + (16 << 12)) {
			w[OP_STATE] = kSustain;
		} else {
			env += (int32_t)kOplDecayBlock[w[OP_RATE_D]];
		}
		break;
	case kSustain:
		env += (int32_t)kOplDecayBlock[w[OP_RATE_S]];
		break;
	default:
		env += (int32_t)kOplDecayBlock[w[OP_RATE_R]];
		break;
	}
	if (w[OP_STATE] != kAttack && env >= (0x1f8 << 12))
		env = 0x1ff << 12;
	w[OP_ENV] = env;

	int32_t envOut = (env >> 12) + w[OP_TLKSL] + ((w[OP_FLAGS] & 2) ? chip->tremolo : 0);
	if (envOut > 0x1ff)
		envOut = 0x1ff;
	const int32_t gain = (int32_t)kOplGain[envOut];
	w[OP_GAIN] = gain;
	w[OP_GAINMOD] = mpyHi(gain, 1 << 16);
	w[OP_GAINFB] = ch.w[CH_FBMUL] ? mpyHi(gain, ch.w[CH_FBMUL]) : 0;

	int32_t inc = w[OP_INCBASE];
	if (w[OP_FLAGS] & 4)
		inc = wrap24(inc + w[kVibratoOffset[chip->vibratoPos]]);
	w[OP_INC] = inc;
}

// Twice a product of a sample by a gain, as the rhythm section sounds: the
// DSP multiplies by the negated doubled gain, which reaches -1.0 where the
// doubled gain itself would not fit the word, and negates the product.
static inline int64_t doubledProduct(int32_t sample, int32_t gain) {
	return (int64_t)sample * (int64_t)gain * 4;
}

// What a rhythm operator sounds at after the pass. The pass leaves an idle
// operator's render parameters stale, so silence is read off its record.
static inline int32_t rhythmGain(const Op &op) {
	if (!(op.w[OP_FLAGS] & 1) && op.w[OP_ENV] == (0x1ff << 12))
		return 0;
	return op.w[OP_GAIN];
}

static inline int32_t rhythmIncrement(const Chip *chip, const Op &op) {
	int32_t inc = op.w[OP_INCBASE];
	if (op.w[OP_FLAGS] & 4)
		inc = wrap24(inc + op.w[kVibratoOffset[chip->vibratoPos]]);
	return inc;
}

static inline int32_t waveSampleOf(const Op &op, int32_t phase);

// Rhythm mode, after the melodic pass has run every envelope: the render
// modes of channels six to eight are replaced, and the hi-hat, the snare and
// the cymbal become this block's table of drum sums.
static void rhythmBoundary(Chip *chip) {
	Channel &bass = chip->ch[kChannelBass];
	switch (bass.w[CH_MODE]) {
	case kModeFmFeedback: bass.w[CH_MODE] = kModeBassFmFeedback; break;
	case kModeFmPlain: bass.w[CH_MODE] = kModeBassFmPlain; break;
	case kModeCarrierOnly:
	case kModeAddFeedback:
	case kModeAddPlain: bass.w[CH_MODE] = kModeBassCarrier; break;   // the carrier alone, whatever the connection
	default: bass.w[CH_MODE] = kModeSkip; break;
	}
	chip->ch[kChannelTom].w[CH_MODE] = rhythmGain(chip->op[kOpTom]) ? kModeTom : kModeSkip;

	const Op *drums[3] = { &chip->op[kOpHiHat], &chip->op[kOpCymbal], &chip->op[kOpSnare] };
	const int32_t gains[3] = { rhythmGain(*drums[0]), rhythmGain(*drums[1]), rhythmGain(*drums[2]) };
	chip->hiHatInc = rhythmIncrement(chip, chip->op[kOpHiHat]);
	chip->cymbalInc = rhythmIncrement(chip, chip->op[kOpCymbal]);
	if (!gains[0] && !gains[1] && !gains[2]) {
		chip->ch[kChannelDrums].w[CH_MODE] = kModeDrumsSilent;
		return;
	}
	chip->ch[kChannelDrums].w[CH_MODE] = kModeDrums;
	int64_t pair[4], snare[4];
	for (int i = 0; i < 4; ++i) {
		pair[i] = doubledProduct(waveSampleOf(*drums[0], rhythmPhase(2 * i)), gains[0])
		          + doubledProduct(waveSampleOf(*drums[1], rhythmPhase(2 * i + 1)), gains[1]);
		snare[i] = doubledProduct(waveSampleOf(*drums[2], rhythmPhase(8 + i)), gains[2]);
	}
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			chip->drumTable[4 * i + j] = clamp24((pair[i] + snare[j]) >> 24);
}

static void blockBoundary(Chip *chip) {
	chip->tremoloPhase += OPL_PRACTICAL_TREMOLO_STEP;
	if (chip->tremoloPhase >= (210 << 12))
		chip->tremoloPhase -= 210 << 12;
	const int32_t pos = chip->tremoloPhase >> 12;
	chip->tremolo = (pos < 105 ? pos : 210 - pos) >> chip->tremoloShift;
	chip->vibratoPhase += OPL_PRACTICAL_VIBRATO_STEP;
	if (chip->vibratoPhase >= (8 << 12))
		chip->vibratoPhase -= 8 << 12;
	chip->vibratoPos = chip->vibratoPhase >> 12;

	for (int c = 0; c < chip->channels; ++c) {
		opBoundary(chip, c, 0);
		opBoundary(chip, c, 1);
		Channel &ch = chip->ch[c];
		const bool modSilent = chip->op[slotOfChannel(c, 0)].w[OP_GAIN] == 0;
		const bool carSilent = chip->op[slotOfChannel(c, 1)].w[OP_GAIN] == 0;
		const bool feedback = ch.w[CH_FBMUL] != 0;
		int32_t mode;
		if (!ch.w[CH_CONN]) {
			mode = carSilent ? kModeSkip : modSilent ? kModeCarrierOnly
			                             : feedback ? kModeFmFeedback : kModeFmPlain;
		} else if (modSilent) {
			mode = carSilent ? kModeSkip : kModeCarrierOnly;
		} else if (carSilent) {
			mode = feedback ? kModeModOnlyFeedback : kModeModOnlyPlain;
		} else {
			mode = feedback ? kModeAddFeedback : kModeAddPlain;
		}
		if (mode == kModeSkip)
			ch.hist0 = ch.hist1 = 0;
		ch.w[CH_MODE] = mode;
	}
	if (chip->rhythm)
		rhythmBoundary(chip);
}

// --------------------------------------------------------- stage bodies
//
// Each mirrors one hardware loop of the DSP kernel. The phase is a 48-bit
// index.fraction accumulator advanced by inc * 1023 * 2, the index is the
// top ten bits after masking, and every product is a truncating multiply.

static inline int32_t phaseIndex(const Op &op) { return (int32_t)(op.phase >> 24) & 0x3ff; }

static inline void advance(Op &op) {
	op.phase = (op.phase + (uint64_t)op.w[OP_INC] * 2046u) & 0x3ffffffffull;
}

static inline int32_t fetch(const Op &op, int32_t index) {
	return waveSample((uint8_t)((op.w[OP_WFBASE] - kWaveBase) / 1024), (uint16_t)(index & 0x3ff));
}

static inline int32_t waveSampleOf(const Op &op, int32_t phase) { return fetch(op, phase); }

// A mix-bound product: plain, or doubled for the rhythm section.
static inline int32_t mixProduct(int32_t sample, int32_t gain, bool doubled) {
	return doubled ? (int32_t)(doubledProduct(sample, gain) >> 24) : mpyHi(sample, gain);
}

// An unmodulated operator writing the modulation ring.
static void independentWrite(Chip *chip, Op &op) {
	for (int i = 0; i < kBlockFrames; ++i) {
		const int32_t sample = fetch(op, phaseIndex(op));
		advance(op);
		chip->modRing[i] = mpyHi(sample, op.w[OP_GAINMOD]);
	}
}

// An unmodulated operator accumulating into the mix ring.
static void independentAccumulate(Chip *chip, Op &op, bool doubled = false) {
	for (int i = 0; i < kBlockFrames; ++i) {
		const int32_t sample = fetch(op, phaseIndex(op));
		advance(op);
		chip->mixRing[i] = clamp24((int64_t)chip->mixRing[i] + mixProduct(sample, op.w[OP_GAIN], doubled));
	}
}

// A self-modulated operator; onward product to the ring or the mix.
static void feedbackStage(Chip *chip, Channel &ch, Op &op, bool toMix) {
	const int32_t onward = toMix ? op.w[OP_GAIN] : op.w[OP_GAINMOD];
	for (int i = 0; i < kBlockFrames; ++i) {
		const int32_t index = (ch.hist0 + ch.hist1 + phaseIndex(op)) & 0x3ff;
		ch.hist0 = ch.hist1;
		const int32_t sample = fetch(op, index);
		advance(op);
		ch.hist1 = mpyHi(sample, op.w[OP_GAINFB]);
		const int32_t product = mpyHi(sample, onward);
		if (toMix)
			chip->mixRing[i] = clamp24((int64_t)chip->mixRing[i] + product);
		else
			chip->modRing[i] = product;
	}
}

// A carrier modulated by the ring, accumulating into the mix.
static void serialAccumulate(Chip *chip, Op &op, bool doubled = false) {
	for (int i = 0; i < kBlockFrames; ++i) {
		const int32_t index = (chip->modRing[i] + phaseIndex(op)) & 0x3ff;
		const int32_t sample = fetch(op, index);
		advance(op);
		chip->mixRing[i] = clamp24((int64_t)chip->mixRing[i] + mixProduct(sample, op.w[OP_GAIN], doubled));
	}
}

// The hi-hat, the snare and the cymbal: three passes over the block, as the
// DSP runs them. The noise steps twice a frame and lends two of its bits;
// the hi-hat's phase picks a row of the select table and the cymbal's a
// column, and the entry and the noise bits pick the frame's drum sum.
static void drumStage(Chip *chip) {
	int32_t noiseRing[kBlockFrames], rowRing[kBlockFrames];
	for (int i = 0; i < kBlockFrames; ++i) {
		for (int step = 0; step < 2; ++step) {
			const int32_t carry = chip->noise & 1;
			chip->noise >>= 1;
			if (carry)
				chip->noise ^= kNoiseTaps;
		}
		noiseRing[i] = chip->noise & kNoiseBits;
	}
	Op &hiHat = chip->op[kOpHiHat];
	Op &cymbal = chip->op[kOpCymbal];
	const int32_t hiHatInc = hiHat.w[OP_INC], cymbalInc = cymbal.w[OP_INC];
	hiHat.w[OP_INC] = chip->hiHatInc;
	cymbal.w[OP_INC] = chip->cymbalInc;
	for (int i = 0; i < kBlockFrames; ++i) {
		rowRing[i] = rhythmHiHatRow((uint16_t)phaseIndex(hiHat));
		advance(hiHat);
	}
	for (int i = 0; i < kBlockFrames; ++i) {
		const int32_t select = rhythmSelect(rowRing[i] - kRhythmSelect + rhythmCymbalColumn((uint16_t)phaseIndex(cymbal)));
		advance(cymbal);
		chip->mixRing[i] = clamp24((int64_t)chip->mixRing[i] + chip->drumTable[noiseRing[i] + select]);
	}
	hiHat.w[OP_INC] = hiHatInc;
	cymbal.w[OP_INC] = cymbalInc;
}

// With all three silent the two oscillators still run, since each one's
// phase bits shape the others: a block's advance in one step.
static void drumStageSilent(Chip *chip) {
	Op &hiHat = chip->op[kOpHiHat];
	Op &cymbal = chip->op[kOpCymbal];
	hiHat.phase = (hiHat.phase + (uint64_t)chip->hiHatInc * (2046u * kBlockFrames)) & 0x3ffffffffull;
	cymbal.phase = (cymbal.phase + (uint64_t)chip->cymbalInc * (2046u * kBlockFrames)) & 0x3ffffffffull;
}

// ------------------------------------------------------------ one block

// Renders one block into out[] as the DSP's 24-bit output words: the mix
// scaled by the master gain (rounded, so that full volume, which is one LSB
// short of 1.0, passes the mix through unchanged), plus the host PCM word,
// doubled and saturated. The 16-bit sample is the word's top sixteen bits.
static inline void renderBlock(Chip *chip, const int32_t *pcm, int32_t *out) {
	if (!chip->paused)
		blockBoundary(chip);
	memset(chip->mixRing, 0, sizeof(chip->mixRing));
	for (int c = 0; !chip->paused && c < chip->channels; ++c) {
		Channel &ch = chip->ch[c];
		Op &mod = chip->op[slotOfChannel(c, 0)];
		Op &car = chip->op[slotOfChannel(c, 1)];
		switch (ch.w[CH_MODE]) {
		case kModeCarrierOnly:
			independentAccumulate(chip, car);
			break;
		case kModeFmFeedback:
			feedbackStage(chip, ch, mod, false);
			serialAccumulate(chip, car);
			break;
		case kModeFmPlain:
			independentWrite(chip, mod);
			serialAccumulate(chip, car);
			break;
		case kModeAddFeedback:
			feedbackStage(chip, ch, mod, true);
			independentAccumulate(chip, car);
			break;
		case kModeAddPlain:
			independentAccumulate(chip, mod);
			independentAccumulate(chip, car);
			break;
		case kModeModOnlyFeedback:
			feedbackStage(chip, ch, mod, true);
			break;
		case kModeModOnlyPlain:
			independentAccumulate(chip, mod);
			break;
		case kModeBassCarrier:
			independentAccumulate(chip, car, true);
			break;
		case kModeBassFmFeedback:
			feedbackStage(chip, ch, mod, false);
			serialAccumulate(chip, car, true);
			break;
		case kModeBassFmPlain:
			independentWrite(chip, mod);
			serialAccumulate(chip, car, true);
			break;
		case kModeTom:
			independentAccumulate(chip, mod, true);
			break;
		case kModeDrums:
			drumStage(chip);
			break;
		case kModeDrumsSilent:
			drumStageSilent(chip);
			break;
		default:
			break;
		}
	}
	for (int i = 0; i < kBlockFrames; ++i)
		out[i] = clamp24(2 * ((int64_t)mpyrHi(chip->mixRing[i], chip->masterGain) + (pcm ? pcm[i] : 0)));
	chip->block++;
}

// ------------------------------------------------------------ the decoder
//
// Register-level state and the derivation of every DSP parameter word. The
// sink receives (address, value) pairs; the caller supplies the block the
// write belongs to. Words are emitted only when they change, except by a
// reset, which cannot know what the machine holds and sends everything.
//
// The words an operator derives from several registers - its increments,
// its level and rates, its flags - are not derived at each write but marked
// and derived once, when the block moves on or the caller flushes: a note
// is written as two or three registers that each touch the same words, and
// only the last value of a word within a block ever reaches the boundary
// pass. So a caller must flush before it renders the block it last wrote.

struct Sink {
	virtual ~Sink() {}
	virtual void write(uint32_t block, uint16_t address, int32_t value) = 0;
};

struct Decoder {
	struct SlotRegs {
		uint8_t tremolo, vibrato, type, ksr, mult, ksl, tl, ar, dr, sl, rr, wf;
	};
	struct ChannelRegs {
		uint16_t fnum;
		uint8_t block, feedback, connection;
	};
	// An operator is keyed by its channel's key bit, by its rhythm bit, or by
	// both; a key-on edge is the first of them to come on.
	enum { kKeyChannel = 1, kKeyDrum = 2 };

	SlotRegs slot[kSlots];
	ChannelRegs channel[kChannels];
	uint8_t slotKey[kSlots];
	uint32_t slotTrigger[kSlots];
	uint8_t nts, newm, tremoloShift, vibratoShift;
	uint8_t rhythm;            // register 0xbd's low six bits
	// An OPL2 plays every operator's sine until register 1 enables the
	// waveform select registers; the OPL3 has no such bit.
	uint8_t waveformGate, waveformEnable;
	int channels;
	int32_t shadowOp[kSlots][kOpStride];
	int32_t shadowChannel[kChannels][kChannelStride];
	int32_t shadowScalar[3];
	int32_t shadowRhythm;
	Sink *sink;
	uint32_t block;
	// Operators whose words are still to be derived, one bit per slot index.
	uint64_t dirtyIncrement, dirtyEnvelope, dirtySlot;

	Decoder() { memset(this, 0, sizeof(*this)); }

	// The chip's reset, carried out on the machine as well: every word the
	// host owns goes out whatever the shadow held, and with them the words
	// the machine owns that keep a voice alive (the envelope, its state and
	// the applied key-on count), so nothing of an earlier song survives.
	// Events apply before their block's boundary pass, which therefore finds
	// each operator idle. The machine may be fresh from boot or mid-note.
	void reset(Sink *out, int channelCount, uint32_t atBlock = 0) {
		if (atBlock != block)
			flush();   // they played before the reset; those of its own block never do
		memset(this, 0, sizeof(*this));
		sink = out;
		channels = channelCount;
		block = atBlock;
		tremoloShift = 4;
		vibratoShift = 1;
		waveformGate = channelCount <= 9;
		for (int i = 0; i < kSlots; ++i) {
			int32_t *shadow = shadowOp[i];
			shadow[OP_STATE] = kRelease;
			shadow[OP_ENV] = 0x1ff << 12;
			shadow[OP_WFBASE] = kWaveBase;
			if (i >= channels * 2)
				continue;   // never rendered
			static const uint8_t words[] = {
				OP_TRIG, OP_TRIGSEEN, OP_FLAGS, OP_STATE, OP_ENV, OP_SL, OP_RATE_A, OP_RATE_D, OP_RATE_S,
				OP_RATE_R, OP_TLKSL, OP_INCBASE, OP_WFBASE, OP_VIBDELTA, OP_VIBDELTA + 1, OP_VIBDELTA + 2,
				OP_VIBDELTA + 3, OP_VIBDELTA + 4
			};
			for (unsigned w = 0; w < sizeof(words); ++w)
				sink->write(block, (uint16_t)(kOpBase + i * kOpStride + words[w]), shadow[words[w]]);
		}
		for (int c = 0; c < channels; ++c) {
			sink->write(block, (uint16_t)(kChannelBase + c * kChannelStride + CH_CONN), 0);
			sink->write(block, (uint16_t)(kChannelBase + c * kChannelStride + CH_FBMUL), 0);
		}
		shadowScalar[SC_TREMOLO_SHIFT - SC_TREMOLO_SHIFT] = tremoloShift;
		shadowScalar[SC_CHANNELS - SC_TREMOLO_SHIFT] = channelCount;
		sink->write(block, SC_TREMOLO_SHIFT, tremoloShift);
		sink->write(block, SC_CHANNELS, channelCount);
		sink->write(block, SC_RHYTHM, 0);
	}

	// Every word the shadow holds goes out as the shadow holds it: the way back
	// into step after events were lost on the way to the machine. The shadow
	// is written before the sink sees a value, so after a loss it still holds
	// what the machine should have, and resending it is enough; no later write
	// could repair the loss on its own, because an equal write is suppressed.
	// By default only the host's own words, which leaves running envelopes
	// alone. With machine set, also the words the machine owns that a reset
	// clears - the envelope, its state and the applied key-on count - which the
	// shadow keeps at their reset values: for a reset whose own events were
	// lost, this silences every voice and retriggers the keyed ones instead of
	// leaving the machine on the state the reset ended.
	// Words still to be derived are not in the shadow yet; they go out with
	// the next flush, after these.
	void resend(Sink *out, uint32_t atBlock, bool machine) const {
		static const uint8_t hostWords[] = {
			OP_TRIG, OP_FLAGS, OP_SL, OP_RATE_A, OP_RATE_D, OP_RATE_S, OP_RATE_R, OP_TLKSL,
			OP_INCBASE, OP_WFBASE, OP_VIBDELTA, OP_VIBDELTA + 1, OP_VIBDELTA + 2, OP_VIBDELTA + 3,
			OP_VIBDELTA + 4
		};
		static const uint8_t machineWords[] = { OP_TRIGSEEN, OP_STATE, OP_ENV };
		for (int i = 0; i < channels * 2; ++i) {
			const uint16_t base = (uint16_t)(kOpBase + i * kOpStride);
			if (machine)
				for (unsigned w = 0; w < sizeof(machineWords); ++w)
					out->write(atBlock, (uint16_t)(base + machineWords[w]), shadowOp[i][machineWords[w]]);
			for (unsigned w = 0; w < sizeof(hostWords); ++w)
				out->write(atBlock, (uint16_t)(base + hostWords[w]), shadowOp[i][hostWords[w]]);
		}
		for (int c = 0; c < channels; ++c) {
			const uint16_t base = (uint16_t)(kChannelBase + c * kChannelStride);
			out->write(atBlock, (uint16_t)(base + CH_CONN), shadowChannel[c][CH_CONN]);
			out->write(atBlock, (uint16_t)(base + CH_FBMUL), shadowChannel[c][CH_FBMUL]);
		}
		out->write(atBlock, SC_TREMOLO_SHIFT, shadowScalar[SC_TREMOLO_SHIFT - SC_TREMOLO_SHIFT]);
		out->write(atBlock, SC_CHANNELS, shadowScalar[SC_CHANNELS - SC_TREMOLO_SHIFT]);
		out->write(atBlock, SC_RHYTHM, shadowRhythm);
	}

	// Chip slot index from the OPL register slot number.
	static int slotIndex(int chipSlot) {
		for (int c = 0; c < kChannels; ++c) {
			if (OplKernel::kChannelSlot[c] == chipSlot)
				return slotOfChannel(c, 0);
			if (OplKernel::kChannelSlot[c] + 3 == chipSlot)
				return slotOfChannel(c, 1);
		}
		return -1;
	}

	void emitOp(int index, int offset, int32_t value) {
		if (shadowOp[index][offset] == value)
			return;
		shadowOp[index][offset] = value;
		sink->write(block, (uint16_t)(kOpBase + index * kOpStride + offset), value);
	}
	void emitChannel(int index, int offset, int32_t value) {
		if (shadowChannel[index][offset] == value)
			return;
		shadowChannel[index][offset] = value;
		sink->write(block, (uint16_t)(kChannelBase + index * kChannelStride + offset), value);
	}
	void emitScalar(int address, int32_t value) {
		int32_t &shadow = address == SC_RHYTHM ? shadowRhythm : shadowScalar[address - SC_TREMOLO_SHIFT];
		if (shadow == value)
			return;
		shadow = value;
		sink->write(block, (uint16_t)address, value);
	}

	static int32_t effectiveRate(int ks, int nibble) {
		if (!nibble)
			return 0;
		const int rate = ks + (nibble << 2);
		return rate >= 64 ? 60 + (rate & 3) : rate;
	}

	// The increment word of an f-number as the chip would step it. The
	// chip's phase is 19 bits, so an increment of more than half a cycle
	// per sample (a high block under a high multiplier) is the negative
	// frequency it aliases to at the chip's own rate, and that is what is
	// retimed; saturating it instead would fold every such pitch onto one.
	static int32_t increment(uint32_t fnum, int blockNumber, int mult) {
		const uint32_t native = ((((fnum << blockNumber) >> 1) * OplKernel::kFreqMultiply[mult]) >> 1) & 0x7ffff;
		const int32_t aliased = native >= 0x40000 ? (int32_t)native - 0x80000 : (int32_t)native;
		return (int32_t)(((int64_t)aliased * (int64_t)OPL_PRACTICAL_INC_Q16) >> 16);
	}

	// Pitch: the plain increment and its offset at each vibrato displacement.
	// The chip shifts the f-number's top three bits (halved at the odd LFO
	// positions, halved again at the shallow depth, truncating each time)
	// and adds them to the f-number before the block and the multiplier
	// apply, so a small range vanishes entirely: f-numbers below 256 have no
	// shallow half-step, those below 128 no vibrato at all.
	void updateIncrement(int index) {
		const ChannelRegs &ch = channel[index / 2];
		const uint32_t range = (ch.fnum >> 7) & 7;
		const uint32_t half = (range >> 1) >> vibratoShift;
		const uint32_t full = range >> vibratoShift;
		const int mult = slot[index].mult;
		const int32_t base = increment(ch.fnum, ch.block, mult);
		emitOp(index, OP_INCBASE, base);
		emitOp(index, OP_VIBDELTA + 1, wrap24((int64_t)increment(ch.fnum + half, ch.block, mult) - base));
		emitOp(index, OP_VIBDELTA + 2, wrap24((int64_t)increment(ch.fnum + full, ch.block, mult) - base));
		emitOp(index, OP_VIBDELTA + 3, wrap24((int64_t)increment(ch.fnum - half, ch.block, mult) - base));
		emitOp(index, OP_VIBDELTA + 4, wrap24((int64_t)increment(ch.fnum - full, ch.block, mult) - base));
	}

	// Level and rates: everything the key-scale value reaches.
	void updateEnvelope(int index) {
		const ChannelRegs &ch = channel[index / 2];
		const SlotRegs &s = slot[index];
		const int ksv = (ch.block << 1) | ((ch.fnum >> (9 - nts)) & 1);
		int ksl = (OplKernel::kKslRom[ch.fnum >> 6] << 2) - ((8 - ch.block) << 5);
		if (ksl < 0)
			ksl = 0;
		emitOp(index, OP_TLKSL, (s.tl << 2) + (ksl >> OplKernel::kKslShift[s.ksl]));
		const int ks = ksv >> ((s.ksr ^ 1) << 1);
		emitOp(index, OP_RATE_A, effectiveRate(ks, s.ar));
		emitOp(index, OP_RATE_D, effectiveRate(ks, s.dr));
		emitOp(index, OP_RATE_S, s.type ? 0 : effectiveRate(ks, s.rr));
		emitOp(index, OP_RATE_R, effectiveRate(ks, s.rr));
	}

	void updateSlot(int index) {
		const SlotRegs &s = slot[index];
		emitOp(index, OP_SL, ((s.sl == 15 ? 31 : s.sl) << 4) << 12);
		emitOp(index, OP_FLAGS, (slotKey[index] ? 1 : 0) | (s.tremolo ? 2 : 0) | (s.vibrato ? 4 : 0));
		// The machine holds the OPL2's four waveforms; past them its Y memory
		// is the kernel's own code, so an OPL3 selection stays inside.
		const int32_t selected = newm ? (s.wf & 7) : (s.wf & 3);
		const int32_t wf = (waveformGate && !waveformEnable) ? 0 : (selected % kWaveforms);
		emitOp(index, OP_WFBASE, kWaveBase + wf * 1024);
		emitOp(index, OP_TRIG, (int32_t)slotTrigger[index]);
	}

	void setKey(int index, uint8_t source, bool on) {
		const uint8_t key = (uint8_t)(on ? (slotKey[index] | source) : (slotKey[index] & ~source));
		if (key && !slotKey[index])
			slotTrigger[index]++;
		slotKey[index] = key;
		dirtySlot |= slotBit(index);
	}

	// Register 0xbd's rhythm bits: hi-hat, cymbal, tom-tom, snare, bass drum
	// from bit 0 up, and the mode in bit 5. Leaving the mode releases them.
	void setRhythm(uint8_t value) {
		rhythm = value;
		const bool on = channels >= 9 && (value & 0x20);
		setKey(kOpHiHat, kKeyDrum, on && (value & 0x01));
		setKey(kOpCymbal, kKeyDrum, on && (value & 0x02));
		setKey(kOpTom, kKeyDrum, on && (value & 0x04));
		setKey(kOpSnare, kKeyDrum, on && (value & 0x08));
		setKey(kOpBassModulator, kKeyDrum, on && (value & 0x10));
		setKey(kOpBassCarrier, kKeyDrum, on && (value & 0x10));
		emitScalar(SC_RHYTHM, on ? 1 : 0);
	}

	// What a write marks: one operator, or both of a channel, or every
	// operator a channel count reaches.
	static uint64_t slotBit(int index) { return (uint64_t)1 << index; }
	static uint64_t channelBits(int c) { return (uint64_t)3 << (2 * c); }
	uint64_t allBits() const { return channels * 2 >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << (channels * 2)) - 1); }

	// Derives every marked word, stamped with the block that marked it.
	void flush() {
		const uint64_t any = dirtyIncrement | dirtyEnvelope | dirtySlot;
		if (!any)
			return;
		// The bit steps along rather than being shifted into place: a 64-bit
		// shift by a variable count is a library call on the 68030.
		uint64_t bit = 1;
		for (int index = 0; index < kSlots; ++index, bit <<= 1) {
			if (!(any & bit))
				continue;
			if (dirtyIncrement & bit)
				updateIncrement(index);
			if (dirtyEnvelope & bit)
				updateEnvelope(index);
			if (dirtySlot & bit)
				updateSlot(index);
		}
		dirtyIncrement = dirtyEnvelope = dirtySlot = 0;
	}

	void updateChannel(int c) {
		const ChannelRegs &ch = channel[c];
		emitChannel(c, CH_CONN, ch.connection);
		emitChannel(c, CH_FBMUL, ch.feedback ? (1 << (7 + ch.feedback)) : 0);
	}

	void write(uint32_t atBlock, uint16_t reg, uint8_t value) {
		if (atBlock != block)
			flush();
		block = atBlock;
		const uint8_t high = (uint8_t)((reg >> 8) & 1);
		const uint8_t low = (uint8_t)(reg & 0xff);
		const int8_t addressed = OplKernel::kAddressSlot[low & 0x1f];
		const int index = addressed >= 0 ? slotIndex(18 * high + addressed) : -1;
		SlotRegs *s = index >= 0 ? &slot[index] : nullptr;

		switch (low & 0xf0) {
		case 0x00:
			if (high && (low & 0x0f) == 0x05)
				newm = value & 1;
			else if (!high && (low & 0x0f) == 0x08) {
				nts = (value >> 6) & 1;
				dirtyEnvelope |= allBits();
			} else if (!high && (low & 0x0f) == 0x01) {
				const uint8_t enable = (value >> 5) & 1;
				if (enable != waveformEnable && waveformGate)
					dirtySlot |= allBits();
				waveformEnable = enable;
			}
			return;
		case 0x20:
		case 0x30:
			if (!s)
				return;
			s->tremolo = (value >> 7) & 1;
			s->vibrato = (value >> 6) & 1;
			s->type = (value >> 5) & 1;
			s->ksr = (value >> 4) & 1;
			s->mult = value & 0x0f;
			dirtyIncrement |= slotBit(index);
			dirtyEnvelope |= slotBit(index);
			dirtySlot |= slotBit(index);
			return;
		case 0x40:
		case 0x50:
			if (!s)
				return;
			s->ksl = (value >> 6) & 3;
			s->tl = value & 0x3f;
			dirtyEnvelope |= slotBit(index);
			return;
		case 0x60:
		case 0x70:
			if (!s)
				return;
			s->ar = (value >> 4) & 0x0f;
			s->dr = value & 0x0f;
			dirtyEnvelope |= slotBit(index);
			return;
		case 0x80:
		case 0x90:
			if (!s)
				return;
			s->sl = (value >> 4) & 0x0f;
			s->rr = value & 0x0f;
			dirtyEnvelope |= slotBit(index);
			dirtySlot |= slotBit(index);
			return;
		case 0xe0:
		case 0xf0:
			if (!s)
				return;
			s->wf = value & 0x07;
			dirtySlot |= slotBit(index);
			return;
		case 0xa0:
			if ((low & 0x0f) < 9) {
				const int c = 9 * high + (low & 0x0f);
				channel[c].fnum = (uint16_t)((channel[c].fnum & 0x300) | value);
				dirtyIncrement |= channelBits(c);
				dirtyEnvelope |= channelBits(c);
			}
			return;
		case 0xb0:
			if (low == 0xbd && !high) {
				tremoloShift = (uint8_t)(((((value >> 7) ^ 1) & 1) << 1) + 2);
				vibratoShift = (uint8_t)(((value >> 6) & 1) ^ 1);
				emitScalar(SC_TREMOLO_SHIFT, tremoloShift);
				dirtyIncrement |= allBits();
				setRhythm(value & 0x3f);
				return;
			}
			if ((low & 0x0f) < 9) {
				const int c = 9 * high + (low & 0x0f);
				ChannelRegs &ch = channel[c];
				ch.fnum = (uint16_t)((ch.fnum & 0xff) | ((value & 0x03) << 8));
				ch.block = (value >> 2) & 0x07;
				setKey(slotOfChannel(c, 0), kKeyChannel, (value >> 5) & 1);
				setKey(slotOfChannel(c, 1), kKeyChannel, (value >> 5) & 1);
				dirtyIncrement |= channelBits(c);
				dirtyEnvelope |= channelBits(c);
			}
			return;
		case 0xc0:
			if ((low & 0x0f) < 9) {
				const int c = 9 * high + (low & 0x0f);
				channel[c].feedback = (value & 0x0e) >> 1;
				channel[c].connection = value & 1;
				updateChannel(c);
			}
			return;
		default:
			return;
		}
	}
};

// A sink that applies writes straight into a Chip: the reference path.
struct DirectSink : Sink {
	Chip *chip;
	explicit DirectSink(Chip *c) : chip(c) {}
	void write(uint32_t, uint16_t address, int32_t value) override { poke(chip, address, value); }
};

} // namespace OplPractical

#endif
