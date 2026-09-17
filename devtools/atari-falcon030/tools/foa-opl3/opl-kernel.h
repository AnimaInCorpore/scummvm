// Two-operator OPL kernel, written in the arithmetic a DSP56001 can execute
// directly: flat arrays, index routing instead of pointers, no division and
// no value wider than the 24-bit word the target actually has.
//
// Scope: two-operator melodic channels on both register banks, OPL2 (nine
// channels) and OPL3 two-operator mode (eighteen). Hardware four-operator
// pairing and rhythm mode are deliberately absent; neither ScummVM AdLib
// driver enables them, and the captured Atlantis stream never touches them.
//
// This is the reference the DSP assembly is transliterated from, and the
// oracle its output is compared against. It is checked sample for sample
// against Nuked-OPL3 by kernel-test.cpp.
#ifndef FOA_OPL_KERNEL_H
#define FOA_OPL_KERNEL_H

#include <stdint.h>
#include <string.h>

#include "opl-tables.h"

namespace OplKernel {

enum { kChannels = 18, kSlots = 36 };

// Modulation source for a slot's phase input.
enum ModSource { kModZero = 0, kModSelfFeedback = 1, kModPartner = 2 };

enum EnvelopeGen { kAttack = 0, kDecay = 1, kSustain = 2, kRelease = 3 };

static const uint8_t kFreqMultiply[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
static const uint8_t kKslRom[16] = { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };
static const uint8_t kKslShift[4] = { 8, 1, 2, 0 };
static const uint8_t kEgIncStep[4][4] = { { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 1, 0 }, { 1, 1, 1, 0 } };
// Register offset 0x00..0x1f to slot index, -1 where the chip decodes nothing.
static const int8_t kAddressSlot[0x20] = {
	0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
	12, 13, 14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};
// Channel index to its modulator slot; the carrier is three slots higher.
static const uint8_t kChannelSlot[18] = {
	0, 1, 2, 6, 7, 8, 12, 13, 14, 18, 19, 20, 24, 25, 26, 30, 31, 32
};

struct Slot {
	// Phase accumulator. Only bits 9..18 ever reach the waveform lookup, so
	// nineteen bits is the whole requirement; a 24-bit word covers it.
	uint32_t phase;
	uint32_t phaseInc;
	uint32_t phaseIncVib[8];
	uint16_t phaseOut;
	int16_t out;
	int16_t prevOut;
	int16_t feedbackMod;

	uint16_t envRaw;     // 9-bit attenuation held by the envelope generator
	uint16_t envOut;     // envRaw + level/scaling + tremolo
	uint16_t envTlKsl;   // cached (regTl << 2) + scaled key level
	uint8_t envGen;
	uint8_t envKsl;
	uint8_t envKs;
	uint8_t envRates[4];
	uint8_t envRateHi[4];
	uint8_t envRateLo[4];

	uint8_t key;
	uint8_t phaseReset;
	uint8_t regVib, regType, regKsr, regMult, regKsl, regTl;
	uint8_t regAr, regDr, regSl, regRr, regWf;
	uint8_t tremoloOn;
	uint8_t channel;
	uint8_t modSource;
};

struct Channel {
	uint16_t fnum;
	uint8_t block;
	uint8_t ksv;
	uint8_t feedback;
	uint8_t connection;
	uint8_t outLeftMask;   // 1 when this channel reaches the left output
	uint8_t outRightMask;
	uint8_t slot0, slot1;
};

struct Chip {
	Slot slot[kSlots];
	Channel channel[kChannels];

	uint32_t timer;
	uint16_t tremoloPos;
	uint8_t tremolo;
	uint8_t tremoloShift;
	uint8_t vibPos;
	uint8_t vibShift;

	// Envelope clock: 36 bits, held on the DSP as two 24-bit words. Only the
	// low 13 bits drive the rate machine, but the wrap point is observable
	// after 2^36 samples, so the width is reproduced rather than truncated.
	uint64_t envTimer;
	uint8_t envTimerRem;
	uint8_t envState;
	uint8_t envAdd;
	uint8_t envTimerLo;

	uint8_t newm;   // OPL3 mode
	uint8_t nts;
	uint8_t channels;   // 9 for OPL2, 18 for OPL3 two-operator

	int32_t rightPending;   // the right mix is one sample behind the left
};

// ---------------------------------------------------------------- waveforms

// Quarter-sine mirror, reading bits 0..8 of the phase.
static inline uint16_t logSinQuarter(uint16_t phase) {
	return kOplLogSin[(phase & 0x100) ? ((phase & 0xff) ^ 0xff) : (phase & 0xff)];
}

// Double-rate variant used by waveforms 4 and 5.
static inline uint16_t logSinDouble(uint16_t phase) {
	return kOplLogSin[(phase & 0x80) ? (((phase ^ 0xff) << 1) & 0xff) : ((phase << 1) & 0xff)];
}

// Packed waveform word: bit 15 is the output sign, bits 0..14 the attenuation.
// 0x1000 is the silent entry; it drives the exponent past the table's range.
static inline uint16_t waveform(uint8_t wf, uint16_t phase) {
	phase &= 0x3ff;
	switch (wf) {
	case 0: return (uint16_t)((phase & 0x200 ? 0x8000 : 0) | logSinQuarter(phase));
	case 1: return (uint16_t)(phase & 0x200 ? 0x1000 : logSinQuarter(phase));
	case 2: return logSinQuarter(phase);
	case 3: return (uint16_t)(phase & 0x100 ? 0x1000 : kOplLogSin[phase & 0xff]);
	case 4: return (uint16_t)(phase & 0x200 ? 0x1000
	                          : ((phase & 0x100 ? 0x8000 : 0) | logSinDouble(phase)));
	case 5: return (uint16_t)(phase & 0x200 ? 0x1000 : logSinDouble(phase));
	case 6: return (uint16_t)(phase & 0x200 ? 0x8000 : 0);
	default: return (uint16_t)(phase & 0x200 ? (0x8000 | (((phase ^ 0x1ff) & 0x1ff) << 3))
	                                         : ((phase & 0x1ff) << 3));
	}
}

// ------------------------------------------------------------ slot updates

static inline void updateKeyScaleLevel(Chip *chip, Slot *slot) {
	const Channel *channel = &chip->channel[slot->channel];
	int16_t ksl = (int16_t)((kKslRom[channel->fnum >> 6] << 2) - ((0x08 - channel->block) << 5));
	if (ksl < 0)
		ksl = 0;
	slot->envKsl = (uint8_t)ksl;
	slot->envTlKsl = (uint16_t)((slot->regTl << 2) + (slot->envKsl >> kKslShift[slot->regKsl]));
}

static inline void updateEnvelopeRate(Chip *chip, Slot *slot) {
	slot->envKs = (uint8_t)(chip->channel[slot->channel].ksv >> ((slot->regKsr ^ 1) << 1));
	for (uint8_t i = 0; i < 4; ++i) {
		uint8_t rate = (uint8_t)(slot->envKs + (slot->envRates[i] << 2));
		uint8_t hi = (uint8_t)(rate >> 2);
		if (hi & 0x10)
			hi = 0x0f;
		slot->envRateHi[i] = hi;
		slot->envRateLo[i] = (uint8_t)(rate & 0x03);
	}
}

static inline void updatePhaseIncrement(Chip *chip, Slot *slot) {
	const Channel *channel = &chip->channel[slot->channel];
	uint32_t base = ((uint32_t)channel->fnum << channel->block) >> 1;
	slot->phaseInc = (base * kFreqMultiply[slot->regMult]) >> 1;
	for (uint8_t pos = 0; pos < 8; ++pos) {
		int8_t range = (int8_t)((channel->fnum >> 7) & 7);
		if (!(pos & 3))
			range = 0;
		else if (pos & 1)
			range >>= 1;
		range >>= chip->vibShift;
		if (pos & 4)
			range = (int8_t)-range;
		uint16_t fnum = (uint16_t)(channel->fnum + range);
		slot->phaseIncVib[pos] =
			((((uint32_t)fnum << channel->block) >> 1) * kFreqMultiply[slot->regMult]) >> 1;
	}
}

static inline void updateChannelKsv(Chip *chip, Channel *channel) {
	channel->ksv = (uint8_t)((channel->block << 1)
	                         | ((channel->fnum >> (0x09 - chip->nts)) & 0x01));
}

static void refreshChannel(Chip *chip, uint8_t index) {
	Channel *channel = &chip->channel[index];
	updateChannelKsv(chip, channel);
	for (int which = 0; which < 2; ++which) {
		Slot *slot = &chip->slot[which ? channel->slot1 : channel->slot0];
		updateKeyScaleLevel(chip, slot);
		updateEnvelopeRate(chip, slot);
		updatePhaseIncrement(chip, slot);
	}
}

// -------------------------------------------------------- envelope machine

static void envelopeStep(Chip *chip, Slot *slot) {
	uint8_t reset = 0;
	uint8_t regRate;

	slot->envOut = (uint16_t)(slot->envRaw + slot->envTlKsl + (slot->tremoloOn ? chip->tremolo : 0));
	if (slot->key && slot->envGen == kRelease) {
		reset = 1;
		regRate = slot->envRates[0];
	} else {
		regRate = slot->envRates[slot->envGen];
	}
	slot->phaseReset = reset;

	const uint8_t index = reset ? 0 : slot->envGen;
	const uint8_t rateHi = slot->envRateHi[index];
	const uint8_t rateLo = slot->envRateLo[index];
	const uint8_t envShift = (uint8_t)(rateHi + chip->envAdd);
	uint8_t shift = 0;
	if (regRate != 0) {
		if (rateHi < 12) {
			if (chip->envState) {
				if (envShift == 12)
					shift = 1;
				else if (envShift == 13)
					shift = (uint8_t)((rateLo >> 1) & 1);
				else if (envShift == 14)
					shift = (uint8_t)(rateLo & 1);
			}
		} else {
			shift = (uint8_t)((rateHi & 0x03) + kEgIncStep[rateLo][chip->envTimerLo]);
			if (shift & 0x04)
				shift = 0x03;
			if (!shift)
				shift = chip->envState;
		}
	}

	uint16_t envRaw = slot->envRaw;
	int16_t envInc = 0;
	uint8_t envOff = 0;
	if (reset && rateHi == 0x0f)
		envRaw = 0;                       // instant attack
	if ((slot->envRaw & 0x1f8) == 0x1f8)
		envOff = 1;
	if (slot->envGen != kAttack && !reset && envOff)
		envRaw = 0x1ff;

	switch (slot->envGen) {
	case kAttack:
		if (!slot->envRaw)
			slot->envGen = kDecay;
		else if (slot->key && shift > 0 && rateHi != 0x0f)
			envInc = (int16_t)((uint16_t)~slot->envRaw >> (4 - shift));
		break;
	case kDecay:
		if ((slot->envRaw >> 4) == slot->regSl)
			slot->envGen = kSustain;
		else if (!envOff && !reset && shift > 0)
			envInc = (int16_t)(1 << (shift - 1));
		break;
	default:
		if (!envOff && !reset && shift > 0)
			envInc = (int16_t)(1 << (shift - 1));
		break;
	}
	slot->envRaw = (uint16_t)((envRaw + envInc) & 0x1ff);
	if (reset)
		slot->envGen = kAttack;
	if (!slot->key)
		slot->envGen = kRelease;
}

// ------------------------------------------------------------- slot render

static inline int16_t modulationOf(const Chip *chip, const Slot *slot) {
	switch (slot->modSource) {
	case kModSelfFeedback: return slot->feedbackMod;
	case kModPartner: return chip->slot[chip->channel[slot->channel].slot0].out;
	default: return 0;
	}
}

static void processSlot(Chip *chip, Slot *slot, uint8_t feedback) {
	// Feedback uses this slot's two previous outputs, before the new one.
	slot->feedbackMod = feedback ? (int16_t)((slot->prevOut + slot->out) >> (9 - feedback)) : 0;
	slot->prevOut = slot->out;

	envelopeStep(chip, slot);

	const uint32_t inc = slot->regVib ? slot->phaseIncVib[chip->vibPos] : slot->phaseInc;
	slot->phaseOut = (uint16_t)((slot->phase >> 9) & 0x3ff);
	if (slot->phaseReset)
		slot->phase = 0;
	slot->phase = (slot->phase + inc) & 0x7ffff;

	const uint16_t phase = (uint16_t)(slot->phaseOut + (uint16_t)modulationOf(chip, slot));
	const uint16_t packed = waveform(slot->regWf, phase);
	const int16_t negate = (int16_t)(packed & 0x8000 ? -1 : 0);
	uint32_t level = (uint32_t)(packed & 0x7fff) + ((uint32_t)slot->envOut << 3);
	if (level > 0x1fff)
		level = 0x1fff;
	const uint8_t exponent = (uint8_t)(level >> 8);
	slot->out = (int16_t)((int16_t)(kOplExp[level & 0xff] >> exponent) ^ negate);
}

// ------------------------------------------------------------------- mixer

static inline int16_t clipSample(int32_t sample) {
	if (sample > 32767)
		return 32767;
	if (sample < -32768)
		return -32768;
	return (int16_t)sample;
}

// One native-rate stereo frame. The right output trails the left by one
// sample, exactly as the chip model does.
static inline void generate(Chip *chip, int16_t *left, int16_t *right) {
	*right = clipSample(chip->rightPending);

	for (uint8_t index = 0; index < chip->channels; ++index) {
		Channel *channel = &chip->channel[index];
		processSlot(chip, &chip->slot[channel->slot0], channel->feedback);
		processSlot(chip, &chip->slot[channel->slot1], channel->feedback);
	}

	int32_t mixLeft = 0;
	for (uint8_t index = 0; index < chip->channels; ++index) {
		Channel *channel = &chip->channel[index];
		if (!channel->outLeftMask)
			continue;
		// Slots 15 and above reach the left mix one sample late.
		const Slot *carrier = &chip->slot[channel->slot1];
		const Slot *modulator = &chip->slot[channel->slot0];
		int16_t accumulated = (channel->slot1 >= 15) ? carrier->prevOut : carrier->out;
		if (channel->connection)
			accumulated = (int16_t)(accumulated
			                        + ((channel->slot0 >= 15) ? modulator->prevOut : modulator->out));
		mixLeft += accumulated;
	}
	*left = clipSample(mixLeft);

	if ((chip->timer & 0x3f) == 0x3f) {
		if (++chip->tremoloPos == 210)
			chip->tremoloPos = 0;
	}
	chip->tremolo = (uint8_t)((chip->tremoloPos < 105 ? chip->tremoloPos : 210 - chip->tremoloPos)
	                          >> chip->tremoloShift);
	if ((chip->timer & 0x3ff) == 0x3ff)
		chip->vibPos = (uint8_t)((chip->vibPos + 1) & 7);
	chip->timer++;

	if (chip->envState) {
		const uint32_t low = (uint32_t)(chip->envTimer & 0x1fff);
		if (!low) {
			chip->envAdd = 0;
		} else {
			uint8_t shift = 0;
			while (!((low >> shift) & 1))
				++shift;
			chip->envAdd = (uint8_t)(shift + 1);
		}
		chip->envTimerLo = (uint8_t)(chip->envTimer & 3);
	}
	if (chip->envTimerRem || chip->envState) {
		if (chip->envTimer == 0xfffffffffull) {
			chip->envTimer = 0;
			chip->envTimerRem = 1;
		} else {
			chip->envTimer++;
			chip->envTimerRem = 0;
		}
	}
	chip->envState ^= 1;

	// The right mix is computed now and emitted by the next call. Its delay
	// threshold is slot 33, so it only affects the last three OPL3 channels.
	int32_t mixRight = 0;
	for (uint8_t index = 0; index < chip->channels; ++index) {
		Channel *channel = &chip->channel[index];
		if (!channel->outRightMask)
			continue;
		const Slot *carrier = &chip->slot[channel->slot1];
		const Slot *modulator = &chip->slot[channel->slot0];
		int16_t accumulated = (channel->slot1 >= 33) ? carrier->prevOut : carrier->out;
		if (channel->connection)
			accumulated = (int16_t)(accumulated
			                        + ((channel->slot0 >= 33) ? modulator->prevOut : modulator->out));
		mixRight += accumulated;
	}
	chip->rightPending = mixRight;
}

// ------------------------------------------------------------ register I/O

static void setConnection(Chip *chip, Channel *channel) {
	chip->slot[channel->slot0].modSource = kModSelfFeedback;
	chip->slot[channel->slot1].modSource = channel->connection ? kModZero : kModPartner;
}

static inline void reset(Chip *chip, uint8_t channels) {
	memset(chip, 0, sizeof(*chip));
	chip->channels = channels;
	chip->tremoloShift = 4;
	chip->vibShift = 1;
	for (uint8_t index = 0; index < kSlots; ++index) {
		chip->slot[index].envRaw = 0x1ff;
		chip->slot[index].envOut = 0x1ff;
		chip->slot[index].envGen = kRelease;
	}
	for (uint8_t index = 0; index < kChannels; ++index) {
		Channel *channel = &chip->channel[index];
		channel->slot0 = kChannelSlot[index];
		channel->slot1 = (uint8_t)(kChannelSlot[index] + 3);
		channel->outLeftMask = 1;
		channel->outRightMask = 1;
		chip->slot[channel->slot0].channel = index;
		chip->slot[channel->slot1].channel = index;
		setConnection(chip, channel);
	}
}

static inline void writeRegister(Chip *chip, uint16_t reg, uint8_t value) {
	const uint8_t high = (uint8_t)((reg >> 8) & 1);
	const uint8_t low = (uint8_t)(reg & 0xff);
	const int8_t addressed = kAddressSlot[low & 0x1f];
	Slot *slot = addressed >= 0 ? &chip->slot[18 * high + addressed] : nullptr;

	switch (low & 0xf0) {
	case 0x00:
		if (high && (low & 0x0f) == 0x05)
			chip->newm = (uint8_t)(value & 1);
		else if (!high && (low & 0x0f) == 0x08)
			chip->nts = (uint8_t)((value >> 6) & 1);
		return;
	case 0x20:
	case 0x30:
		if (!slot)
			return;
		slot->tremoloOn = (uint8_t)((value >> 7) & 1);
		slot->regVib = (uint8_t)((value >> 6) & 1);
		slot->regType = (uint8_t)((value >> 5) & 1);
		slot->envRates[2] = slot->regType ? 0 : slot->regRr;
		slot->regKsr = (uint8_t)((value >> 4) & 1);
		slot->regMult = (uint8_t)(value & 0x0f);
		updateEnvelopeRate(chip, slot);
		updatePhaseIncrement(chip, slot);
		return;
	case 0x40:
	case 0x50:
		if (!slot)
			return;
		slot->regKsl = (uint8_t)((value >> 6) & 3);
		slot->regTl = (uint8_t)(value & 0x3f);
		updateKeyScaleLevel(chip, slot);
		return;
	case 0x60:
	case 0x70:
		if (!slot)
			return;
		slot->regAr = (uint8_t)((value >> 4) & 0x0f);
		slot->regDr = (uint8_t)(value & 0x0f);
		slot->envRates[0] = slot->regAr;
		slot->envRates[1] = slot->regDr;
		updateEnvelopeRate(chip, slot);
		return;
	case 0x80:
	case 0x90:
		if (!slot)
			return;
		slot->regSl = (uint8_t)((value >> 4) & 0x0f);
		if (slot->regSl == 0x0f)
			slot->regSl = 0x1f;
		slot->regRr = (uint8_t)(value & 0x0f);
		slot->envRates[2] = slot->regType ? 0 : slot->regRr;
		slot->envRates[3] = slot->regRr;
		updateEnvelopeRate(chip, slot);
		return;
	case 0xe0:
	case 0xf0:
		if (!slot)
			return;
		slot->regWf = (uint8_t)(value & 0x07);
		if (!chip->newm)
			slot->regWf &= 0x03;
		return;
	case 0xa0:
		if ((low & 0x0f) < 9) {
			Channel *channel = &chip->channel[9 * high + (low & 0x0f)];
			channel->fnum = (uint16_t)((channel->fnum & 0x300) | value);
			refreshChannel(chip, (uint8_t)(9 * high + (low & 0x0f)));
		}
		return;
	case 0xb0:
		if (low == 0xbd && !high) {
			const uint8_t tremoloShift = (uint8_t)(((((value >> 7) ^ 1) & 1) << 1) + 2);
			const uint8_t vibShift = (uint8_t)(((value >> 6) & 1) ^ 1);
			chip->tremoloShift = tremoloShift;
			if (chip->vibShift != vibShift) {
				chip->vibShift = vibShift;
				for (uint8_t index = 0; index < kSlots; ++index)
					updatePhaseIncrement(chip, &chip->slot[index]);
			}
			return;   // rhythm mode is out of scope and never enabled here
		}
		if ((low & 0x0f) < 9) {
			const uint8_t index = (uint8_t)(9 * high + (low & 0x0f));
			Channel *channel = &chip->channel[index];
			channel->fnum = (uint16_t)((channel->fnum & 0xff) | ((value & 0x03) << 8));
			channel->block = (uint8_t)((value >> 2) & 0x07);
			refreshChannel(chip, index);
			const uint8_t on = (uint8_t)((value >> 5) & 1);
			chip->slot[channel->slot0].key = on;
			chip->slot[channel->slot1].key = on;
		}
		return;
	case 0xc0:
		if ((low & 0x0f) < 9) {
			Channel *channel = &chip->channel[9 * high + (low & 0x0f)];
			channel->feedback = (uint8_t)((value & 0x0e) >> 1);
			channel->connection = (uint8_t)(value & 1);
			setConnection(chip, channel);
			if (chip->newm) {
				channel->outLeftMask = (uint8_t)((value >> 4) & 1);
				channel->outRightMask = (uint8_t)((value >> 5) & 1);
			}
		}
		return;
	default:
		return;
	}
}

} // namespace OplKernel

#endif
