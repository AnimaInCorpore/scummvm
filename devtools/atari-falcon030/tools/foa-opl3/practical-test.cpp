// Render the same register scripts through the exact kernel (49,716 Hz) and
// the practical block-rate kernel (the codec's 49,170 Hz), and write both as
// raw PCM for practical-gate.py to score, with each script's notes (key-on,
// key-off and the carrier's frequency) so that every patch gets its own
// spectral check. Nothing here measures a Falcon.
//
// usage: opl-practical-test <output-dir> [--trace opl-writes.ev] [--seconds N] [--wav]
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

const double kNativeRate = 49716.0;
const double kCodecRate = OPL_PRACTICAL_CODEC_RATE;

struct Event {
	double seconds;
	uint16 reg;
	uint8 value;
};

struct Note {
	double on, off, hz;
};

struct Script {
	std::string name;
	std::vector<Event> events;
	std::vector<Note> notes;   // single-voice scripts only: what the gate checks one by one
	uint8 mult[9];             // the carrier's multiplier register, per channel
	bool held;                 // every note is held at a constant level: stationary by construction
	double seconds;
	double at;

	Script(const char *n) : name(n), held(false), seconds(0), at(0) { memset(mult, 0, sizeof(mult)); }
	void write(uint16 reg, uint8 value) { events.push_back(Event{at, reg, value}); }
	void wait(double s) { at += s; }
	void finish(double tail) { seconds = at + tail; }
};

const uint8 kModOffset[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 };

void patch(Script &s, uint8 channel, uint8 characteristic, uint8 modLevel, uint8 carLevel,
           uint8 attackDecay, uint8 sustainRelease, uint8 waveform, uint8 feedbackConnection) {
	const uint8 mod = kModOffset[channel];
	const uint8 car = mod + 3;
	s.mult[channel] = characteristic & 0x0f;
	s.write(0x20 + mod, characteristic);
	s.write(0x20 + car, characteristic);
	s.write(0x40 + mod, modLevel);
	s.write(0x40 + car, carLevel);
	s.write(0x60 + mod, attackDecay);
	s.write(0x60 + car, attackDecay);
	s.write(0x80 + mod, sustainRelease);
	s.write(0x80 + car, sustainRelease);
	s.write(0xe0 + mod, waveform);
	s.write(0xe0 + car, waveform);
	s.write(0xc0 + channel, feedbackConnection);
}

// The frequency the chip plays: its 19-bit phase wraps, so an increment past
// half the range is heard as the alias below it.
double noteHz(uint16 fnum, uint8 block, uint8 mult) {
	uint32 inc = (((((uint32)fnum << block) >> 1) * OplKernel::kFreqMultiply[mult]) >> 1) & 0x7ffff;
	if (inc >= 0x40000)
		inc = 0x80000 - inc;
	return inc * kNativeRate / 524288.0;
}

void note(Script &s, uint8 channel, uint16 fnum, uint8 block, double on, double off) {
	s.notes.push_back(Note{s.at, s.at + on, noteHz(fnum, block, s.mult[channel])});
	s.write(0xa0 + channel, (uint8)(fnum & 0xff));
	s.write(0xb0 + channel, (uint8)(0x20 | (block << 2) | (fnum >> 8)));
	s.wait(on);
	s.write(0xb0 + channel, (uint8)((block << 2) | (fnum >> 8)));
	s.wait(off);
}

