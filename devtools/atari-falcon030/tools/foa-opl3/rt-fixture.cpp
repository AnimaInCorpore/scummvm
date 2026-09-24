// Build the DSP practical-kernel bench's input image and its expected output
// from the host reference in opl-practical.h, so the comparison on the DSP
// side has one source of truth.
//
// OPLDATA.BIN is big-endian 32-bit throughout:
//   'OPLR', upload block count, then per block: space (0 = X, 1 = Y),
//   address, word count, the words; then chunk count, then per chunk:
//   block count, event count, and (block << 16 | address), value pairs.
// EXPECT.BIN holds every chunk's output words in order, 24 bits each.
//
// usage: opl-rt-fixture <trace|stress|paths|rhythm|phase> <opldata.bin> <expect.bin>
//                       [--trace opl-writes.ev] [--from S] [--seconds N]
//                       [--chunk-blocks N] [--play playdata.bin]
//
// --from starts the trace's window S seconds in, on the register image the
// earlier writes left.
//
// With --play the events are also written as stream-mode periods of 768
// frames with silent PCM ('OPLP', period count, then per period: event
// count, the event pairs, a zero PCM flag), and the manifest carries the
// checksum the DSP's stream emit must reproduce: the sum of the limited
// output words over every period, modulo 2^24.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/scummsys.h"
#include "opl-kernel.h"
#include "opl-practical.h"

