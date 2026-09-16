// Build the DSP benchmark's input image and its expected output from the same
// host kernel that is checked against Nuked-OPL3, so the comparison on the
// other side has one source of truth.
//
// The scenario is a stress case for the synthesis loop: every channel
// sounding, every feedback depth in use, all waveforms in use, and both
// connections present. Envelopes are deliberately parked in a sustaining
// state with a zero sustain rate and tremolo off, because this DSP kernel
// renders synthesis only: its envelope and LFO are not implemented yet, so
// the comparison would otherwise be measuring a hole rather than the loop.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/scummsys.h"
#include "opl-kernel.h"

namespace {

const uint32 kMagic = 0x4F504C44;
const uint32 kOpState = 0x0400;
const uint32 kOpStride = 9;
const uint32 kWaveBase = 0x1000;
const uint32 kShiftTable = 0x0040;

const uint8 kModOffset[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 };

struct Writer {
	FILE *file;
	void word(uint32 value) {
		for (int shift = 24; shift >= 0; shift -= 8)
			std::fputc((int)((value >> shift) & 0xff), file);
	}
};

void fail(const char *what) {
	std::fprintf(stderr, "dsp-fixture: %s\n", what);
	std::exit(1);
}

// The DSP holds the phase shifted left five so its table index is bits 14-23.
uint32 phaseWord(uint32 hostPhase) { return (hostPhase & 0x7ffff) << 5; }

} // namespace