std::vector<Script> scenarios() {
	std::vector<Script> all;

	{
		Script s("tone");
		s.write(0x01, 0x20);
		patch(s, 0, 0x01, 0x18, 0x00, 0xf2, 0x14, 0, 0x00);
		note(s, 0, 0x241, 4, 1.5, 0.5);
		s.finish(0.0);
		all.push_back(s);
	}
	{
		Script s("pitch");
		s.write(0x01, 0x20);
		patch(s, 0, 0x01, 0x20, 0x00, 0xf4, 0x13, 0, 0x00);
		static const uint16 fnums[6] = { 0x181, 0x1b0, 0x1e5, 0x241, 0x2aa, 0x33d };
		for (uint8 block = 2; block < 8; block += 2)
			for (int f = 0; f < 6; f += 2)
				note(s, 0, fnums[f], block, 0.45, 0.1);
		s.finish(0.0);
		all.push_back(s);
	}
	{
		Script s("envelope");
		s.write(0x01, 0x20);
		static const uint8 ad[5] = { 0xf3, 0xa5, 0x74, 0xf8, 0x51 };
		static const uint8 sr[5] = { 0x24, 0x63, 0x18, 0xa2, 0x35 };
		for (int i = 0; i < 5; ++i) {
			patch(s, 0, 0x01, 0x1c, 0x00, ad[i], sr[i], 0, 0x00);
			note(s, 0, 0x241, 4, 0.7, 0.5);
			patch(s, 0, 0x21, 0x1c, 0x00, ad[i], sr[i], 0, 0x00);   // sustaining type
			note(s, 0, 0x241, 4, 0.7, 0.5);
		}
		s.finish(0.0);
		all.push_back(s);
	}
	{
		Script s("waveforms");
		s.write(0x01, 0x20);
		for (uint8 con = 0; con < 2; ++con)
			for (uint8 wf = 0; wf < 4; ++wf) {
				patch(s, 0, 0x01, con ? 0x08 : 0x14, 0x00, 0xf2, 0x14, wf, (uint8)(0x04 | con));
				note(s, 0, 0x241, 4, 0.5, 0.2);
			}
		s.finish(0.0);
		all.push_back(s);
	}
	{
		// Every feedback depth against bright to mellow modulator levels, held
		// at full level: depths 5 to 7 under a loud modulator leave the
		// periodic regime, which the gate measures on the exact chip itself.
		Script s("feedback");
		s.held = true;
		s.write(0x01, 0x20);
		static const uint8 levels[4] = { 0, 8, 16, 32 };
		for (int level = 0; level < 4; ++level)
			for (uint8 fb = 0; fb < 8; ++fb) {
				patch(s, 0, 0x21, levels[level], 0x00, 0xf0, 0x0f, 0, (uint8)(fb << 1));
				note(s, 0, 0x1e5, 3, 0.65, 0.1);
			}
		s.finish(0.0);
		all.push_back(s);
	}
	{
		// A pure sine through every multiplier at the top block: from 5 up the
		// pitch is above the partial band, from 8 up the chip's phase wraps
		// and it plays the alias.
		Script s("high-pitch");
		s.held = true;
		s.write(0x01, 0x20);
		for (uint8 mult = 0; mult < 16; ++mult) {
			patch(s, 0, (uint8)(0x20 | mult), 0x3f, 0x00, 0xf0, 0x0f, 0, 0x00);
			note(s, 0, 0x241, 7, 0.65, 0.1);
		}
		s.finish(0.0);
		all.push_back(s);
	}
	{
		Script s("tremolo");
		s.write(0x01, 0x20);
		for (uint8 deep = 0; deep < 2; ++deep) {
			s.write(0xbd, (uint8)(deep << 7));
			patch(s, 0, 0xa1, 0x3f, 0x00, 0xf2, 0x14, 0, 0x00);   // silent modulator, sustaining: a held sine
			note(s, 0, 0x241, 4, 2.5, 0.3);
		}
		s.finish(0.0);
		all.push_back(s);
	}
	{
		Script s("vibrato");
		s.write(0x01, 0x20);
		for (uint8 deep = 0; deep < 2; ++deep) {
			s.write(0xbd, (uint8)(deep << 6));
			patch(s, 0, 0x61, 0x3f, 0x00, 0xf2, 0x14, 0, 0x00);   // silent modulator, sustaining: a held sine
			note(s, 0, 0x2aa, 4, 2.5, 0.3);
		}
		// An f-number whose top three bits shift away at the shallow depth: the
		// chip plays it with the vibrato bit set and no vibrato at all.
		s.write(0xbd, 0x00);
		note(s, 0, 0x0c8, 5, 2.5, 0.3);
		s.finish(0.0);
		all.push_back(s);
	}
	{
		Script s("polyphony");
		s.write(0x01, 0x20);
		static const uint16 chord[9] = { 0x181, 0x1e5, 0x241, 0x2aa, 0x181, 0x1e5, 0x241, 0x2aa, 0x33d };
		for (uint8 c = 0; c < 9; ++c)
			patch(s, c, (uint8)(0x21 + (c & 3)), (uint8)(0x10 + c * 3), (uint8)(c & 7),
			      (uint8)(0xd0 | (c & 7)), (uint8)(0x30 | (c & 0xf)), (uint8)(c & 3),
			      (uint8)(((c & 7) << 1) | (c == 4 ? 1 : 0)));
		for (int round = 0; round < 3; ++round) {
			for (uint8 c = 0; c < 9; ++c) {
				const uint8 block = (uint8)(2 + (c + round) % 4);
				s.write(0xa0 + c, (uint8)(chord[c] & 0xff));
				s.write(0xb0 + c, (uint8)(0x20 | (block << 2) | (chord[c] >> 8)));
				s.wait(0.08);
			}
			s.wait(0.6);
			for (uint8 c = 0; c < 9; ++c) {
				const uint8 block = (uint8)(2 + (c + round) % 4);
				s.write(0xb0 + c, (uint8)((block << 2) | (chord[c] >> 8)));
				s.wait(0.05);
			}
			s.wait(0.4);
		}
		s.finish(0.0);
		all.push_back(s);
	}
	return all;
}