namespace {

namespace P = OplPractical;

const uint32 kMagic = 0x4F504C52;   // 'OPLR'
const uint32 kMaxEventsPerChunk = 2048;

struct Writer {
	FILE *file;
	void word(uint32 value) {
		for (int shift = 24; shift >= 0; shift -= 8)
			std::fputc((int)((value >> shift) & 0xff), file);
	}
};

void fail(const char *what) {
	std::fprintf(stderr, "rt-fixture: %s\n", what);
	std::exit(1);
}

struct Event {
	uint32 block;
	uint16 address;
	int32 value;
};

struct RecordingSink : P::Sink {
	std::vector<Event> events;
	void write(uint32 block, uint16 address, int32 value) override {
		events.push_back(Event{block, address, value});
	}
};

struct RegisterWrite {
	double seconds;
	uint16 reg;
	uint8 value;
};

const uint8 kModOffset[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 };
const uint16 kResetChip = 0xffff;   // not a register: the driver resetting the chip
const uint16 kPauseChip = 0xfffe;   // not a register: the mixer pausing FM

// Every channel in feedback FM with tremolo and vibrato, held: the most
// expensive render path on all nine channels for the whole run.
std::vector<RegisterWrite> stressScript(double seconds) {
	std::vector<RegisterWrite> writes;
	writes.push_back(RegisterWrite{0.0, 0x01, 0x20});
	writes.push_back(RegisterWrite{0.0, 0xbd, 0xc0});
	static const uint16 chord[9] = { 0x181, 0x1e5, 0x241, 0x2aa, 0x181, 0x1e5, 0x241, 0x2aa, 0x33d };
	for (uint8 c = 0; c < 9; ++c) {
		const uint8 mod = kModOffset[c];
		const uint8 car = (uint8)(mod + 3);
		const uint8 characteristic = (uint8)(0xe1 + (c & 3));   // tremolo, vibrato, sustaining
		writes.push_back(RegisterWrite{0.0, (uint16)(0x20 + mod), characteristic});
		writes.push_back(RegisterWrite{0.0, (uint16)(0x20 + car), characteristic});
		writes.push_back(RegisterWrite{0.0, (uint16)(0x40 + mod), (uint8)(0x10 + c * 2)});
		writes.push_back(RegisterWrite{0.0, (uint16)(0x40 + car), (uint8)(c & 3)});
		writes.push_back(RegisterWrite{0.0, (uint16)(0x60 + mod), (uint8)(0xd0 | (c & 7))});
		writes.push_back(RegisterWrite{0.0, (uint16)(0x60 + car), (uint8)(0xd0 | (c & 7))});
		writes.push_back(RegisterWrite{0.0, (uint16)(0x80 + mod), (uint8)(0x30 | (c & 0xf))});
		writes.push_back(RegisterWrite{0.0, (uint16)(0x80 + car), (uint8)(0x30 | (c & 0xf))});
		writes.push_back(RegisterWrite{0.0, (uint16)(0xe0 + mod), (uint8)(c & 3)});
		writes.push_back(RegisterWrite{0.0, (uint16)(0xe0 + car), (uint8)(c & 3)});
		writes.push_back(RegisterWrite{0.0, (uint16)(0xc0 + c), (uint8)(((1 + c % 7) << 1) | 0)});
		const uint8 block = (uint8)(2 + c % 4);
		writes.push_back(RegisterWrite{0.0, (uint16)(0xa0 + c), (uint8)(chord[c] & 0xff)});
		writes.push_back(RegisterWrite{0.0, (uint16)(0xb0 + c), (uint8)(0x20 | (block << 2) | (chord[c] >> 8))});
	}
	// A retrigger and a patch change mid-run, so events land in later chunks.
	for (uint8 c = 0; c < 9; ++c) {
		const uint8 block = (uint8)(2 + c % 4);
		writes.push_back(RegisterWrite{seconds * 0.5, (uint16)(0xb0 + c), (uint8)((block << 2) | (chord[c] >> 8))});
		writes.push_back(RegisterWrite{seconds * 0.5 + 0.01, (uint16)(0x40 + kModOffset[c]), (uint8)(0x20 + c)});
		writes.push_back(RegisterWrite{seconds * 0.5 + 0.02, (uint16)(0xb0 + c), (uint8)(0x20 | (block << 2) | (chord[c] >> 8))});
	}
	return writes;
}

// The stress case, then the paths a musical patch rarely takes, so that the
// DSP's are compared word for word too. Not a cost measurement.
std::vector<RegisterWrite> pathsScript(double seconds) {
	std::vector<RegisterWrite> writes = stressScript(seconds);
	// Rate changes during attack must hold, but the later key-on at maximum
	// rate is still instant. Also pause and resume while all voices are live.
	writes.push_back(RegisterWrite{seconds * 0.10, 0xb0, 0x09});
	// The stress patch has release rate zero: give it time to attenuate
	// before rekeying, so the slow attack starts above zero attenuation.
	writes.push_back(RegisterWrite{seconds * 0.10, 0x80, 0x3f});
	writes.push_back(RegisterWrite{seconds * 0.10, 0x83, 0x3f});
	writes.push_back(RegisterWrite{seconds * 0.10, 0x60, 0x30});
	writes.push_back(RegisterWrite{seconds * 0.10, 0x63, 0x30});
	writes.push_back(RegisterWrite{seconds * 0.11, 0xb0, 0x29});
	writes.push_back(RegisterWrite{seconds * 0.15, 0x60, 0xf0});
	writes.push_back(RegisterWrite{seconds * 0.15, 0x63, 0x00});
	writes.push_back(RegisterWrite{seconds * 0.30, kPauseChip, 1});
	writes.push_back(RegisterWrite{seconds * 0.40, kPauseChip, 0});
	static const uint16 chord[9] = { 0x181, 0x1e5, 0x241, 0x2aa, 0x181, 0x1e5, 0x241, 0x2aa, 0x33d };
	// Channel 8's carrier decays past a sustain level that is then lowered
	// under it (the decay must run on) and channel 7's is lowered onto its
	// envelope (it must hold there); channel 6 plays above half the chip's
	// phase range, where the increment is negative, with vibrato across it.
	double at = seconds * 0.6;
	for (uint8 c = 7; c < 9; ++c) {
		const uint8 car = (uint8)(kModOffset[c] + 3);
		writes.push_back(RegisterWrite{at, (uint16)(0xb0 + c), 0x0a});
		writes.push_back(RegisterWrite{at, (uint16)(0x60 + car), (uint8)(c == 8 ? 0xf8 : 0xf3)});
		writes.push_back(RegisterWrite{at, (uint16)(0x80 + car), 0xf4});
		writes.push_back(RegisterWrite{at + 0.005, (uint16)(0xb0 + c), 0x2a});
		writes.push_back(RegisterWrite{at + 0.035, (uint16)(0x80 + car), (uint8)(c == 8 ? 0x14 : 0x04)});
	}
	for (int which = 0; which < 2; ++which)
		writes.push_back(RegisterWrite{at, (uint16)(0x20 + kModOffset[6] + 3 * which), 0x6f});
	writes.push_back(RegisterWrite{at, 0xa6, 0xf0});
	writes.push_back(RegisterWrite{at, 0xb6, 0x3d});        // f-number 0x1f0, block 7, multiplier 15
	writes.push_back(RegisterWrite{at + 0.1, 0xa6, 0x11});
	writes.push_back(RegisterWrite{at + 0.1, 0xb6, 0x3d});   // 0x111: the deep vibrato straddles half the range
	// A reset under held notes, and a song started on the reset chip.
	at = seconds * 0.8;
	writes.push_back(RegisterWrite{at, kResetChip, 0});
	writes.push_back(RegisterWrite{at, 0x01, 0x20});
	for (uint8 c = 0; c < 9; c += 2) {
		const uint8 mod = kModOffset[c];
		const uint8 car = (uint8)(mod + 3);
		writes.push_back(RegisterWrite{at, (uint16)(0x20 + mod), 0x01});
		writes.push_back(RegisterWrite{at, (uint16)(0x20 + car), 0x01});
		writes.push_back(RegisterWrite{at, (uint16)(0x40 + mod), 0x00});   // zeros a stale shadow would swallow
		writes.push_back(RegisterWrite{at, (uint16)(0x40 + car), 0x00});
		writes.push_back(RegisterWrite{at, (uint16)(0x60 + mod), 0xf2});
		writes.push_back(RegisterWrite{at, (uint16)(0x60 + car), 0xf2});
		writes.push_back(RegisterWrite{at, (uint16)(0x80 + mod), 0x24});
		writes.push_back(RegisterWrite{at, (uint16)(0x80 + car), 0x24});
		writes.push_back(RegisterWrite{at, (uint16)(0xc0 + c), (uint8)(c << 1)});
		writes.push_back(RegisterWrite{at + 0.01 * c, (uint16)(0xa0 + c), (uint8)(chord[c] & 0xff)});
		writes.push_back(RegisterWrite{at + 0.01 * c, (uint16)(0xb0 + c), (uint8)(0x30 | (chord[c] >> 8))});
	}
	// Writes are decoded in time order.
	for (size_t i = 1; i < writes.size(); ++i)
		for (size_t j = i; j > 0 && writes[j].seconds < writes[j - 1].seconds; --j) {
			const RegisterWrite moved = writes[j];
			writes[j] = writes[j - 1];
			writes[j - 1] = moved;
		}
	return writes;
}

// Rhythm mode at its most expensive - six feedback FM channels beside a
// bass drum in feedback FM and all four single-operator drums held, with
// vibrato on the hi-hat's and the cymbal's oscillators - and then every
// shape the rhythm section takes: each drum alone, all of them silent, the
// bass drum in each connection, the mode left and entered under sounding
// notes, a reset under it, and an OPL2's waveform select enable.
std::vector<RegisterWrite> rhythmScript(double seconds) {
	std::vector<RegisterWrite> writes;
	const std::vector<RegisterWrite> melodic = stressScript(seconds);
	for (size_t i = 0; i < melodic.size(); ++i) {
		// channels zero to five of the stress case; the mid-run changes too
		const uint16 reg = melodic[i].reg;
		const int slotOffset = reg & 0x1f;
		const bool rhythmSlot = reg >= 0x20 && reg < 0xa0 ? slotOffset >= 0x10 : false;
		const bool rhythmWave = reg >= 0xe0 && slotOffset >= 0x10;
		const bool rhythmChannel = reg >= 0xa0 && reg < 0xd0 && reg != 0xbd && (reg & 0x0f) >= 6;
		if (!rhythmSlot && !rhythmWave && !rhythmChannel)
			writes.push_back(melodic[i]);
	}
	for (uint8 ch = 6; ch < 9; ++ch)
		for (uint8 which = 0; which < 2; ++which) {
			const uint8 op = (uint8)(kModOffset[ch] + 3 * which);
			writes.push_back(RegisterWrite{0.0, (uint16)(0x20 + op), (uint8)(0xe1 + ((ch + which) & 3))});
			writes.push_back(RegisterWrite{0.0, (uint16)(0x40 + op), (uint8)((ch == 6 && !which) ? 0x12 : 2 * which)});
			writes.push_back(RegisterWrite{0.0, (uint16)(0x60 + op), 0xf3});
			writes.push_back(RegisterWrite{0.0, (uint16)(0x80 + op), 0x2d});
			writes.push_back(RegisterWrite{0.0, (uint16)(0xe0 + op), (uint8)((ch + which) & 3)});
		}
	writes.push_back(RegisterWrite{0.0, 0xc6, 0x0a});
	writes.push_back(RegisterWrite{0.0, 0xa6, 0x57});
	writes.push_back(RegisterWrite{0.0, 0xb6, 0x09});
	writes.push_back(RegisterWrite{0.0, 0xa7, 0x03});
	writes.push_back(RegisterWrite{0.0, 0xb7, 0x0e});
	writes.push_back(RegisterWrite{0.0, 0xa8, 0x57});
	writes.push_back(RegisterWrite{0.0, 0xb8, 0x0d});
	writes.push_back(RegisterWrite{0.0, 0xbd, 0xff});
	// all released, to silence; then one at a time
	writes.push_back(RegisterWrite{seconds * 0.30, 0xbd, 0xe0});
	static const uint8 alone[4] = { 0x01, 0x08, 0x02, 0x04 };
	for (int i = 0; i < 4; ++i) {
		writes.push_back(RegisterWrite{seconds * (0.40 + 0.05 * i), 0xbd, (uint8)(0xe0 | alone[i])});
		writes.push_back(RegisterWrite{seconds * (0.43 + 0.05 * i), 0xbd, 0xe0});
	}
	// the bass drum under the additive connection, then without feedback
	writes.push_back(RegisterWrite{seconds * 0.60, 0xc6, 0x0b});
	writes.push_back(RegisterWrite{seconds * 0.60, 0xbd, 0xf0});
	writes.push_back(RegisterWrite{seconds * 0.63, 0xbd, 0xe0});
	writes.push_back(RegisterWrite{seconds * 0.65, 0xc6, 0x00});
	writes.push_back(RegisterWrite{seconds * 0.65, 0xbd, 0xf0});
	writes.push_back(RegisterWrite{seconds * 0.68, 0xbd, 0xff});
	// the mode left under the drums, the channels keyed as melodic voices,
	// and the mode entered again over them
	writes.push_back(RegisterWrite{seconds * 0.72, 0xbd, 0xdf});
	for (uint8 ch = 6; ch < 9; ++ch)
		writes.push_back(RegisterWrite{seconds * 0.74, (uint16)(0xb0 + ch), (uint8)(0x29 + ch)});
	writes.push_back(RegisterWrite{seconds * 0.78, 0xbd, 0xff});
	for (uint8 ch = 6; ch < 9; ++ch)
		writes.push_back(RegisterWrite{seconds * 0.80, (uint16)(0xb0 + ch), (uint8)(0x09 + ch)});
	// A reset under it all. The OPL2's waveform select enable is clear again:
	// the waveforms written now are held and the drums play sines, until
	// register 1 brings the selection into force.
	double at = seconds * 0.84;
	writes.push_back(RegisterWrite{at, kResetChip, 0});
	for (uint8 ch = 6; ch < 9; ++ch)
		for (uint8 which = 0; which < 2; ++which) {
			const uint8 op = (uint8)(kModOffset[ch] + 3 * which);
			writes.push_back(RegisterWrite{at, (uint16)(0x20 + op), 0x21});
			writes.push_back(RegisterWrite{at, (uint16)(0x40 + op), 0x00});
			writes.push_back(RegisterWrite{at, (uint16)(0x60 + op), 0xf2});
			writes.push_back(RegisterWrite{at, (uint16)(0x80 + op), 0x16});
			writes.push_back(RegisterWrite{at, (uint16)(0xe0 + op), (uint8)(1 + ((ch + which) % 3))});
		}
	writes.push_back(RegisterWrite{at, 0xa6, 0x57});
	writes.push_back(RegisterWrite{at, 0xb6, 0x09});
	writes.push_back(RegisterWrite{at, 0xa7, 0x03});
	writes.push_back(RegisterWrite{at, 0xb7, 0x0a});
	writes.push_back(RegisterWrite{at, 0xa8, 0x57});
	writes.push_back(RegisterWrite{at, 0xb8, 0x09});
	writes.push_back(RegisterWrite{at, 0xbd, 0x3f});
	writes.push_back(RegisterWrite{seconds * 0.92, 0x01, 0x20});
	for (size_t i = 1; i < writes.size(); ++i)
		for (size_t j = i; j > 0 && writes[j].seconds < writes[j - 1].seconds; --j) {
			const RegisterWrite moved = writes[j];
			writes[j] = writes[j - 1];
			writes[j - 1] = moved;
		}
	return writes;
}

// Unequal attacks, feedback behind a silent carrier, and rhythm entered
// and left under held keys. Later writes expose the previously silent
// oscillators, so stopping or double-stepping one changes the output.
std::vector<RegisterWrite> phaseScript(double seconds) {
	std::vector<RegisterWrite> writes;
	for (int c = 0; c < 9; ++c) {
		for (int which = 0; which < 2; ++which) {
			const int offset = kModOffset[c] + 3 * which;
			writes.push_back(RegisterWrite{0, (uint16)(0x20 + offset), 0x61});
			const uint8 attack = (c & 1) == which ? 0xf0 : (c < 2 ? 0x30 : 0);
			writes.push_back(RegisterWrite{0, (uint16)(0x60 + offset), attack});
			if (!attack)
				writes.push_back(RegisterWrite{seconds * 0.25, (uint16)(0x60 + offset), 0x70});
		}
		writes.push_back(RegisterWrite{0, (uint16)(0xc0 + c), (uint8)(c < 2 ? 1 : 0x0a | (c & 1))});
		writes.push_back(RegisterWrite{0, (uint16)(0xa0 + c), 0x41});
		writes.push_back(RegisterWrite{0, (uint16)(0xb0 + c), 0x32});
		writes.push_back(RegisterWrite{seconds * 0.75, (uint16)(0xc0 + c), 0x0b});
	}
	writes.push_back(RegisterWrite{seconds * 0.1, 0xbd, 0x3f});
	writes.push_back(RegisterWrite{seconds * 0.6, 0xbd, 0});
	for (size_t i = 1; i < writes.size(); ++i)
		for (size_t j = i; j > 0 && writes[j].seconds < writes[j - 1].seconds; --j) {
			const RegisterWrite moved = writes[j];
			writes[j] = writes[j - 1];
			writes[j - 1] = moved;
		}
	return writes;
}

std::vector<RegisterWrite> traceScript(const char *path, double seconds, double from) {
	std::vector<RegisterWrite> writes;
	FILE *file = std::fopen(path, "r");
	if (!file)
		fail("cannot open the trace");
	char line[256];
	// Writes before the window only leave their register image, applied once
	// at its start: replayed in an instant they would be key-on edges.
	int image[512];
	for (int i = 0; i < 512; ++i)
		image[i] = -1;
	bool started = false;
	while (std::fgets(line, sizeof(line), file)) {
		if (line[0] != 'W')
			continue;
		unsigned long long when;
		char context;
		unsigned reg, value;
		if (std::sscanf(line, "W %llu %c %x %x", &when, &context, &reg, &value) != 4)
			continue;
		const double at = when / 1000000.0 - from;
		if (at > seconds)
			break;
		if (at < 0.0) {
			image[reg & 0x1ff] = (int)value;
			continue;
		}
		if (!started) {
			for (int i = 0; i < 512; ++i)
				if (image[i] >= 0)
					writes.push_back(RegisterWrite{0.0, (uint16)i, (uint8)image[i]});
			started = true;
		}
		writes.push_back(RegisterWrite{at, (uint16)reg, (uint8)value});
	}
	std::fclose(file);
	return writes;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 4) {
		std::fprintf(stderr, "usage: opl-rt-fixture <trace|stress|paths|rhythm|phase> <opldata.bin> <expect.bin>"
		                     " [--trace file] [--seconds N] [--chunk-blocks N]\n");
		return 2;
	}
	const std::string scenario = argv[1];
	const char *trace = nullptr;
	const char *play = nullptr;
	double seconds = 4.0, from = 0.0;
	// The bench output area holds 4,096 frames.
	const uint32 kMaxChunkBlocks = 4096 / P::kBlockFrames;
	uint32 chunkBlocks = kMaxChunkBlocks;
	for (int i = 4; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--trace") && i + 1 < argc)
			trace = argv[++i];
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--from") && i + 1 < argc)
			from = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--chunk-blocks") && i + 1 < argc)
			chunkBlocks = (uint32)std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--play") && i + 1 < argc)
			play = argv[++i];
	}
	if (chunkBlocks < 1 || chunkBlocks > kMaxChunkBlocks)
		fail("chunk blocks exceed the bench output area");