int main(int argc, char **argv) {
	if (argc != 5) {
		std::fprintf(stderr, "usage: dsp-fixture <channels> <frames> <opldata.bin> <expect.bin>\n");
		return 2;
	}
	const int channels = std::atoi(argv[1]);
	const int frames = std::atoi(argv[2]);
	if (channels != 9 && channels != 18)
		fail("channels must be 9 or 18");
	if (frames < 1 || frames > 2048)
		fail("frames must be 1..2048");

	OplKernel::Chip chip;
	OplKernel::reset(&chip, (uint8)channels);
	OplKernel::writeRegister(&chip, 0x01, 0x20);          // waveform select enable
	if (channels == 18)
		OplKernel::writeRegister(&chip, 0x105, 0x01);     // OPL3 mode

	// One patch per channel, spreading waveform, feedback and connection.
	for (int index = 0; index < channels; ++index) {
		const uint16 bank = index < 9 ? 0 : 0x100;
		const uint8 ch = (uint8)(index % 9);
		const uint8 mod = kModOffset[ch];
		const uint8 car = (uint8)(mod + 3);
		const uint8 waveform = (uint8)(index % (channels == 18 ? 8 : 4));
		const uint8 feedback = (uint8)(index % 8);
		// Eighteen channels place every modulator at slot 15 or above, and
		// this kernel only delays carriers, so that configuration stays in
		// the frequency-modulation connection where modulators are silent.
		const uint8 connection = (uint8)(channels == 18 ? 0 : (index % 3 == 0 ? 1 : 0));
		// Bit 5 selects the sustaining envelope type, which parks the
		// envelope once decay reaches the sustain level.
		for (int which = 0; which < 2; ++which) {
			const uint8 slot = which ? car : mod;
			OplKernel::writeRegister(&chip, bank | (0x20 + slot), (uint8)(0x20 | (1 + index % 15)));
			OplKernel::writeRegister(&chip, bank | (0x40 + slot), (uint8)((index * 3) % 64));
			OplKernel::writeRegister(&chip, bank | (0x60 + slot), 0xf0);   // fast attack, fast decay
			OplKernel::writeRegister(&chip, bank | (0x80 + slot), 0x00);   // sustain at full level
			OplKernel::writeRegister(&chip, bank | (0xe0 + slot), waveform);
		}
		uint8 control = (uint8)((feedback << 1) | connection);
		if (channels == 18)
			control |= 0x30;                               // both stereo outputs
		OplKernel::writeRegister(&chip, bank | (0xc0 + ch), control);
		OplKernel::writeRegister(&chip, bank | (0xa0 + ch), (uint8)(0x40 + index * 11));
		OplKernel::writeRegister(&chip, bank | (0xb0 + ch), (uint8)(0x20 | ((index % 7) << 2) | 1));
	}

	// Settle the envelopes, then confirm they really have stopped moving.
	int16 left, right;
	for (int i = 0; i < 20000; ++i)
		OplKernel::generate(&chip, &left, &right);
	uint16 settled[OplKernel::kSlots];
	for (int i = 0; i < OplKernel::kSlots; ++i)
		settled[i] = chip.slot[i].envOut;
	OplKernel::Chip probe = chip;
	for (int i = 0; i < frames + 64; ++i)
		OplKernel::generate(&probe, &left, &right);
	for (int i = 0; i < OplKernel::kSlots; ++i) {
		if (probe.slot[i].envOut != settled[i])
			fail("an envelope is still moving; the frozen-envelope comparison would be invalid");
		if (chip.slot[i].regVib)
			fail("vibrato is set; this DSP kernel does not implement it yet");
		if (chip.slot[i].tremoloOn)
			fail("tremolo is set; this DSP kernel does not implement it yet");
	}

	// ---- the DSP image
	Writer out;
	out.file = std::fopen(argv[3], "wb");
	if (!out.file)
		fail("cannot create the data image");

	const int waveforms = channels == 18 ? 8 : 4;
	// Carriers of channels whose carrier slot is 15 or above reach the left
	// mix one sample late, and they are the last channels in order.
	int delayed = 0;
	for (int index = 0; index < channels; ++index)
		if (chip.channel[index].slot1 >= 15)
			++delayed;
	for (int index = 0; index < channels - delayed; ++index)
		if (chip.channel[index].slot1 >= 15)
			fail("the delayed carriers are not the last channels");
	int rightDelayed = 0;
	for (int index = 0; index < channels; ++index)
		if (chip.channel[index].slot1 >= 33)
			++rightDelayed;
	for (int index = 0; index < channels - rightDelayed; ++index)
		if (chip.channel[index].slot1 >= 33)
			fail("the right-delayed carriers are not the last channels");
	// A delayed modulator would need a case this kernel does not implement,
	// but a modulator only reaches the mix in the additive connection.
	for (int index = 0; index < channels; ++index)
		if (chip.channel[index].connection && chip.channel[index].slot0 >= 15)
			fail("a contributing modulator is delayed; the DSP kernel cannot express that");

	out.word(kMagic);
	out.word((uint32)channels);
	out.word((uint32)delayed);
	out.word((uint32)rightDelayed);
	out.word((uint32)frames);
	out.word(5 + (uint32)waveforms * 2);        // blocks

	// The chip's output pipeline is one frame deep, so the first rendered
	// frame needs the state the warm-up left behind: the pending right mix
	// and the delayed group's previous sum. X:$0004 and X:$0005 are
	// right_pending and ldelay_prev in the DSP's scalar page.
	int32 delayedPrevious = 0;
	for (int index = channels - delayed; index < channels; ++index)
		delayedPrevious += chip.slot[chip.channel[index].slot1].out;
	int32 rightDelayedPrevious = 0;
	for (int index = channels - rightDelayed; index < channels; ++index)
		rightDelayedPrevious += chip.slot[chip.channel[index].slot1].out;
	out.word(0); out.word(0x0004); out.word(2);
	out.word((uint32)chip.rightPending & 0xffffff);
	out.word((uint32)delayedPrevious & 0xffffff);
	// rdelay_prev sits at the end of the scalar page, after carrier_first.
	out.word(0); out.word(0x0012); out.word(1);
	out.word((uint32)rightDelayedPrevious & 0xffffff);

	// exponential ROM, pre-doubled, into internal Y
	out.word(1); out.word(0x0000); out.word(256);
	for (int i = 0; i < 256; ++i)
		out.word((uint32)(2 * kOplExp[i]) & 0xffffff);

	// shift constants 2^(22-k), zero once the exponent kills the value
	out.word(0); out.word(kShiftTable); out.word(64);
	for (int k = 0; k < 64; ++k)
		out.word(k <= 11 ? (uint32)(1u << (22 - k)) : 0u);

	// waveform magnitudes into external X, sign masks into external Y
	for (int wf = 0; wf < waveforms; ++wf) {
		out.word(0); out.word(kWaveBase + (uint32)wf * 1024); out.word(1024);
		for (uint16 phase = 0; phase < 1024; ++phase)
			out.word((uint32)(OplKernel::waveform((uint8)wf, phase) & 0x7fff));
	}
	for (int wf = 0; wf < waveforms; ++wf) {
		out.word(1); out.word(kWaveBase + (uint32)wf * 1024); out.word(1024);
		for (uint16 phase = 0; phase < 1024; ++phase)
			out.word((OplKernel::waveform((uint8)wf, phase) & 0x8000) ? 0xffffffu : 0u);
	}

	// operator state, modulator then carrier, in channel order
	out.word(0); out.word(kOpState); out.word((uint32)channels * 2 * kOpStride);
	for (int index = 0; index < channels; ++index) {
		const OplKernel::Channel &channel = chip.channel[index];
		for (int which = 0; which < 2; ++which) {
			const OplKernel::Slot &slot = chip.slot[which ? channel.slot1 : channel.slot0];
			const bool carrier = which != 0;
			const bool contributes = carrier || channel.connection;
			uint32 words[kOpStride];
			std::memset(words, 0, sizeof(words));
			words[0] = (uint32)slot.out & 0xffffff;
			words[1] = (uint32)slot.prevOut & 0xffffff;
			// A right shift by k is a multiply by 2^(23-k); feedback uses
			// k = 9 - fb, so the constant never reaches the sign bit.
			words[2] = (!carrier && channel.feedback)
			           ? (uint32)(1u << (14 + channel.feedback)) : 0u;
			words[3] = (carrier && !channel.connection) ? 0xffffffu : 0u;
			words[4] = phaseWord(slot.phaseInc);
			words[5] = phaseWord(slot.phase);
			words[6] = kWaveBase + (uint32)slot.regWf * 1024;
			words[7] = (uint32)slot.envOut << 3;
			words[8] = contributes ? 0xffffffu : 0u;
			for (uint32 w = 0; w < kOpStride; ++w)
				out.word(words[w]);
		}
	}
	std::fclose(out.file);

	// ---- the frames the host kernel produces from this state
	Writer expect;
	expect.file = std::fopen(argv[4], "wb");
	if (!expect.file)
		fail("cannot create the expected output");
	for (int i = 0; i < frames; ++i) {
		OplKernel::generate(&chip, &left, &right);
		expect.word((uint32)right & 0xffffff);
		expect.word((uint32)left & 0xffffff);
	}
	std::fclose(expect.file);

	std::printf("{\"channels\": %d, \"frames\": %d, \"waveforms\": %d,"
	            " \"delayed_carriers\": %d, \"right_delayed_carriers\": %d}\n",
	            channels, frames, waveforms, delayed, rightDelayed);
	return 0;
}
