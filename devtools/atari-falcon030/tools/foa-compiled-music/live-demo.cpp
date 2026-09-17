// Portable host/68030 harness. Also supplies the source for transport checking.
#include "live-demo.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
	if (argc != 4) { std::fprintf(stderr, "usage: live-demo PACKAGE NEW_OUTPUT.raw FRAMES\n"); return 1; }
	char *end; unsigned long frames = std::strtoul(argv[3], &end, 10);
	if (*end || frames < 1 || frames > 6000000) return 1;
	FILE *f = std::fopen(argv[1], "rb"); if (!f) return 1;
	if (std::fseek(f, 0, SEEK_END)) return 1;
	long n = std::ftell(f); if (n < 12 || n > 16 * 1024 * 1024 || std::fseek(f, 0, SEEK_SET)) return 1;
	std::vector<uint8_t> package(static_cast<size_t>(n));
	bool ok = std::fread(package.data(), 1, package.size(), f) == package.size();
	if (std::fclose(f) || !ok) return 1;
	FCM::Bank bank; FCM::LiveDemo demo;
	if (!bank.open(package.data(), package.size())) return 1;
	std::vector<uint16_t> lookup(FCM::Bank::kLinearValues);
	bank.makeLinearTable(lookup.data());
	if (!demo.open(bank)) return 1;
	f = std::fopen(argv[2], "wbx"); if (!f) return 1;
	int16_t buffer[514];
	for (unsigned long i = 0; i < frames;) {
		unsigned count = frames - i < 257 ? unsigned(frames - i) : 257;
		demo.read(buffer, count);
		for (unsigned j = 0; j < 2 * count; ++j) {
			std::fputc(uint8_t(uint16_t(buffer[j]) >> 8), f); std::fputc(uint8_t(buffer[j]), f);
		}
		i += count;
	}
	ok = !std::ferror(f); if (std::fclose(f) || !ok) return 1;
	std::printf("{\"output_frames\":%lu,\"native_frames\":%u,\"native_checksum\":%u}\n", frames,demo.frames(),demo.checksum());
	return 0;
}
