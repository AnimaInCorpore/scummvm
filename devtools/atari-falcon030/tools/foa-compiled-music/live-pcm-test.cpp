// Host-only independent Munt comparison for the FCM live partial.
#include "live-demo.h"
#include "mt32emu.h"
#include "Tables.h"
#include "LA32WaveGenerator.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

typedef std::vector<uint8_t> Bytes;
static void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
static Bytes read(const char *path) {
	FILE *f = std::fopen(path, "rb"); check(f, "open input");
	check(!std::fseek(f, 0, SEEK_END), "seek input");
	long n = std::ftell(f); check(n >= 0 && n <= 16 * 1024 * 1024, "input length");
	check(!std::fseek(f, 0, SEEK_SET), "rewind input");
	Bytes b(static_cast<size_t>(n)); bool ok = std::fread(b.data(), 1, b.size(), f) == b.size();
	check(!std::fclose(f) && ok, "read input"); return b;
}
static void le32(FILE *f, uint32_t x) { for (unsigned i = 0; i < 4; ++i) std::fputc(uint8_t(x >> (i * 8)), f); }
static void wav(const char *path, const std::vector<int16_t> &samples) {
	FILE *f = std::fopen(path, "wbx"); check(f, "output must be a new file");
	std::fwrite("RIFF", 1, 4, f); le32(f, 36 + samples.size() * 2);
	std::fwrite("WAVEfmt ", 1, 8, f); le32(f, 16); le32(f, 0x00010001);
	le32(f, 32000); le32(f, 64000); le32(f, 0x00100002);
	std::fwrite("data", 1, 4, f); le32(f, samples.size() * 2);
	for (int16_t s : samples) { std::fputc(uint8_t(s), f); std::fputc(uint8_t(uint16_t(s) >> 8), f); }
	bool ok = !std::ferror(f); check(!std::fclose(f) && ok, "write WAV");
}

static void resamplerTest() {
	// Direct absolute-time arithmetic is independent of the coefficient table
	// and catches drift, table wrap, consume-before-output and signed overflow.
	const unsigned frames = 25175 * 8 + 13;
	std::vector<int16_t> source(2 + uint64_t(frames) * 16384 / 25175);
	for (unsigned i = 0; i < source.size(); ++i)
		source[i] = i % 4 == 0 ? -32768 : i % 4 == 1 ? 32767 : int16_t(i * 7919u);
	std::vector<int16_t> expected(frames * 2), actual(frames * 2);
	for (unsigned i = 0; i < frames; ++i) {
		uint64_t position = uint64_t(i) * 32000 * 512 * 65536 / 25175000;
		unsigned index = position >> 16, fraction = (position & 65535) >> 1;
		int32_t a = source[index], b = source[index + 1];
		int32_t sample = a + (((b - a) * int32_t(fraction)) >> 15);
		expected[i * 2] = int16_t(sample); expected[i * 2 + 1] = int16_t(sample >> 1);
	}
	for (unsigned chunk : {1u,2u,255u,256u,997u,25175u,frames}) {
		FCM::FalconResampler converter; converter.open(); converter.prime(source[0], source[1]);
		const int16_t *cursor = source.data() + 2;
		unsigned done = 0;
		while (done < frames) {
			check(converter.render(nullptr, 0, cursor, source.data() + source.size()) == 0, "zero output advanced resampler");
			check(converter.render(nullptr, 1, cursor, cursor) == 0, "empty input advanced resampler");
			unsigned span = std::min(chunk, frames - done);
			// Vary native refills separately from output callback boundaries.
			const int16_t *end = cursor + std::min<size_t>(1 + (done * 17) % 256, source.data() + source.size() - cursor);
			unsigned count = converter.render(actual.data() + done * 2, span, cursor, end);
			check(count > 0 && count <= span && cursor <= end, "resampler made no progress or exceeded input");
			done += count;
			check(cursor - source.data() == 2 + uint64_t(done) * 16384 / 25175, "native sample consumption drifted");
		}
		check(actual == expected, "resampler differs from absolute-time reference");
	}
	std::printf("{\"resampler_frames\":%u,\"callback_sizes\":7,\"passed\":true}\n", frames * 7);
}