bool loadTrace(const char *path, double seconds, Script &s) {
	FILE *file = std::fopen(path, "r");
	if (!file)
		return false;
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
		s.events.push_back(Event{at, (uint16)reg, (uint8)value});
	}
	std::fclose(file);
	s.seconds = seconds;
	return true;
}

void writePcm(const std::string &path, const std::vector<int16> &pcm) {
	FILE *file = std::fopen(path.c_str(), "wb");
	if (!file) {
		std::fprintf(stderr, "cannot create %s\n", path.c_str());
		std::exit(1);
	}
	for (size_t i = 0; i < pcm.size(); ++i) {
		std::fputc(pcm[i] & 0xff, file);
		std::fputc((pcm[i] >> 8) & 0xff, file);
	}
	std::fclose(file);
}

void writeWav(const std::string &path, const std::vector<int16> &pcm, uint32 rate) {
	FILE *file = std::fopen(path.c_str(), "wb");
	if (!file)
		return;
	const uint32 dataBytes = (uint32)pcm.size() * 2;
	auto le32 = [&](uint32 v) { for (int i = 0; i < 4; ++i) std::fputc((v >> (8 * i)) & 0xff, file); };
	auto le16 = [&](uint32 v) { for (int i = 0; i < 2; ++i) std::fputc((v >> (8 * i)) & 0xff, file); };
	std::fwrite("RIFF", 1, 4, file); le32(36 + dataBytes); std::fwrite("WAVE", 1, 4, file);
	std::fwrite("fmt ", 1, 4, file); le32(16); le16(1); le16(1); le32(rate); le32(rate * 2); le16(2); le16(16);
	std::fwrite("data", 1, 4, file); le32(dataBytes);
	for (size_t i = 0; i < pcm.size(); ++i) {
		std::fputc(pcm[i] & 0xff, file);
		std::fputc((pcm[i] >> 8) & 0xff, file);
	}
	std::fclose(file);
}