	std::vector<RegisterWrite> writes;
	if (scenario == "stress")
		writes = stressScript(seconds);
	else if (scenario == "paths")
		writes = pathsScript(seconds);
	else if (scenario == "rhythm")
		writes = rhythmScript(seconds);
	else if (scenario == "phase")
		writes = phaseScript(seconds);
	else if (scenario == "trace" && trace)
		writes = traceScript(trace, seconds, from);
	else
		fail("unknown scenario, or --trace missing");

	const uint32 totalBlocks = (uint32)((uint64)(seconds * OPL_PRACTICAL_CODEC_RATE) / P::kBlockFrames);
	const uint32 chunks = (totalBlocks + chunkBlocks - 1) / chunkBlocks;

	// Decode every register write into block-stamped parameter events.
	RecordingSink sink;
	P::Decoder decoder;
	decoder.reset(&sink, 9);
	for (size_t i = 0; i < writes.size(); ++i) {
		const uint32 block = (uint32)((uint64)(writes[i].seconds * OPL_PRACTICAL_CODEC_RATE) / P::kBlockFrames);
		if (writes[i].reg == kResetChip)
			decoder.reset(&sink, 9, block);
		else if (writes[i].reg == kPauseChip) {
			decoder.flush();
			sink.write(block, P::SC_PAUSED, writes[i].value);
		} else
			decoder.write(block, writes[i].reg, writes[i].value);
	}
	decoder.flush();