static void kernelTest() {
	resamplerTest();
	const auto &tables = MT32Emu::Tables::getInstance();
	MT32Emu::LA32IntPartialPair::initTables(tables);
	Bytes exp, words;
	for (auto x : tables.exp9) { exp.push_back(x >> 8); exp.push_back(x); }
	std::vector<int16_t> native;
	// Generated test words exercise signs, silence, endpoints and interpolation.
	for (unsigned i = 0; i < 8192; ++i) {
		uint16_t x = uint16_t(i * 4051 + (i >> 2));
		words.push_back(x >> 8); words.push_back(x); native.push_back(int16_t(x));
	}
	uint32_t count = 0;
	for (bool loop : {false, true}) for (bool dynamic : {false, true})
	for (unsigned pitch : {0u,8192u,32768u,40001u,50000u,59392u}) {
		FCM::PCMWave wave;
		wave.exp = exp.data(); wave.words = words.data(); wave.length = native.size(); wave.loop = loop; wave.active = true;
		MT32Emu::LA32IntPartialPair ref;
		ref.init(false, false); ref.deactivate(MT32Emu::LA32PartialPair::SLAVE);
		ref.initPCM(MT32Emu::LA32PartialPair::MASTER, native.data(), native.size(), loop);
		for (unsigned i = 0; i < 48000; ++i) {
			uint32_t amp = (i * 7919u) % 67117057u;
			uint16_t livePitch = dynamic ? (pitch + (i / 997) * 137) % 59393 : pitch;
			ref.generateNextSample(MT32Emu::LA32PartialPair::MASTER, amp, livePitch, 0);
			check(wave.next(amp, livePitch) == ref.nextOutSample(), "PCM kernel sample differs");
			check(wave.active == ref.isActive(MT32Emu::LA32PartialPair::MASTER), "PCM end state differs");
			++count;
		}
	}
	std::printf("{\"kernel_samples\":%u,\"passed\":true}\n", count);
}

static void sysex(MT32Emu::Synth &synth, uint8_t a, uint8_t b, uint8_t c, const Bytes &data) {
	Bytes msg = {0xf0,0x41,0x10,0x16,0x12,a,b,c};
	unsigned sum = a + b + c;
	for (uint8_t x : data) { msg.push_back(x); sum += x; }
	msg.push_back(uint8_t(-sum) & 127); msg.push_back(0xf7);
	synth.playSysexNow(msg.data(), msg.size());
}

