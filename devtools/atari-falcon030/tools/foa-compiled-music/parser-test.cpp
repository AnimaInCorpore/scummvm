// Host differential gate using ScummVM's real MidiParser base and SMF parser.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "audio/midiparser_smf.h"
#include "audio/mididrv.h"
#include "scumm/imuse/imuse_fcm.h"
#include "common/memstream.h"
#include "test/system/null_osystem.h"

typedef std::vector<byte> Bytes;
static unsigned soundID, trackID, stepID;
static void check(bool ok, const char *what) {
	if (!ok) { std::fprintf(stderr, "cue %u track %u step %u: %s\n", soundID, trackID, stepID, what); std::exit(1); }
}
static Bytes readFile(const char *path) {
	FILE *f = std::fopen(path, "rb"); check(f != nullptr, "input open");
	check(!std::fseek(f, 0, SEEK_END), "input seek");
	long n = std::ftell(f); check(n >= 0 && n < 16 * 1024 * 1024, "input size");
	std::rewind(f); Bytes data(n);
	check(std::fread(data.data(), 1, n, f) == size_t(n), "input read"); std::fclose(f);
	return data;
}

class Trace : public MidiDriver_BASE {
public:
	Bytes bytes;
	void put(uint32 value) { for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(byte(value >> shift)); }
	void send(uint32 message) override { bytes.push_back(0); put(message); }
	void sysEx(const byte *data, uint16 n) override { bytes.push_back(1); put(n); bytes.insert(bytes.end(), data, data + n); }
	void metaEvent(byte type, const byte *data, uint16 n) override {
		bytes.push_back(2); bytes.push_back(type); put(n);
		if (n) bytes.insert(bytes.end(), data, data + n);
	}
};

static uint64 comparisons = 0, deliveredBytes = 0;
static void compare(MidiParser &original, MidiParser &compiled, Trace &a, Trace &b) {
	check(a.bytes == b.bytes, "driver events differ");
	check(original.getTick() == compiled.getTick(), "tick differs");
	check(original.getActiveTrack() == compiled.getActiveTrack(), "active track differs");
	check(original.isPlaying() == compiled.isPlaying(), "playing state differs");
	deliveredBytes += a.bytes.size(); ++comparisons;
	a.bytes.clear(); b.bytes.clear();
}

int main(int argc, char **argv) {
	check(argc == 3, "usage: parser-test FIXTURE PACKAGE");
	Common::install_null_g_system();
	{
		Bytes fixture = readFile(argv[1]), package = readFile(argv[2]);
		check(fixture.size() >= 8 && !memcmp(fixture.data(), "FCT1", 4), "fixture header");
		Scumm::FCMScore score;
		Common::MemoryReadStream stream(package.data(), package.size());
		check(score.load(stream), "package loader");
		check(score.open(Common::Path(argv[2])), "explicit filesystem path loader");
		uint32 missingSize = 99;
		check(!score.cue(65535, missingSize) && missingSize == 0, "missing ID");
		unsigned cues = READ_BE_UINT32(fixture.data() + 4), tracksChecked = 0, maxStorage = 0;
		size_t pos = 8;
		for (unsigned c = 0; c < cues; ++c) {
			check(pos + 6 <= fixture.size(), "fixture directory");
			soundID = READ_BE_UINT16(fixture.data() + pos);
			uint32 sourceSize = READ_BE_UINT32(fixture.data() + pos + 2); pos += 6;
			check(sourceSize >= 30 && sourceSize <= fixture.size() - pos, "fixture source bounds");
			const byte *source = fixture.data() + pos; pos += sourceSize;
			uint32 cueSize = 0; const byte *cue = score.cue(soundID, cueSize);
			check(cue && cueSize >= 24, "cue lookup");
			uint16 tracks = READ_BE_UINT16(cue + 4);
			for (trackID = 0; trackID < tracks; ++trackID) {
				for (unsigned mode = 0; mode < 2; ++mode) {
					Trace a, b;
					MidiParser_SMF original;
					Scumm::MidiParser_FCM compiled;
					original.setMidiDriver(&a); compiled.setMidiDriver(&b);
					original.property(MidiParser::mpSmartJump, 1); compiled.property(MidiParser::mpSmartJump, 1);
					check(original.loadMusic(source + 16, sourceSize - 16), "original load");
					check(compiled.loadMusic(cue, cueSize), "compiled load");
					if (compiled.storageBytes() > maxStorage) maxStorage = compiled.storageBytes();
					check(original.setTrack(trackID) == compiled.setTrack(trackID), "setTrack");
					original.setTimerRate(10000); compiled.setTimerRate(10000);
					stepID = 0; compare(original, compiled, a, b);
					for (; stepID < 1000000 && original.isPlaying(); ++stepID) {
						original.onTimer(); compiled.onTimer(); compare(original, compiled, a, b);
						if (mode == 1) {
							if (stepID == 20 || stepID == 40 || stepID == 60) {
								uint32 tick = stepID == 20 ? 240 : stepID == 40 ? 0x7fffffffu : 0;
								check(original.jumpToTick(tick, true, true, true) == compiled.jumpToTick(tick, true, true, true), "jump result");
							}
							if (stepID == 80) { original.pausePlaying(); compiled.pausePlaying(); }
							if (stepID == 82) { original.resumePlaying(); compiled.resumePlaying(); }
							if (stepID == 100) check(original.setTrack((trackID + 1) % tracks) == compiled.setTrack((trackID + 1) % tracks), "switch track");
							compare(original, compiled, a, b);
						}
					}
					check(stepID < 1000000, "timer limit");
					original.unloadMusic(); compiled.unloadMusic(); compare(original, compiled, a, b);
					original.setMidiDriver(nullptr); compiled.setMidiDriver(nullptr);
				}
				++tracksChecked;
			}
			// Invalid event-table offset, unsupported type and truncated payload.
			Scumm::MidiParser_FCM invalid;
			Bytes bad(cue, cue + cueSize);
			WRITE_BE_UINT32(bad.data() + 28, 0xffffffffu);
			check(!invalid.loadMusic(bad.data(), bad.size()) && !invalid.isPlaying(), "bad event offset accepted");
			bad.assign(cue, cue + cueSize); WRITE_BE_UINT16(bad.data(), 1);
			check(!invalid.loadMusic(bad.data(), bad.size()), "unsupported format accepted");
			check(!invalid.loadMusic(cue, 23), "truncated cue accepted");
			bad.assign(cue, cue + cueSize);
			uint32 first = READ_BE_UINT32(cue + 28);
			WRITE_BE_UINT16(bad.data() + first + 4, 0x7d00);
			WRITE_BE_UINT16(bad.data() + first + 6, 0);
			WRITE_BE_UINT32(bad.data() + first + 8, cueSize);
			check(!invalid.loadMusic(bad.data(), bad.size()), "short iMUSE part setup accepted");
		}
		check(pos == fixture.size(), "trailing fixture");
		// The SCOR checksum is covered independently of cue validation.
		package[24] ^= 1;
		Common::MemoryReadStream corrupt(package.data(), package.size());
		check(!score.load(corrupt) && !score.cue(21, missingSize), "corrupt score CRC accepted/stale cache retained");
		std::printf("{\"cues\":%u,\"tracks\":%u,\"comparisons\":%llu,\"delivered_bytes\":%llu,\"max_parser_storage_bytes\":%u,\"passed\":true}\n",
		            cues, tracksChecked, (unsigned long long)comparisons, (unsigned long long)deliveredBytes, maxStorage);
	}
	Common::uninstall_null_g_system();
	return 0;
}