	// ---- the DSP image
	Writer out;
	out.file = std::fopen(argv[2], "wb");
	if (!out.file)
		fail("cannot create the data image");
	out.word(kMagic);
	out.word(13);   // upload blocks

	out.word(0); out.word(P::SC_TREMOLO_SHIFT); out.word(4);
	out.word(4); out.word(0); out.word(9); out.word(0x7fffff);

	out.word(0); out.word(P::kGainTable); out.word(512);
	for (int i = 0; i < 512; ++i)
		out.word(kOplGain[i]);

	P::Chip chip;
	P::reset(&chip, 9);
	out.word(0); out.word(P::kOpBase); out.word(P::kSlots * P::kOpStride);
	for (int i = 0; i < P::kSlots; ++i)
		for (int w = 0; w < P::kOpStride; ++w)
			out.word((uint32)chip.op[i].w[w] & 0xffffff);
	out.word(1); out.word(P::kOpBase); out.word(P::kSlots * P::kOpStride);   // phase fractions
	for (int i = 0; i < P::kSlots * P::kOpStride; ++i)
		out.word(0);
	out.word(0); out.word(P::kChannelBase); out.word(P::kChannels * P::kChannelStride);
	for (int i = 0; i < P::kChannels * P::kChannelStride; ++i)
		out.word(0);

