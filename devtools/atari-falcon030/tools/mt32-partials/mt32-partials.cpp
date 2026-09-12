// Measures how many LA32 partials Fate of Atlantis actually asks an MT-32 for.
//
// Plays cue event files exported by foa-mt32-demand.py --export through the
// vendored Munt and records, weighted by time, how many partials and how many
// notes are sounding. That converts "5.63 concurrent notes" into a partial
// count, which is the quantity the Falcon DSP budget is denominated in.
//
// Usage: mt32-partials <roms-dir> <cues-dir> [max-cues]
//        mt32-partials --check-cues <cues-dir>      (no ROM needed)
//        mt32-partials --list-roms                  (what Munt accepts)
//        mt32-partials --probe <roms-dir>           (cost of one note per program)
//
// Munt is the oracle here, not an approximation: the partial allocation this
// reports is the hardware's own behaviour as Munt models it, including its
// voice stealing when the 32-partial pool runs out.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>

#include "mt32emu.h"

namespace {

const uint32_t kBlockFrames = 64;      // partial states sampled once per block
const double kTailSeconds = 8.0;       // render past the last event by at most this
const uint32_t kPartialPool = MT32Emu::DEFAULT_MAX_PARTIALS;

struct Event {
	uint64_t us;
	std::vector<uint8_t> bytes;
};

struct Stats {
	// seconds spent with exactly n partials / n notes sounding
	std::vector<double> partialTime;
	std::vector<double> noteTime;
	double totalSeconds = 0.0;
	double partialSecondsWhenSounding = 0.0;  // sum(partials * dt) where notes > 0
	double noteSecondsWhenSounding = 0.0;     // sum(notes    * dt) where notes > 0
	double soundingSeconds = 0.0;
	uint32_t peakPartials = 0;
	uint32_t peakNotes = 0;
	double stolenSeconds = 0.0;               // time at the pool ceiling

	Stats() : partialTime(kPartialPool + 1, 0.0), noteTime(kPartialPool + 1, 0.0) {}
};

bool readCue(const std::string &path, std::vector<Event> &out) {
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return false;
	char line[4096];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || line[0] == '\n') continue;
		char *sp = strchr(line, ' ');
		if (!sp) continue;
		*sp = 0;
		Event e;
		e.us = strtoull(line, nullptr, 10);
		for (char *p = sp + 1; p[0] && p[1] && p[0] != '\n'; p += 2) {
			if (p[0] == '\n' || p[1] == '\n') break;
			char hex[3] = {p[0], p[1], 0};
			e.bytes.push_back((uint8_t)strtoul(hex, nullptr, 16));
		}
		if (!e.bytes.empty()) out.push_back(e);
	}
	fclose(f);
	return true;
}

// Counts sounding partials and notes as Munt currently has them.
void sample(MT32Emu::Synth &synth, uint32_t &partials, uint32_t &notes) {
	MT32Emu::PartialState states[kPartialPool];
	synth.getPartialStates(states);
	partials = 0;
	for (uint32_t i = 0; i < kPartialPool; ++i)
		if (states[i] != MT32Emu::PartialState_INACTIVE) ++partials;

	notes = 0;
	uint8_t keys[kPartialPool], vels[kPartialPool];
	for (uint8_t part = 0; part < 9; ++part)
		notes += synth.getPlayingNotes(part, keys, vels);
}

void accumulate(Stats &s, uint32_t partials, uint32_t notes, double dt) {
	s.partialTime[std::min(partials, kPartialPool)] += dt;
	s.noteTime[std::min(notes, kPartialPool)] += dt;
	s.totalSeconds += dt;
	s.peakPartials = std::max(s.peakPartials, partials);
	s.peakNotes = std::max(s.peakNotes, notes);
	if (notes > 0) {
		s.soundingSeconds += dt;
		s.partialSecondsWhenSounding += partials * dt;
		s.noteSecondsWhenSounding += notes * dt;
	}
	if (partials >= kPartialPool) s.stolenSeconds += dt;
}

