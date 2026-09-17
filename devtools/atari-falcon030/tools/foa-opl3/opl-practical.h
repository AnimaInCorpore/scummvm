// Practical (approximate) two-operator OPL kernel for the Falcon DSP56001,
// as a host reference.
//
// The exact kernel in opl-kernel.h costs about twice the DSP's budget because
// the chip's envelope advances every sample. This kernel gives that up: it
// renders at the Falcon codec's 32,779.9479 Hz in operator-major blocks of
// 32 frames, advances every envelope and the LFO once per block, and applies
// register writes at block boundaries. Everything else keeps the chip's
// arithmetic: the 1,024-step waveforms, the log-domain envelope in the same
// 0.1875 dB units, the feedback and modulation depth, f-number pitch.
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
	kVibratoTable = 0x0a80,      // X external, 8 deep then 8 shallow
	kRtParams = 0x0b00,          // X external, 4 words per operator: INC, GAIN, GAINMOD, WFBASE
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
	OP_VIBSTEP2 = 13,    // host: doubled increment of the vibrato range
	OP_WFBASE = 14,      // host: waveform table base in Y
	OP_GAIN = 15,        // gain of a mix-bound product, 24-bit fraction
	OP_GAINMOD = 16,     // gain of a modulation-ring product, gain >> 7
	OP_GAINFB = 17,      // gain of a feedback-history product
	OP_INC = 18          // phase increment for this block (x1 of the mac)
};

enum { CH_MODE = 0, CH_CONN = 1, CH_FBMUL = 2 };   // FBMUL = 2^(7 + fb), 0 for no feedback

// Scalars in internal X the host writes.
enum { SC_TREMOLO_SHIFT = 0x0010, SC_VIB_SHIFT = 0x0011, SC_CHANNELS = 0x0012, SC_MASTER_GAIN = 0x0013 };

enum EnvelopeState { kAttack = 0, kDecay = 1, kSustain = 2, kRelease = 3 };

enum ChannelMode {
	kModeSkip = 0, kModeCarrierOnly = 1, kModeFmFeedback = 2, kModeFmPlain = 3,
	kModeAddFeedback = 4, kModeAddPlain = 5, kModeModOnlyFeedback = 6, kModeModOnlyPlain = 7
};

// ------------------------------------------------------ DSP arithmetic