	out.word(0); out.word(P::kAttackTable); out.word(64);
	for (int i = 0; i < 64; ++i)
		out.word(kOplAttackBlock[i]);
	out.word(0); out.word(P::kDecayTable); out.word(64);
	for (int i = 0; i < 64; ++i)
		out.word(kOplDecayBlock[i]);
	out.word(0); out.word(P::kVibratoTable); out.word(8);
	for (int i = 0; i < 8; ++i)
		out.word((uint32)P::kVibratoOffset[i]);

	out.word(1); out.word(P::kWaveBase); out.word(P::kWaveforms * 1024);
	for (int wf = 0; wf < P::kWaveforms; ++wf)
		for (uint16 phase = 0; phase < 1024; ++phase)
			out.word((uint32)P::waveSample((uint8)wf, phase) & 0xffffff);

	// rhythm mode's lookups
	out.word(0); out.word(P::kRhythmHiHat); out.word(1024);
	for (uint16 phase = 0; phase < 1024; ++phase)
		out.word((uint32)P::rhythmHiHatRow(phase));
	out.word(1); out.word(P::kRhythmCymbal); out.word(1024);
	for (uint16 phase = 0; phase < 1024; ++phase)
		out.word((uint32)P::rhythmCymbalColumn(phase));
	out.word(0); out.word(P::kRhythmSelect); out.word(32);
	for (int i = 0; i < 32; ++i)
		out.word((uint32)P::rhythmSelect(i));
	out.word(0); out.word(P::kRhythmPhases); out.word(12);
	for (int i = 0; i < 12; ++i)
		out.word((uint32)P::rhythmPhase(i));

