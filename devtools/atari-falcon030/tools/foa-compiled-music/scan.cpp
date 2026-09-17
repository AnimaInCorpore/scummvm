// Portable consumer gate for FCM1 score data; does not implement iMUSE policy.
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "runtime.h"

static void check(bool ok) { if (!ok) throw std::runtime_error("invalid FCM package"); }
static uint32_t hashByte(uint32_t hash, uint8_t value) { return (hash ^ value) * 16777619u; }
static void hashBytes(uint32_t &hash, const uint8_t *data, uint32_t count) {
	for (uint32_t i = 0; i < count; ++i) hash = hashByte(hash, data[i]);
}
static uint32_t crc32(const uint8_t *data, uint32_t size) {
	uint32_t crc = 0xffffffffu;
	for (uint32_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

int main(int argc, char **argv) {
	try {
		check(argc == 2);
		FILE *f = std::fopen(argv[1], "rb"); check(f != nullptr);
		check(std::fseek(f, 0, SEEK_END) == 0);
		long length = std::ftell(f); check(length >= 12 && uint64_t(length) < 0x80000000u);
		check(std::fseek(f, 0, SEEK_SET) == 0);
		std::vector<uint8_t> bytes(static_cast<size_t>(length));
		bool read = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
		int closed = std::fclose(f); check(read && closed == 0);
		const uint8_t *data = bytes.data();
		check(!std::memcmp(data, "FCM1", 4) && FCM::be16(data + 4) == 1 && FCM::be32(data + 8) == bytes.size());
		uint32_t sections = FCM::be16(data + 6), end = 12 + 16 * sections;
		check(end <= bytes.size());
		const uint8_t *score = nullptr;
		uint32_t scoreBytes = 0;
		for (uint32_t i = 0; i < sections; ++i) {
			const uint8_t *entry = data + 12 + i * 16;
			uint32_t offset = FCM::be32(entry + 4), size = FCM::be32(entry + 8);
			check(offset == ((end + 3) & ~3u) && offset <= bytes.size() && size <= bytes.size() - offset);
			for (uint32_t p = end; p < offset; ++p) check(data[p] == 0);
			for (uint32_t j = 0; j < i; ++j) check(std::memcmp(entry, data + 12 + j * 16, 4) != 0);
			check(crc32(data + offset, size) == FCM::be32(entry + 12));
			if (!std::memcmp(entry, "SCOR", 4)) { score = data + offset; scoreBytes = size; }
			end = offset + size;
		}
		check(end == bytes.size() && score && scoreBytes >= 4);
		uint32_t cueCount = FCM::be32(score), hash = 2166136261u;
		check(cueCount > 0 && cueCount <= (scoreBytes - 4) / 12);
		uint32_t previousEnd = 4 + 12 * cueCount, trackCount = 0, eventCount = 0, previousID = 0;
		for (uint32_t i = 0; i < cueCount; ++i) {
			const uint8_t *entry = score + 4 + i * 12;
			uint32_t id = FCM::be16(entry), offset = FCM::be32(entry + 4), size = FCM::be32(entry + 8);
			check((i == 0 || id > previousID) && offset == ((previousEnd + 3) & ~3u) && offset <= scoreBytes && size <= scoreBytes - offset && size >= 8);
			for (uint32_t p = previousEnd; p < offset; ++p) check(score[p] == 0);
			previousEnd = offset + size; previousID = id;
			const uint8_t *cue = score + offset;
			uint32_t format = FCM::be16(cue), division = FCM::be16(cue + 2), tracks = FCM::be16(cue + 4), mdlen = FCM::be16(cue + 6);
			check(format <= 2 && division > 0 && division < 0x8000 && tracks > 0 && (format != 0 || tracks == 1));
			uint32_t table = 8 + mdlen, eventEnd = table + tracks * 8;
			check(eventEnd <= size);
			check(mdlen == 16 && !std::memcmp(cue + 8, "MDhd\0\0\0\10", 8));
			for (uint32_t t = 0; t < tracks; ++t) {
				uint32_t count = FCM::be32(cue + table + t * 8), pos = FCM::be32(cue + table + t * 8 + 4);
				check(count > 0 && pos == eventEnd && count <= (size - eventEnd) / 12);
				eventEnd += count * 12;
			}
			hashBytes(hash, entry, 4); hashBytes(hash, cue, table);
			for (uint32_t t = 0; t < tracks; ++t) {
				FCM::Track track; check(track.open(cue, size, uint16_t(t)));
				hashBytes(hash, cue + table + t * 8, 4);
				uint32_t previousTick = 0;
				for (uint32_t e = 0; e < track.count; ++e) {
					const uint8_t *event = track.peek(); check(event != nullptr);
					uint32_t tick = FCM::be32(event), opcode = FCM::be16(event + 4), n = FCM::be16(event + 6), arg = FCM::be32(event + 8);
					check(tick >= previousTick); previousTick = tick;
					hashBytes(hash, event, 8);
					if (opcode >= 0x80 && opcode < 0xf0) {
						check(n == ((opcode >> 4 == 12 || opcode >> 4 == 13) ? 1u : 2u));
						check(arg < (1u << (8 * n)));
						for (uint32_t k = 4 - n; k < 4; ++k) check(event[8 + k] < 128);
						hashBytes(hash, event + 12 - n, n);
					} else {
						bool known = opcode >> 8 == 0xff || opcode == 0xf0f0 || opcode == 0xf0f7;
						if (opcode == 0x4100) { known = true; check(n == 247); }
						if (opcode >> 8 == 0x7d) {
							unsigned cmd = opcode & 255;
							known = cmd <= 2 || (cmd >= 48 && cmd <= 53) || cmd == 64 || cmd == 80 || cmd == 81 || cmd == 96;
							if (cmd == 0 || (cmd >= 48 && cmd <= 53) || cmd == 80) check(n >= 1);
						}
						check(known && arg >= eventEnd && arg <= size && n <= size - arg);
						if (opcode == 0xff51) check(n == 3 && (cue[arg] || cue[arg+1] || cue[arg+2]));
						if (opcode == 0x4100) {
							check(cue[arg] < 16);
							for (uint32_t k = 0; k < n; ++k) check(cue[arg + k] < 128);
						}
						hashBytes(hash, cue + arg, n);
					}
					check((opcode == 0xff2f) == (e + 1 == track.count));
					if (opcode == 0xff2f) check(n == 0);
					++track.cursor; ++eventCount;
				}
				check(!track.peek());
				// Jump/loop cursor checks against a linear scan, including ticks
				// shared by multiple events. A second player has independent state.
				for (uint32_t test = 0; test < 7; ++test) {
					uint32_t tick = test == 6 ? 0xffffffffu : previousTick / 5 * test;
					FCM::Track other; check(other.open(cue, size, uint16_t(t)));
					uint32_t expected = 0;
					while (expected < track.count && FCM::be32(cue + track.records + expected * 12) < tick) ++expected;
					track.seek(tick); check(track.cursor == expected && other.cursor == 0);
				}
				++trackCount;
			}
		}
		check(previousEnd == scoreBytes);
		std::printf("{\"cues\":%u,\"tracks\":%u,\"events\":%u,\"instruction_fnv1a\":\"%08x\",\"passed\":true}\n", cueCount, trackCount, eventCount, hash);
	} catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
	return 0;
}