const MT32Emu::ROMImage *loadROM(const std::string &dir, MT32Emu::ROMInfo::Type want,
                                 MT32Emu::FileStream *keep, std::string &nameOut) {
	DIR *d = opendir(dir.c_str());
	if (!d) return nullptr;
	const MT32Emu::ROMImage *found = nullptr;
	struct dirent *ent;
	while ((ent = readdir(d)) != nullptr) {
		if (ent->d_name[0] == '.') continue;
		std::string path = dir + "/" + ent->d_name;
		MT32Emu::FileStream probe;
		if (!probe.open(path.c_str())) continue;
		const MT32Emu::ROMInfo *info = MT32Emu::ROMInfo::getROMInfo(&probe);
		if (!info || info->type != want || info->pairType != MT32Emu::ROMInfo::Full) continue;
		if (!keep->open(path.c_str())) continue;
		found = MT32Emu::ROMImage::makeROMImage(keep);
		nameOut = std::string(info->shortName) + " (" + ent->d_name + ")";
		break;
	}
	closedir(d);
	return found;
}

void report(const Stats &s) {
	if (s.totalSeconds <= 0.0) { printf("no audio rendered\n"); return; }
	printf("\n%.1f min rendered, %.1f min with notes sounding\n",
	       s.totalSeconds / 60.0, s.soundingSeconds / 60.0);
	printf("peak partials %u of %u, peak notes %u\n", s.peakPartials, kPartialPool, s.peakNotes);

	printf("\nTime-weighted ACTIVE PARTIALS (the quantity the DSP budget is in):\n");
	double cum = 0.0;
	for (uint32_t n = 0; n <= kPartialPool; ++n) {
		if (s.partialTime[n] <= 0.0) continue;
		double pct = 100.0 * s.partialTime[n] / s.totalSeconds;
		cum += pct;
		printf("  %2u : %5.1f%%  cum %5.1f%%\n", n, pct, cum);
	}

	if (s.soundingSeconds > 0.0) {
		double meanPartials = s.partialSecondsWhenSounding / s.soundingSeconds;
		double meanNotes = s.noteSecondsWhenSounding / s.soundingSeconds;
		printf("\nmean while sounding: %.2f partials, %.2f notes"
		       "  ->  %.2f partials per note\n", meanPartials, meanNotes,
		       meanNotes > 0.0 ? meanPartials / meanNotes : 0.0);
	}
	printf("Munt itself at the %u-partial ceiling: %.1f%% of the time\n",
	       kPartialPool, 100.0 * s.stolenSeconds / s.totalSeconds);

	printf("\nShare of time fitting each Falcon partial budget"
	       " (la32-budget.md figures):\n");
	const struct { const char *name; uint32_t budget; } budgets[] = {
		{"3  exact, moving saw     ", 3},
		{"4  exact, settled saw    ", 4},
		{"5  exact, settled square ", 5},
		{"5  approx, moving        ", 5},
		{"7  approx, settled saw   ", 7},
		{"8  approx, settled square", 8},
	};
	for (const auto &b : budgets) {
		double fit = 0.0;
		for (uint32_t n = 0; n <= b.budget && n <= kPartialPool; ++n) fit += s.partialTime[n];
		printf("  %s : %5.1f%%\n", b.name, 100.0 * fit / s.totalSeconds);
	}
}

} // namespace

// Prints every ROM image this Munt recognises, so a dump you make yourself can
// be checked before it is used. Munt matches on size and SHA-1, so a dump that
// appears here is the right one and one that does not will be rejected.
static int listROMs() {
	const MT32Emu::ROMInfo **all = MT32Emu::ROMInfo::getROMInfoList(0xFFFFFFFF, 0xFFFFFFFF);
	if (!all) { fprintf(stderr, "no ROM catalogue in this Munt build\n"); return 1; }
	const char *typeName[] = {"PCM", "Control", "Reverb"};
	const char *pairName[] = {"full", "first half", "second half", "mux even", "mux odd"};
	printf("%-9s %-11s %9s  %-40s %s\n", "TYPE", "PAIRING", "BYTES", "SHA-1", "DESCRIPTION");
	size_t n = 0;
	for (const MT32Emu::ROMInfo **r = all; *r != nullptr; ++r, ++n) {
		printf("%-9s %-11s %9zu  %-40s %s\n",
		       typeName[(*r)->type], pairName[(*r)->pairType],
		       (size_t)(*r)->fileSize, (const char *)(*r)->sha1Digest, (*r)->description);
	}
	MT32Emu::ROMInfo::freeROMInfoList(all);
	printf("\n%zu known images. This harness uses \"full\" pairs only: put one\n"
	       "Control and one PCM image in the ROMs directory. Dump them from your\n"
	       "own hardware - they are Roland's firmware and are not redistributable.\n", n);
	return 0;
}