	// ---- chunks, and the reference output alongside
	Writer expect;
	expect.file = std::fopen(argv[3], "wb");
	if (!expect.file)
		fail("cannot create the expected output");

	out.word(chunks);
	size_t next = 0;
	uint32 peakEvents = 0;
	// Blocks in which some audible operator took a rarely taken path.
	uint32 decayPastBlocks = 0, decayHeldBlocks = 0, negativeIncrementBlocks = 0, vibratoBlocks = 0;
	uint32 pausedBlocks = 0, attackZeroBlocks = 0, attackMaxBlocks = 0;
	// Blocks by the shape the rhythm section took, and with an OPL2 waveform held back.
	uint32 drumBlocks = 0, drumSilentBlocks = 0, tomBlocks = 0, bassCarrierBlocks = 0, bassFeedbackBlocks = 0;
	uint32 bassPlainBlocks = 0, drumVibratoBlocks = 0, melodicAfterRhythmBlocks = 0;
	bool rhythmSeen = false;
	int32 frames[P::kBlockFrames];
	for (uint32 chunk = 0; chunk < chunks; ++chunk) {
		const uint32 first = chunk * chunkBlocks;
		const uint32 count = (first + chunkBlocks <= totalBlocks) ? chunkBlocks : totalBlocks - first;
		size_t end = next;
		while (end < sink.events.size() && sink.events[end].block < first + count)
			++end;
		const uint32 eventCount = (uint32)(end - next);
		if (eventCount > kMaxEventsPerChunk)
			fail("a chunk carries more events than the DSP table holds; use smaller chunks");
		if (eventCount > peakEvents)
			peakEvents = eventCount;
		out.word(count);
		out.word(eventCount);
		for (size_t i = next; i < end; ++i)
			if (sink.events[i].block < first)
				fail("events are not in block order");
		for (size_t i = next; i < end; ++i) {
			out.word(((sink.events[i].block - first) << 16) | sink.events[i].address);
			out.word((uint32)sink.events[i].value & 0xffffff);
		}
		// the reference consumes the same events at the same blocks
		size_t at = next;
		for (uint32 b = 0; b < count; ++b) {
			while (at < end && sink.events[at].block == first + b) {
				P::poke(&chip, sink.events[at].address, sink.events[at].value);
				++at;
			}
			bool past = false, held = false, negative = false, vibrato = false;
			bool attackZero = false, attackMax = false;
			for (int i = 0; i < 18; ++i) {
				const int32 *w = chip.op[i].w;
				if (w[P::OP_STATE] == P::kDecay && w[P::OP_ENV] >= w[P::OP_SL] + (16 << 12))
					past = true;
				if (w[P::OP_STATE] == P::kDecay && w[P::OP_ENV] > w[P::OP_SL] && w[P::OP_ENV] < w[P::OP_SL] + (16 << 12))
					held = true;
				if (w[P::OP_STATE] == P::kAttack && w[P::OP_ENV] > 0 && (w[P::OP_FLAGS] & 1)) {
					attackZero |= w[P::OP_RATE_A] == 0;
					attackMax |= w[P::OP_RATE_A] >= 60;
				}
			}
			P::renderBlock(&chip, nullptr, frames);
			for (int i = 0; i < 18; ++i) {
				const int32 *w = chip.op[i].w;
				if (w[P::OP_GAIN] && w[P::OP_INC] < 0)
					negative = true;
				if (w[P::OP_GAIN] && w[P::OP_INC] != w[P::OP_INCBASE])
					vibrato = true;
			}
			if (chip.rhythm && !chip.paused) {
				rhythmSeen = true;
				drumBlocks += chip.ch[P::kChannelDrums].w[P::CH_MODE] == P::kModeDrums;
				drumSilentBlocks += chip.ch[P::kChannelDrums].w[P::CH_MODE] == P::kModeDrumsSilent;
				tomBlocks += chip.ch[P::kChannelTom].w[P::CH_MODE] == P::kModeTom;
				bassCarrierBlocks += chip.ch[P::kChannelBass].w[P::CH_MODE] == P::kModeBassCarrier
				                     || chip.ch[P::kChannelBass].w[P::CH_MODE] == P::kModeBassAddFeedback;
				bassFeedbackBlocks += chip.ch[P::kChannelBass].w[P::CH_MODE] == P::kModeBassFmFeedback;
				bassPlainBlocks += chip.ch[P::kChannelBass].w[P::CH_MODE] == P::kModeBassFmPlain;
				drumVibratoBlocks += chip.ch[P::kChannelDrums].w[P::CH_MODE] == P::kModeDrums
				                     && chip.hiHatInc != chip.op[P::kOpHiHat].w[P::OP_INCBASE];
			} else if (!chip.paused && rhythmSeen) {
				// the mode left under sounding notes: channel seven is melodic again
				melodicAfterRhythmBlocks += chip.ch[P::kChannelDrums].w[P::CH_MODE] != P::kModeSkip;
			}
			decayPastBlocks += past;
			decayHeldBlocks += held;
			negativeIncrementBlocks += negative;
			vibratoBlocks += vibrato;
			pausedBlocks += chip.paused != 0;
			attackZeroBlocks += attackZero && !chip.paused;
			attackMaxBlocks += attackMax && !chip.paused;
			for (int i = 0; i < P::kBlockFrames; ++i)
				expect.word((uint32)frames[i] & 0xffffff);
		}
		next = end;
	}
	std::fclose(out.file);
	std::fclose(expect.file);

