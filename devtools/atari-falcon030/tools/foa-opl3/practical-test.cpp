// Render the same register scripts through the exact kernel (49,716 Hz) and
// the practical block-rate kernel (the codec's 49,170 Hz), and write both as
// raw PCM for practical-gate.py to score, with each script's notes (key-on,
// key-off and the carrier's frequency) so that every patch gets its own
// spectral check. Nothing here measures a Falcon.
//
// usage: opl-practical-test <output-dir> [--trace opl-writes.ev] [--seconds N] [--wav]
//                           [--rhythm-trace opl-writes.ev] [--rhythm-from S]
//
// The rhythm trace is a second captured stream, of a game that plays its
// percussion through rhythm mode (Cruise for a Corpse); its window starts
// where the drums do, on the register image the earlier writes left.
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
	// Held rhythm-mode drums, one at a time. hz centres the bands of a pitched
	// drum on its note and is zero for the others; a negative hz marks a drum
	// the gate grades on level alone.
	std::vector<Note> drums;
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
	{
		// Rhythm mode. First every drum alone and held (a sustaining envelope at
		// full level), with two tunings of the hi-hat's and the cymbal's
		// oscillators and the bass drum in both connections, for the spectra;
		// then a pattern of percussive hits over a melodic voice, for the
		// contour and the onsets.
		Script s("rhythm");
		s.write(0x01, 0x20);
		static const uint8 multipliers[2][2] = { { 0x01, 0x01 }, { 0x04, 0x05 } };   // hi-hat, cymbal
		for (int tuning = 0; tuning < 2; ++tuning) {
			for (uint8 ch = 6; ch < 9; ++ch)
				for (uint8 which = 0; which < 2; ++which) {
					const uint8 op = (uint8)(kModOffset[ch] + 3 * which);
					uint8 mult = 0x01;
					if (ch == 7 && !which)
						mult = multipliers[tuning][0];
					if (ch == 8 && which)
						mult = multipliers[tuning][1];
					s.write(0x20 + op, (uint8)(0x20 | mult));
					s.write(0x40 + op, (ch == 6 && !which) ? 0x14 : 0x00);
					s.write(0x60 + op, 0xf0);
					s.write(0x80 + op, 0x09);
					s.write(0xe0 + op, (uint8)(tuning && ch == 7 ? 1 : 0));
				}
			s.write(0xc6, 0x06);
			s.write(0xc7, 0x00);
			s.write(0xc8, 0x00);
			s.write(0xa6, 0x57); s.write(0xb6, 0x09);   // bass drum: f-number 0x157, block 2
			s.write(0xa7, 0x03); s.write(0xb7, 0x0a);   // snare and hi-hat: 0x203, block 2
			s.write(0xa8, 0x57); s.write(0xb8, 0x09);   // tom-tom and cymbal: 0x157, block 2
			static const uint8 keys[6] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 };
			for (int drum = 0; drum < 6; ++drum) {
				if (drum == 5)
					s.write(0xc6, 0x07);   // the additive connection: the carrier alone
				s.write(0xbd, 0x20);
				// The hi-hat and the cymbal are built from their oscillators' phase
				// bits, the fastest of which is a square wave at 128 times the
				// oscillator's pitch: under the second tuning that is past 49 kHz
				// and folds to another frequency at each kernel's own rate, so
				// the two spectra cannot line up and only the level is held.
				const bool pitched = drum >= 2 && drum != 3;
				const bool folds = tuning == 1 && drum < 2;
				s.drums.push_back(Note{s.at, s.at + 1.0, folds ? -1.0 : pitched ? noteHz(0x157, 2, 1) : 0.0});
				s.write(0xbd, (uint8)(0x20 | keys[drum]));
				s.wait(1.0);
				s.write(0xbd, 0x20);
				s.wait(0.25);
			}
		}
		// The pattern: percussive envelopes, the drums keyed in combinations.
		for (uint8 ch = 6; ch < 9; ++ch)
			for (uint8 which = 0; which < 2; ++which) {
				const uint8 op = (uint8)(kModOffset[ch] + 3 * which);
				s.write(0x20 + op, 0x01);
				s.write(0x40 + op, (ch == 6 && !which) ? 0x10 : (ch == 7 && !which) ? 0x08 : 0x02);
				s.write(0x60 + op, (uint8)(0xf0 | (ch == 6 ? 0x6 : ch == 7 ? 0x8 : 0x5)));
				s.write(0x80 + op, (uint8)(0xf0 | (ch == 6 ? 0x6 : ch == 7 ? 0x8 : 0x5)));
				s.write(0xe0 + op, 0x00);
			}
		s.write(0xc6, 0x08);
		patch(s, 0, 0x21, 0x1a, 0x04, 0xf3, 0x36, 0, 0x04);
		static const uint8 pattern[16] = {
			0x11, 0x01, 0x09, 0x01, 0x11, 0x05, 0x09, 0x03, 0x11, 0x01, 0x09, 0x05, 0x15, 0x11, 0x0b, 0x1f
		};
		for (int step = 0; step < 16; ++step) {
			if (!(step & 3)) {
				s.write(0xa0, (uint8)(0x41 + step * 7));
				s.write(0xb0, 0x2e);
			}
			s.write(0xbd, 0x20);
			s.wait(0.004);
			s.write(0xbd, (uint8)(0x20 | pattern[step]));
			s.wait(0.121);
			if ((step & 3) == 3)
				s.write(0xb0, 0x0e);
		}
		s.write(0xbd, 0x20);
		s.finish(0.6);
		all.push_back(s);
	}
	return all;
}