static void referenceTest(char **argv) {
	Bytes bytes = read(argv[4]); FCM::Bank bank;
	check(bank.open(bytes.data(), bytes.size()), "bank validation failed");
	std::vector<uint16_t> lookup(FCM::Bank::kLinearValues);
	bank.makeLinearTable(lookup.data());
	FCM::PCMVoice voice;
	check(!voice.noteOn(bank, 0, 60, 100), "unsupported multi-partial program accepted");
	check(!voice.noteOn(bank, 103, 60, 128), "invalid velocity accepted");
	check(!voice.noteOn(bank, 103, 128, 100), "invalid key accepted");
	check(!voice.noteOn(bank, 103, 60, 100, 101), "invalid volume accepted");
	Bytes damaged = bytes; damaged.back() ^= 1;
	FCM::Bank bad; check(!bad.open(damaged.data(), damaged.size()), "corrupt CRC accepted");
	check(!bad.open(bytes.data(), bytes.size() - 1), "truncated bank accepted");
	// Independent callback sizes must not move events or accumulate clock error.
	FCM::LiveDemo whole, split;
	check(whole.open(bank) && split.open(bank), "demo profile rejected");
	std::vector<int16_t> a(2 * 700000), b(a.size());
	whole.read(a.data(), a.size() / 2);
	unsigned pos = 0;
	while (pos < b.size() / 2) {
		unsigned span = std::min<unsigned>(b.size() / 2 - pos, 1 + (pos * 43) % 997);
		split.read(b.data() + 2 * pos, span); pos += span;
		split.read(nullptr, 0);
	}
	check(a == b && whole.checksum() == split.checksum() && whole.frames() == split.frames(), "callback sizes changed audio");
	check(whole.frames() == ((2 + uint64_t(700000) * 32000 * 512 / 25175000 + 255) / 256) * 256, "32 kHz/codec clock drift");
	MT32Emu::FileStream cf, pf;
	check(cf.open(argv[2]) && pf.open(argv[3]), "open reference ROMs");
	const auto *ci = MT32Emu::ROMInfo::getROMInfo(&cf), *pi = MT32Emu::ROMInfo::getROMInfo(&pf);
	check(ci && pi && !std::strcmp(ci->shortName, "ctrl_mt32_1_07") && !std::strcmp(pi->shortName, "pcm_mt32"), "requires complete control 1.07 and MT-32 PCM ROMs");
	const auto *control = MT32Emu::ROMImage::makeROMImage(&cf), *pcm = MT32Emu::ROMImage::makeROMImage(&pf);
	std::vector<int16_t> actual, expected;
	uint32_t samples = 0, cases = 0, mismatches = 0; int peakError = 0, peak = 0;
	for (unsigned key : {36u,48u,60u,72u,96u}) for (unsigned velocity : {1u,32u,64u,100u,127u})
	for (unsigned off : {1u,160u,8000u,24000u}) for (unsigned bend : {0u,8192u,16383u}) {
		MT32Emu::Synth synth;
		synth.selectRendererType(MT32Emu::RendererType_BIT16S);
		synth.setDACInputMode(MT32Emu::DACInputMode_PURE);
		synth.setNiceAmpRampEnabled(false); synth.setNicePanningEnabled(false); synth.setNicePartialMixingEnabled(false);
		check(synth.open(*control, *pcm, 32, MT32Emu::AnalogOutputMode_DIGITAL_ONLY), "open synth");
		synth.setReverbEnabled(false);
		synth.playMsgNow(0xc1 | (103 << 8));
		// Full left isolates the unpanned partial; no reverb/DAC/analog filter.
		sysex(synth, 3, 0, 8, Bytes{100,14});
		sysex(synth, 3, 0, 6, Bytes{0});
		synth.playMsgNow(0xb1 | (11 << 8) | (127 << 16));
		check(voice.noteOn(bank, 103, key, velocity), "Xylophone profile rejected");
		check(voice.bend(bend), "bend rejected");
		synth.playMsgNow(0xe1 | ((bend & 127) << 8) | ((bend >> 7) << 16));
		synth.playMsgNow(0x91 | (key << 8) | (velocity << 16));
		bool preview = key == 60 && velocity >= 32 && off == 8000 && bend == 8192;
		std::vector<int16_t> blocked(48000);
		FCM::PCMVoice batched = voice;
		unsigned blockPos = 0;
		while (blockPos < blocked.size()) {
			if (blockPos == off) batched.noteOff(key);
			unsigned span = std::min<unsigned>(blocked.size() - blockPos, 1 + (blockPos * 17) % 257);
			if (blockPos < off) span = std::min(span, off - blockPos);
			batched.render(blocked.data() + blockPos, span); blockPos += span;
		}
		for (unsigned frame = 0; frame < 48000; ++frame) {
			if (frame == off) { voice.noteOff(key); synth.playMsgNow(0x81 | (key << 8)); }
			int16_t l = 0, r = 0, dl = 0, dr = 0, wl = 0, wr = 0;
			synth.renderStreams(&l, &r, &dl, &dr, &wl, &wr, 1);
			int16_t value = voice.next();
			check(value == blocked[frame], "block renderer changed envelope or oscillator timing");
			int error = std::abs(int(value) - int(l));
			if (error && mismatches < 8) std::fprintf(stderr, "key %u velocity %u off %u frame %u: FCM %d Munt %d dry %d\n",key,velocity,off,frame,value,l,dl);
			mismatches += error != 0; peakError = std::max(peakError, error); peak = std::max(peak, std::abs(int(value)));
			check(!r && !dl && !dr && !wl && !wr, "reference routing is not isolated dry left");
			if (preview) { actual.push_back(value); expected.push_back(l); }
			++samples;
		}
		++cases;
	}
	wav(argv[5], actual); wav(argv[6], expected);
	MT32Emu::ROMImage::freeROMImage(control); MT32Emu::ROMImage::freeROMImage(pcm);
	std::printf("{\"note_cases\":%u,\"samples\":%u,\"mismatched_samples\":%u,\"peak_error\":%d,\"peak\":%d,\"passed\":%s}\n", cases,samples,mismatches,peakError,peak,mismatches ? "false" : "true");
	check(!mismatches, "live note/reference mismatch");
}

int main(int argc, char **argv) {
	try {
		if (argc == 2 && !std::strcmp(argv[1], "kernel")) kernelTest();
		else if (argc == 7 && !std::strcmp(argv[1], "reference")) referenceTest(argv);
		else throw std::runtime_error("usage: live-pcm-test kernel | reference CONTROL PCM PACKAGE NEW_FCM.wav NEW_MUNT.wav");
	} catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
	return 0;
}