	// ---- stream-mode periods and their checksum, from a fresh reference
	const uint32 periodBlocks = OPL_PRACTICAL_PERIOD_BLOCKS;
	const uint32 periods = totalBlocks / periodBlocks;
	uint32 checksum = 0;
	if (play) {
		Writer pl;
		pl.file = std::fopen(play, "wb");
		if (!pl.file)
			fail("cannot create the play data");
		pl.word(0x4F504C50);
		pl.word(periods);
		P::Chip stream;
		P::reset(&stream, 9);
		size_t at = 0;
		for (uint32 period = 0; period < periods; ++period) {
			const uint32 first = period * periodBlocks;
			size_t end = at;
			while (end < sink.events.size() && sink.events[end].block < first + periodBlocks)
				++end;
			pl.word((uint32)(end - at));
			for (size_t i = at; i < end; ++i) {
				pl.word(((sink.events[i].block - first) << 16) | sink.events[i].address);
				pl.word((uint32)sink.events[i].value & 0xffffff);
			}
			pl.word(0);   // silent PCM
			for (uint32 b = 0; b < periodBlocks; ++b) {
				while (at < end && sink.events[at].block == first + b) {
					P::poke(&stream, sink.events[at].address, sink.events[at].value);
					++at;
				}
				P::renderBlock(&stream, nullptr, frames);
				for (int i = 0; i < P::kBlockFrames; ++i)
					checksum = (checksum + ((uint32)frames[i] & 0xffffff)) & 0xffffff;
			}
			at = end;
		}
		std::fclose(pl.file);
	}