bool loadTrace(const char *path, double seconds, Script &s, double from = 0.0) {
	FILE *file = std::fopen(path, "r");
	if (!file)
		return false;
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
					s.events.push_back(Event{0.0, (uint16)i, (uint8)image[i]});
			started = true;
		}
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

// The exact kernel, writes applied at their native sample - except that a
// key register (0xb0-0xb8, 0xbd) written twice within one sample gets a
// sample between the two writes. A captured trace stamps every write of a
// driver tick with the tick's time, and a sample-exact chip given a key-off
// and a key-on in the same sample never sees the key-off: the note is not
// retriggered and a percussive patch falls silent for good (Cruise for a
// Corpse rekeys every note that way; its music stops after a few bars). On
// the card each port write takes longer than the chip's 20 microsecond
// sample, so the chip does see it, which is also what the practical kernel's
// counted key-on edges give.
std::vector<int16> renderExact(const Script &s) {
	OplKernel::Chip chip;
	OplKernel::reset(&chip, 9);
	chip.opl2WaveformGate = 1;   // an OPL2, as the practical decoder is for nine channels
	const uint64 total = (uint64)(s.seconds * kNativeRate);
	std::vector<int16> pcm;
	pcm.reserve(total);
	size_t next = 0;
	int16 left, right;
	for (uint64 sample = 0; sample < total; ++sample) {
		bool keyWritten[16];
		memset(keyWritten, 0, sizeof(keyWritten));
		while (next < s.events.size() && (uint64)(s.events[next].seconds * kNativeRate) <= sample) {
			const uint16 reg = s.events[next].reg;
			if (reg >= 0xb0 && reg <= 0xbd) {
				if (keyWritten[reg - 0xb0])
					break;   // the rest of this tick's writes follow a sample later
				keyWritten[reg - 0xb0] = true;
			}
			OplKernel::writeRegister(&chip, reg, s.events[next].value);
			++next;
		}
		OplKernel::generate(&chip, &left, &right);
		pcm.push_back(left);
	}
	return pcm;
}

