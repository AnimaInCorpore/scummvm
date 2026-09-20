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
// usage: opl-rt-fixture <trace|stress|paths> <opldata.bin> <expect.bin>
//                       [--trace opl-writes.ev] [--seconds N] [--chunk-blocks N]
//                       [--play playdata.bin]
//
// With --play the events are also written as stream-mode periods of 15
// blocks with silent PCM ('OPLP', period count, then per period: event
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
	// Pause and resume while all voices are live.
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

std::vector<RegisterWrite> traceScript(const char *path, double seconds) {
	std::vector<RegisterWrite> writes;
	FILE *file = std::fopen(path, "r");
	if (!file)
		fail("cannot open the trace");
	char line[256];
	while (std::fgets(line, sizeof(line), file)) {
		if (line[0] != 'W')
			continue;
		unsigned long long when;
		char context;
		unsigned reg, value;
		if (std::sscanf(line, "W %llu %c %x %x", &when, &context, &reg, &value) != 4)
			continue;
		const double at = when / 1000000.0;
		if (at > seconds)
			break;
		writes.push_back(RegisterWrite{at, (uint16)reg, (uint8)value});
	}
	std::fclose(file);
	return writes;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 4) {
		std::fprintf(stderr, "usage: opl-rt-fixture <trace|stress|paths> <opldata.bin> <expect.bin>"
		                     " [--trace file] [--seconds N] [--chunk-blocks N]\n");
		return 2;
	}
	const std::string scenario = argv[1];
	const char *trace = nullptr;
	const char *play = nullptr;
	double seconds = 4.0;
	// The bench output area holds 4,096 frames.
	const uint32 kMaxChunkBlocks = 4096 / P::kBlockFrames;
	uint32 chunkBlocks = kMaxChunkBlocks;
	for (int i = 4; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--trace") && i + 1 < argc)
			trace = argv[++i];
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = std::atof(argv[++i]);
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
	else if (scenario == "trace" && trace)
		writes = traceScript(trace, seconds);
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
	out.word(9);   // upload blocks

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
	uint32 pausedBlocks = 0;
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
			for (int i = 0; i < 18; ++i) {
				const int32 *w = chip.op[i].w;
				if (w[P::OP_STATE] == P::kDecay && w[P::OP_ENV] >= w[P::OP_SL] + (16 << 12))
					past = true;
				if (w[P::OP_STATE] == P::kDecay && w[P::OP_ENV] > w[P::OP_SL] && w[P::OP_ENV] < w[P::OP_SL] + (16 << 12))
					held = true;
			}
			P::renderBlock(&chip, nullptr, frames);
			for (int i = 0; i < 18; ++i) {
				const int32 *w = chip.op[i].w;
				if (w[P::OP_GAIN] && w[P::OP_INC] < 0)
					negative = true;
				if (w[P::OP_GAIN] && w[P::OP_INC] != w[P::OP_INCBASE])
					vibrato = true;
			}
			decayPastBlocks += past;
			decayHeldBlocks += held;
			negativeIncrementBlocks += negative;
			vibratoBlocks += vibrato;
			pausedBlocks += chip.paused != 0;
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

	std::printf("{\"scenario\": \"%s\", \"seconds\": %.3f, \"blocks\": %u, \"frames\": %u, \"chunks\": %u,"
	            " \"chunk_blocks\": %u, \"register_writes\": %zu, \"parameter_events\": %zu,"
	            " \"peak_events_per_chunk\": %u, \"periods\": %u, \"period_checksum\": %u,"
	            " \"blocks_decaying_past_sustain_level\": %u, \"blocks_entering_sustain_above_level\": %u,"
	            " \"blocks_with_negative_increment\": %u, \"blocks_with_vibrato\": %u,"
	            " \"blocks_paused\": %u}\n",
	            scenario.c_str(), seconds, totalBlocks, totalBlocks * P::kBlockFrames, chunks, chunkBlocks,
	            writes.size(), sink.events.size(), peakEvents, play ? periods : 0, checksum,
	            decayPastBlocks, decayHeldBlocks, negativeIncrementBlocks, vibratoBlocks,
	            pausedBlocks);
	return 0;
}