// Measures what one note of each program costs, by playing exactly one note on
// part 1 and reading the partial pool. This is the per-timbre breakdown behind
// the aggregate partials-per-note figure: it says which of the game's timbres
// are expensive, not just what the average is.
static int probePrograms(const std::string &romDir) {
	MT32Emu::FileStream controlFile, pcmFile;
	std::string controlName, pcmName;
	const MT32Emu::ROMImage *control =
		loadROM(romDir, MT32Emu::ROMInfo::Control, &controlFile, controlName);
	const MT32Emu::ROMImage *pcm =
		loadROM(romDir, MT32Emu::ROMInfo::PCM, &pcmFile, pcmName);
	if (!control || !pcm) { fprintf(stderr, "no usable ROM pair in %s\n", romDir.c_str()); return 1; }
	printf("control ROM: %s\n\n", controlName.c_str());
	printf("%-8s %-11s %s\n", "PROGRAM", "PARTIALS", "TIMBRE NAME");

	std::vector<int16_t> buf(kBlockFrames * 2);
	for (int program = 0; program < 128; ++program) {
		MT32Emu::Synth synth;
		if (!synth.open(*control, *pcm, kPartialPool, MT32Emu::AnalogOutputMode_DIGITAL_ONLY))
			return 1;
		// Part 1, middle C at a typical velocity. One note, nothing else.
		synth.playMsgNow(0xC0 | 1 | (program << 8));
		synth.playMsgNow(0x90 | 1 | (60 << 8) | (100 << 16));
		// The attack allocates within a few ms; sample the peak over 100 ms.
		uint32_t peak = 0;
		for (int block = 0; block < 50; ++block) {
			synth.render(buf.data(), kBlockFrames);
			uint32_t partials = 0, notes = 0;
			sample(synth, partials, notes);
			peak = std::max(peak, partials);
		}
		char name[11] = {0};
		bool named = synth.getSoundName(name, program < 64 ? 0 : 1, program & 63);
		printf("%-8d %-11u %s\n", program, peak, named ? name : "?");
		synth.close();
	}
	MT32Emu::ROMImage::freeROMImage(control);
	MT32Emu::ROMImage::freeROMImage(pcm);
	return 0;
}