// The exact kernel, writes applied at their native sample.
std::vector<int16> renderExact(const Script &s) {
	OplKernel::Chip chip;
	OplKernel::reset(&chip, 9);
	const uint64 total = (uint64)(s.seconds * kNativeRate);
	std::vector<int16> pcm;
	pcm.reserve(total);
	size_t next = 0;
	int16 left, right;
	for (uint64 sample = 0; sample < total; ++sample) {
		while (next < s.events.size() && (uint64)(s.events[next].seconds * kNativeRate) <= sample) {
			OplKernel::writeRegister(&chip, s.events[next].reg, s.events[next].value);
			++next;
		}
		OplKernel::generate(&chip, &left, &right);
		pcm.push_back(left);
	}
	return pcm;
}

// The practical kernel, writes applied at the boundary of their block.
std::vector<int16> renderPractical(const Script &s, unsigned long long *writesOut) {
	OplPractical::Chip chip;
	OplPractical::reset(&chip, 9);
	OplPractical::DirectSink sink(&chip);
	OplPractical::Decoder decoder;
	decoder.reset(&sink, 9);
	const uint64 totalFrames = (uint64)(s.seconds * kCodecRate);
	const uint64 blocks = (totalFrames + OplPractical::kBlockFrames - 1) / OplPractical::kBlockFrames;
	std::vector<int16> pcm;
	pcm.reserve(blocks * OplPractical::kBlockFrames);
	size_t next = 0;
	int32 out[OplPractical::kBlockFrames];
	for (uint64 block = 0; block < blocks; ++block) {
		while (next < s.events.size()
		       && (uint64)(s.events[next].seconds * kCodecRate) / OplPractical::kBlockFrames <= block) {
			decoder.write((uint32)block, s.events[next].reg, s.events[next].value);
			++next;
		}
		OplPractical::renderBlock(&chip, nullptr, out);
		for (int i = 0; i < OplPractical::kBlockFrames; ++i)
			pcm.push_back((int16)(out[i] >> 8));
	}
	*writesOut = s.events.size();
	return pcm;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: opl-practical-test <output-dir> [--trace file] [--seconds N] [--wav]\n");
		return 2;
	}
	const std::string outDir = argv[1];
	const char *trace = nullptr;
	double seconds = 60.0;
	bool wav = false;
	for (int i = 2; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--trace") && i + 1 < argc)
			trace = argv[++i];
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--wav"))
			wav = true;
	}

	std::vector<Script> all = scenarios();
	if (trace) {
		Script s("atlantis");
		if (!loadTrace(trace, seconds, s)) {
			std::fprintf(stderr, "cannot read trace %s\n", trace);
			return 1;
		}
		all.push_back(s);
	}

	std::printf("[\n");
	for (size_t i = 0; i < all.size(); ++i) {
		const Script &s = all[i];
		std::vector<int16> exact = renderExact(s);
		unsigned long long writes = 0;
		std::vector<int16> practical = renderPractical(s, &writes);
		writePcm(outDir + "/" + s.name + "-exact.pcm", exact);
		writePcm(outDir + "/" + s.name + "-practical.pcm", practical);
		if (wav) {
			writeWav(outDir + "/" + s.name + "-exact.wav", exact, (uint32)kNativeRate);
			writeWav(outDir + "/" + s.name + "-practical.wav", practical, (uint32)(kCodecRate + 0.5));
		}
		std::string notes;
		for (size_t n = 0; n < s.notes.size(); ++n) {
			char text[96];
			std::snprintf(text, sizeof(text), "%s[%.6f, %.6f, %.4f]", n ? ", " : "", s.notes[n].on, s.notes[n].off,
			              s.notes[n].hz);
			notes += text;
		}
		std::printf("  {\"name\": \"%s\", \"seconds\": %.6f, \"writes\": %llu, \"exact_rate\": %.1f,"
		            " \"practical_rate\": %.6f, \"exact_samples\": %zu, \"practical_frames\": %zu,"
		            " \"held\": %s, \"notes\": [%s]}%s\n",
		            s.name.c_str(), s.seconds, writes, kNativeRate, kCodecRate, exact.size(),
		            practical.size(), s.held ? "true" : "false", notes.c_str(), i + 1 < all.size() ? "," : "");
	}
	std::printf("]\n");
	return 0;
}