	std::printf("{\"scenario\": \"%s\", \"block_frames\": %d, \"period_blocks\": %d,"
	            " \"seconds\": %.3f, \"blocks\": %u, \"frames\": %u, \"chunks\": %u,"
	            " \"chunk_blocks\": %u, \"register_writes\": %zu, \"parameter_events\": %zu,"
	            " \"peak_events_per_chunk\": %u, \"periods\": %u, \"period_checksum\": %u,"
	            " \"blocks_decaying_past_sustain_level\": %u, \"blocks_entering_sustain_above_level\": %u,"
	            " \"blocks_with_negative_increment\": %u, \"blocks_with_vibrato\": %u,"
	            " \"blocks_paused\": %u, \"blocks_holding_attack_zero\": %u, \"blocks_holding_attack_max\": %u,"
	            " \"blocks_with_drums\": %u, \"blocks_with_silent_drums\": %u, \"blocks_with_tom\": %u,"
	            " \"blocks_with_bass_carrier_alone\": %u, \"blocks_with_bass_feedback_fm\": %u,"
	            " \"blocks_with_bass_plain_fm\": %u, \"blocks_with_drum_vibrato\": %u,"
	            " \"blocks_melodic_after_rhythm\": %u}\n",
	            scenario.c_str(), (int)P::kBlockFrames, (int)OPL_PRACTICAL_PERIOD_BLOCKS,
	            seconds, totalBlocks, totalBlocks * P::kBlockFrames, chunks, chunkBlocks,
	            writes.size(), sink.events.size(), peakEvents, play ? periods : 0, checksum,
	            decayPastBlocks, decayHeldBlocks, negativeIncrementBlocks, vibratoBlocks,
	            pausedBlocks, attackZeroBlocks, attackMaxBlocks,
	            drumBlocks, drumSilentBlocks, tomBlocks, bassCarrierBlocks, bassFeedbackBlocks, bassPlainBlocks,
	            drumVibratoBlocks, melodicAfterRhythmBlocks);
	return 0;
}