int main(int argc, char **argv) {
	if (argc == 2 && std::string(argv[1]) == "--list-roms") return listROMs();
	if (argc == 3 && std::string(argv[1]) == "--probe") return probePrograms(argv[2]);
	if (argc < 3) {
		fprintf(stderr,
		        "usage: %s <roms-dir> <cues-dir> [max-cues]\n"
		        "       %s --check-cues <cues-dir>\n"
		        "       %s --list-roms\n\n"
		        "  roms-dir  holds an MT-32 control ROM and a PCM ROM (not shippable;\n"
		        "            supply your own dump)\n"
		        "  cues-dir  output of: foa-mt32-demand.py --export <dir>\n", argv[0]);
		return 2;
	}
	const std::string romDir = argv[1], cueDir = argv[2];
	const size_t maxCues = argc > 3 ? strtoul(argv[3], nullptr, 10) : 0;
	const bool checkOnly = romDir == "--check-cues";

	std::vector<std::string> cues;
	if (DIR *d = opendir(cueDir.c_str())) {
		struct dirent *ent;
		while ((ent = readdir(d)) != nullptr) {
			std::string n = ent->d_name;
			if (n.size() > 3 && n.compare(n.size() - 3, 3, ".ev") == 0)
				cues.push_back(cueDir + "/" + n);
		}
		closedir(d);
	}
	std::sort(cues.begin(), cues.end());
	if (cues.empty()) {
		fprintf(stderr, "no .ev cue files in %s\n", cueDir.c_str());
		return 1;
	}
	if (maxCues && cues.size() > maxCues) cues.resize(maxCues);

	if (checkOnly) {
		// Validates everything except the synthesis: that every cue parses,
		// that timestamps rise, and that the messages are ones Munt accepts.
		size_t events = 0, sysex = 0, voice = 0, bad = 0;
		double longest = 0.0;
		for (const std::string &path : cues) {
			std::vector<Event> ev;
			if (!readCue(path, ev) || ev.empty()) {
				fprintf(stderr, "unreadable or empty cue: %s\n", path.c_str());
				++bad;
				continue;
			}
			uint64_t prev = 0;
			for (const Event &e : ev) {
				++events;
				if (e.us < prev) { fprintf(stderr, "time goes backwards in %s\n", path.c_str()); ++bad; break; }
				prev = e.us;
				if (e.bytes[0] == 0xF0) {
					if (e.bytes.back() != 0xF7) { fprintf(stderr, "unterminated sysex in %s\n", path.c_str()); ++bad; }
					++sysex;
				} else if (e.bytes[0] >= 0x80 && e.bytes[0] < 0xF0) {
					++voice;
				} else {
					fprintf(stderr, "unexpected status %02x in %s\n", e.bytes[0], path.c_str());
					++bad;
				}
			}
			longest = std::max(longest, ev.back().us / 1e6);
		}
		printf("%zu cues, %zu events (%zu voice, %zu sysex), longest cue %.1f s\n",
		       cues.size(), events, voice, sysex, longest);
		printf(bad ? "FAILED: %zu problems\n" : "cue files OK\n", bad);
		return bad ? 1 : 0;
	}

	MT32Emu::FileStream controlFile, pcmFile;
	std::string controlName, pcmName;
	const MT32Emu::ROMImage *control =
		loadROM(romDir, MT32Emu::ROMInfo::Control, &controlFile, controlName);
	const MT32Emu::ROMImage *pcm =
		loadROM(romDir, MT32Emu::ROMInfo::PCM, &pcmFile, pcmName);
	if (!control || !pcm) {
		fprintf(stderr,
		        "No usable ROM pair in %s.\n"
		        "  control ROM: %s\n  PCM ROM:     %s\n"
		        "Supply an MT-32 (or CM-32L) control ROM and its PCM ROM as complete,\n"
		        "unpaired images. Munt identifies them by size and SHA-1, so half or\n"
		        "mux images are rejected here rather than silently mispaired.\n",
		        romDir.c_str(), control ? controlName.c_str() : "not found",
		        pcm ? pcmName.c_str() : "not found");
		return 1;
	}
	printf("control ROM: %s\nPCM ROM:     %s\n", controlName.c_str(), pcmName.c_str());

	printf("cues: %zu\n", cues.size());

	Stats total;
	std::vector<int16_t> buf(kBlockFrames * 2);

	for (size_t i = 0; i < cues.size(); ++i) {
		std::vector<Event> events;
		if (!readCue(cues[i], events) || events.empty()) continue;

		// A fresh Synth per cue: the game reinitialises the device between cues,
		// and a shared one would carry timbre and part state across them.
		MT32Emu::Synth synth;
		if (!synth.open(*control, *pcm, kPartialPool, MT32Emu::AnalogOutputMode_DIGITAL_ONLY)) {
			fprintf(stderr, "synth.open failed\n");
			return 1;
		}
		const double rate = synth.getStereoOutputSampleRate();
		const double blockSeconds = kBlockFrames / rate;

		double rendered = 0.0;           // seconds of audio produced
		size_t next = 0;
		const double lastEvent = events.back().us / 1e6;
		const double until = lastEvent + kTailSeconds;

		while (rendered < until) {
			// Deliver everything due before the end of the block we are about to render.
			const double blockEnd = rendered + blockSeconds;
			while (next < events.size() && events[next].us / 1e6 < blockEnd) {
				const std::vector<uint8_t> &b = events[next].bytes;
				if (b[0] == 0xF0)
					synth.playSysexNow(b.data(), (MT32Emu::Bit32u)b.size());
				else if (b.size() >= 1 && b[0] < 0xF0) {
					MT32Emu::Bit32u msg = b[0];
					if (b.size() > 1) msg |= (MT32Emu::Bit32u)b[1] << 8;
					if (b.size() > 2) msg |= (MT32Emu::Bit32u)b[2] << 16;
					synth.playMsgNow(msg);
				}
				++next;
			}
			synth.render(buf.data(), kBlockFrames);
			rendered = blockEnd;

			uint32_t partials = 0, notes = 0;
			sample(synth, partials, notes);
			accumulate(total, partials, notes, blockSeconds);

			// Past the last event, stop as soon as the tail has died away.
			if (next >= events.size() && partials == 0 && rendered > lastEvent) break;
		}
		synth.close();
		if ((i + 1) % 20 == 0 || i + 1 == cues.size())
			printf("  %zu/%zu cues, %.1f min rendered\n", i + 1, cues.size(),
			       total.totalSeconds / 60.0);
		fflush(stdout);
	}

	report(total);
	MT32Emu::ROMImage::freeROMImage(control);
	MT32Emu::ROMImage::freeROMImage(pcm);
	return 0;
}