// How far ahead of its time the block boundary applies a write, over every
// write and over the writes that start a note (a key-on edge of any slot).
struct Lead {
	unsigned long long writes, keyOns;
	double writeSum, writeMax, keyOnSum, keyOnMax;
	Lead() : writes(0), keyOns(0), writeSum(0), writeMax(0), keyOnSum(0), keyOnMax(0) {}
	void note(double ms, bool keyOn) {
		++writes;
		writeSum += ms;
		if (ms > writeMax)
			writeMax = ms;
		if (keyOn) {
			++keyOns;
			keyOnSum += ms;
			if (ms > keyOnMax)
				keyOnMax = ms;
		}
	}
};

// The practical kernel, writes applied at the boundary of their block.
std::vector<int16> renderPractical(const Script &s, unsigned long long *writesOut, Lead *lead) {
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
			uint64 before = 0, after = 0;
			for (int i = 0; i < OplPractical::kSlots; ++i)
				before += decoder.slotTrigger[i];
			decoder.write((uint32)block, s.events[next].reg, s.events[next].value);
			for (int i = 0; i < OplPractical::kSlots; ++i)
				after += decoder.slotTrigger[i];
			const double due = s.events[next].seconds * kCodecRate;
			lead->note((due - (double)(block * OplPractical::kBlockFrames)) * 1000.0 / kCodecRate, after != before);
			++next;
		}
		decoder.flush();
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
	const char *rhythmTrace = nullptr;
	double seconds = 60.0, rhythmFrom = 0.0;
	bool wav = false;
	for (int i = 2; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--trace") && i + 1 < argc)
			trace = argv[++i];
		else if (!std::strcmp(argv[i], "--rhythm-trace") && i + 1 < argc)
			rhythmTrace = argv[++i];
		else if (!std::strcmp(argv[i], "--rhythm-from") && i + 1 < argc)
			rhythmFrom = std::atof(argv[++i]);
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
	if (rhythmTrace) {
		Script s("cruise");
		if (!loadTrace(rhythmTrace, seconds, s, rhythmFrom)) {
			std::fprintf(stderr, "cannot read trace %s\n", rhythmTrace);
			return 1;
		}
		all.push_back(s);
	}

	std::printf("[\n");
	for (size_t i = 0; i < all.size(); ++i) {
		const Script &s = all[i];
		std::vector<int16> exact = renderExact(s);
		unsigned long long writes = 0;
		Lead lead;
		std::vector<int16> practical = renderPractical(s, &writes, &lead);
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
		std::string drums;
		for (size_t n = 0; n < s.drums.size(); ++n) {
			char text[64];
			std::snprintf(text, sizeof(text), "%s[%.6f, %.6f, %.4f]", n ? ", " : "", s.drums[n].on, s.drums[n].off,
			              s.drums[n].hz);
			drums += text;
		}
		std::printf("  {\"name\": \"%s\", \"seconds\": %.6f, \"writes\": %llu, \"exact_rate\": %.1f,"
		            " \"practical_rate\": %.6f, \"exact_samples\": %zu, \"practical_frames\": %zu,"
		            " \"block_frames\": %d, \"key_ons\": %llu,"
		            " \"write_lead_ms\": [%.4f, %.4f], \"key_on_lead_ms\": [%.4f, %.4f],"
		            " \"held\": %s, \"notes\": [%s], \"drums\": [%s]}%s\n",
		            s.name.c_str(), s.seconds, writes, kNativeRate, kCodecRate, exact.size(),
		            practical.size(), (int)OplPractical::kBlockFrames, lead.keyOns,
		            lead.writes ? lead.writeSum / lead.writes : 0.0, lead.writeMax,
		            lead.keyOns ? lead.keyOnSum / lead.keyOns : 0.0, lead.keyOnMax,
		            s.held ? "true" : "false", notes.c_str(), drums.c_str(),
		            i + 1 < all.size() ? "," : "");
	}
	std::printf("]\n");
	return 0;
}