// The data ALU multiplies two 24-bit operands into a 48-bit product shifted
// left once; A1 is the high word. For an integer times a fraction that is
// the integer part of the scaled value, truncated toward minus infinity.
static inline int32_t mpyHi(int32_t x, int32_t y) {
	return (int32_t)(((int64_t)x * (int64_t)y * 2) >> 24);
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
// attenuation, times 128 so a mix of nine channels stays inside 24 bits and
// one arithmetic shift left lands the 16-bit result in the top of the word.
// A mix-bound product is sample * 128 * gain; a modulation-ring product
// needs the chip's own units (gain >> 7), and a feedback product the chip's
// (out0 + out1) >> (9 - fb) depth, gain * 2^(fb - 16).
static inline int32_t waveSample(uint8_t wf, uint16_t phase) {
	const uint16_t packed = OplKernel::waveform(wf, phase);
	const int32_t negate = (packed & 0x8000) ? -1 : 0;
	uint32_t level = packed & 0x7fff;
	if (level > 0x1fff)
		level = 0x1fff;
	const int32_t out = (int32_t)(kOplExp[level & 0xff] >> (level >> 8)) ^ negate;
	return out * 128;
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
	int32_t tremoloShift, vibratoShift, channels;
	int32_t masterGain;   // fraction applied to the FM mix before the PCM
	uint32_t block;

	const int32_t *vibratoTable() const {
		return (const int32_t *)(vibratoShift ? kOplVibratoShallow : kOplVibratoDeep);
	}
};

static inline int slotOfChannel(int channel, int which) { return 2 * channel + which; }

static inline void reset(Chip *chip, int channels) {
	memset(chip, 0, sizeof(*chip));
	chip->channels = channels;
	chip->tremoloShift = 4;
	chip->vibratoShift = 1;
	chip->masterGain = 0x7fffff;
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
	case SC_VIB_SHIFT: chip->vibratoShift = value; return;
	case SC_CHANNELS: chip->channels = value; return;
	case SC_MASTER_GAIN: chip->masterGain = value; return;
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
	}
	if (!(w[OP_FLAGS] & 1) && w[OP_STATE] != kRelease)
		w[OP_STATE] = kRelease;

	int32_t env = w[OP_ENV];
	switch (w[OP_STATE]) {
	case kAttack:
		env = mpyHi(env, (int32_t)kOplAttackBlock[w[OP_RATE_A]]);
		if (env < (1 << 12)) {
			env = 0;
			w[OP_STATE] = kDecay;
		}
		break;
	case kDecay:
		env += (int32_t)kOplDecayBlock[w[OP_RATE_D]];
		if (env >= w[OP_SL]) {
			env = w[OP_SL];
			w[OP_STATE] = kSustain;
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
		inc = wrap24(inc + mpyHi(w[OP_VIBSTEP2], chip->vibratoTable()[chip->vibratoPos]));
	w[OP_INC] = inc;
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

// An unmodulated operator writing the modulation ring.
static void independentWrite(Chip *chip, Op &op) {
	for (int i = 0; i < kBlockFrames; ++i) {
		const int32_t sample = fetch(op, phaseIndex(op));
		advance(op);
		chip->modRing[i] = mpyHi(sample, op.w[OP_GAINMOD]);
	}
}

// An unmodulated operator accumulating into the mix ring.
static void independentAccumulate(Chip *chip, Op &op) {
	for (int i = 0; i < kBlockFrames; ++i) {
		const int32_t sample = fetch(op, phaseIndex(op));
		advance(op);
		chip->mixRing[i] = clamp24((int64_t)chip->mixRing[i] + mpyHi(sample, op.w[OP_GAIN]));
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
static void serialAccumulate(Chip *chip, Op &op) {
	for (int i = 0; i < kBlockFrames; ++i) {
		const int32_t index = (chip->modRing[i] + phaseIndex(op)) & 0x3ff;
		const int32_t sample = fetch(op, index);
		advance(op);
		chip->mixRing[i] = clamp24((int64_t)chip->mixRing[i] + mpyHi(sample, op.w[OP_GAIN]));
	}
}

// ------------------------------------------------------------ one block

// Renders one block into out[] as the DSP's 24-bit output words: the mix
// scaled by the master gain, plus the host PCM word, doubled and saturated.
// The 16-bit sample is the word's top sixteen bits.
static inline void renderBlock(Chip *chip, const int32_t *pcm, int32_t *out) {
	blockBoundary(chip);
	memset(chip->mixRing, 0, sizeof(chip->mixRing));
	for (int c = 0; c < chip->channels; ++c) {
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
		default:
			break;
		}
	}
	// As the DSP does it: the gain product's 48 bits, the PCM word added to
	// the high word, then the whole accumulator shifted left once, so the
	// product's top fraction bit carries into the result.
	for (int i = 0; i < kBlockFrames; ++i)
		out[i] = clamp24((((int64_t)chip->mixRing[i] * chip->masterGain * 2) >> 23) + 2 * (int64_t)(pcm ? pcm[i] : 0));
	chip->block++;
}

// ------------------------------------------------------------ the decoder
//
// Register-level state and the derivation of every DSP parameter word. The
// sink receives (address, value) pairs; the caller supplies the block the
// write belongs to. Words are emitted only when they change.

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
		uint8_t block, key, feedback, connection;
		uint32_t trigger;
	};

	SlotRegs slot[kSlots];
	ChannelRegs channel[kChannels];
	uint8_t nts, newm, tremoloShift, vibratoShift;
	int channels;
	int32_t shadowOp[kSlots][kOpStride];
	int32_t shadowChannel[kChannels][kChannelStride];
	int32_t shadowScalar[3];
	Sink *sink;
	uint32_t block;

	void reset(Sink *out, int channelCount) {
		memset(this, 0, sizeof(*this));
		sink = out;
		channels = channelCount;
		tremoloShift = 4;
		vibratoShift = 1;
		for (int i = 0; i < kSlots; ++i) {
			shadowOp[i][OP_WFBASE] = kWaveBase;
		}
		shadowScalar[0] = 4;
		shadowScalar[1] = 1;
		shadowScalar[2] = channelCount;
		emitScalar(SC_CHANNELS, channelCount);
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
		int32_t &shadow = shadowScalar[address - SC_TREMOLO_SHIFT];
		if (shadow == value && address != SC_CHANNELS)
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

	void updatePitch(int c) {
		const ChannelRegs &ch = channel[c];
		const int ksv = (ch.block << 1) | ((ch.fnum >> (9 - nts)) & 1);
		for (int which = 0; which < 2; ++which) {
			const int index = slotOfChannel(c, which);
			const SlotRegs &s = slot[index];
			const uint32_t native = ((((uint32_t)ch.fnum << ch.block) >> 1) * OplKernel::kFreqMultiply[s.mult]) >> 1;
			uint64_t inc = ((uint64_t)native * OPL_PRACTICAL_INC_Q16) >> 16;
			if (inc > 0x7fffff)
				inc = 0x7fffff;
			emitOp(index, OP_INCBASE, (int32_t)inc);
			const uint32_t range = (ch.fnum >> 7) & 7;
			uint64_t vib = ((uint64_t)((range << ch.block) * OplKernel::kFreqMultiply[s.mult]) * OPL_PRACTICAL_INC_Q16) >> 17;
			if (vib > 0x7fffff)
				vib = 0x7fffff;
			emitOp(index, OP_VIBSTEP2, (int32_t)vib);
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
	}

	void updateSlot(int index) {
		const SlotRegs &s = slot[index];
		const int c = index / 2;
		emitOp(index, OP_SL, ((s.sl == 15 ? 31 : s.sl) << 4) << 12);
		emitOp(index, OP_FLAGS, (channel[c].key ? 1 : 0) | (s.tremolo ? 2 : 0) | (s.vibrato ? 4 : 0));
		emitOp(index, OP_WFBASE, kWaveBase + (int32_t)(newm ? (s.wf & 7) : (s.wf & 3)) * 1024);
		emitOp(index, OP_TRIG, (int32_t)channel[c].trigger);
	}

	void updateChannel(int c) {
		const ChannelRegs &ch = channel[c];
		emitChannel(c, CH_CONN, ch.connection);
		emitChannel(c, CH_FBMUL, ch.feedback ? (1 << (7 + ch.feedback)) : 0);
	}

	void write(uint32_t atBlock, uint16_t reg, uint8_t value) {
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
				for (int c = 0; c < channels; ++c)
					updatePitch(c);
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
			updatePitch(index / 2);
			updateSlot(index);
			return;
		case 0x40:
		case 0x50:
			if (!s)
				return;
			s->ksl = (value >> 6) & 3;
			s->tl = value & 0x3f;
			updatePitch(index / 2);
			return;
		case 0x60:
		case 0x70:
			if (!s)
				return;
			s->ar = (value >> 4) & 0x0f;
			s->dr = value & 0x0f;
			updatePitch(index / 2);
			return;
		case 0x80:
		case 0x90:
			if (!s)
				return;
			s->sl = (value >> 4) & 0x0f;
			s->rr = value & 0x0f;
			updatePitch(index / 2);
			updateSlot(index);
			return;
		case 0xe0:
		case 0xf0:
			if (!s)
				return;
			s->wf = value & 0x07;
			updateSlot(index);
			return;
		case 0xa0:
			if ((low & 0x0f) < 9) {
				const int c = 9 * high + (low & 0x0f);
				channel[c].fnum = (uint16_t)((channel[c].fnum & 0x300) | value);
				updatePitch(c);
			}
			return;
		case 0xb0:
			if (low == 0xbd && !high) {
				tremoloShift = (uint8_t)(((((value >> 7) ^ 1) & 1) << 1) + 2);
				vibratoShift = (uint8_t)(((value >> 6) & 1) ^ 1);
				emitScalar(SC_TREMOLO_SHIFT, tremoloShift);
				emitScalar(SC_VIB_SHIFT, vibratoShift);
				return;
			}
			if ((low & 0x0f) < 9) {
				const int c = 9 * high + (low & 0x0f);
				ChannelRegs &ch = channel[c];
				ch.fnum = (uint16_t)((ch.fnum & 0xff) | ((value & 0x03) << 8));
				ch.block = (value >> 2) & 0x07;
				const uint8_t key = (value >> 5) & 1;
				if (key && !ch.key)
					ch.trigger++;
				ch.key = key;
				updatePitch(c);
				updateSlot(slotOfChannel(c, 0));
				updateSlot(slotOfChannel(c, 1));
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
