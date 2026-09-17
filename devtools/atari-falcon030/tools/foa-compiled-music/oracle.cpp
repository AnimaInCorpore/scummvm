// Host-only Munt data exporter and differential tests. Never renders audio.
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "mt32emu.h"
#include "Structures.h"
#include "Tables.h"
#include "LA32Ramp.h"
#include "runtime.h"

typedef std::vector<uint8_t> Bytes;

static void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

static void put32(Bytes &data, uint32_t value) {
	for (int i = 24; i >= 0; i -= 8) data.push_back(uint8_t(value >> i));
}

static void put16(Bytes &data, uint16_t value) {
	data.push_back(uint8_t(value >> 8)); data.push_back(uint8_t(value));
}

static Bytes rampTable() {
	const auto &tables = MT32Emu::Tables::getInstance();
	Bytes result;
	for (unsigned i = 0; i < 256; ++i) {
		uint32_t step = 0;
		if (i) {
			uint32_t arg = i & 127;
			step = 8191 - tables.exp9[~(arg << 6) & 511];
			step = ((step << (arg >> 3)) + 64) >> 9;
		}
		if (i & 128) ++step;
		put32(result, step);
	}
	return result;
}

static void rampTest() {
	MT32Emu::LA32Ramp::initTables(MT32Emu::Tables::getInstance());
	Bytes lut = rampTable();
	uint64_t checked = 0;
	MT32Emu::LA32Ramp reference;
	FCM::Ramp compiled;
	// Every target/increment combination, interrupted from varying current
	// states, both per-sample execution and spans that cross the completion IRQ.
	for (unsigned target = 0; target < 256; ++target) {
		for (unsigned increment = 0; increment < 256; ++increment) {
			reference.startRamp(uint8_t(target), uint8_t(increment));
			compiled.start(uint8_t(target), uint8_t(increment), lut.data());
			for (unsigned j = 0; j < 19; ++j) {
				unsigned span = j < 9 ? 1 : 1 + ((target * 31 + increment * 17 + j * 43) % 257);
				uint32_t value = 0;
				for (unsigned k = 0; k < span; ++k) { value = reference.nextValue(); ++checked; }
				require(compiled.advance(span) == value, "compiled ramp value differs");
				require(compiled.checkInterrupt() == reference.checkInterrupt(), "compiled ramp IRQ differs");
				FCM::Ramp saved = compiled;
				require(compiled.advance(0) == value, "zero span changed ramp");
				compiled = saved;
			}
		}
	}
	// Long sustain and early note-off-style retarget, including cancellation of
	// a pending interrupt. Read the interrupt once per sample around boundaries.
	reference.reset(); compiled = FCM::Ramp();
	for (unsigned phase = 0; phase < 4; ++phase) {
		uint8_t target = phase == 0 ? 240 : phase == 1 ? 127 : 0;
		uint8_t inc = phase == 0 ? 127 : phase == 1 ? 0 : 255;
		reference.startRamp(target, inc); compiled.start(target, inc, lut.data());
		for (unsigned j = 0; j < 64000; ++j) {
			require(compiled.advance(1) == reference.nextValue(), "retarget differs");
			require(compiled.checkInterrupt() == reference.checkInterrupt(), "retarget IRQ differs");
			++checked;
		}
	}
	std::printf("{\"ramp_reference_samples\":%llu,\"target_increment_pairs\":65536,\"passed\":true}\n",
	            static_cast<unsigned long long>(checked));
}

static Bytes memory(MT32Emu::Synth &synth, uint32_t address, uint32_t length) {
	Bytes result(length, 0xcc);
	synth.readMemory(address, length, result.data());
	return result;
}

static void section(Bytes &out, const char *tag, const Bytes &data) {
	out.insert(out.end(), tag, tag + 4); put32(out, uint32_t(data.size()));
	out.insert(out.end(), data.begin(), data.end());
}

static void exportBank(const char *ctrlPath, const char *pcmPath, const char *output, const char *customPath) {
	MT32Emu::FileStream ctrlFile, pcmFile;
	require(ctrlFile.open(ctrlPath) && pcmFile.open(pcmPath), "cannot open ROM pair");
	const auto *ci = MT32Emu::ROMInfo::getROMInfo(&ctrlFile);
	const auto *pi = MT32Emu::ROMInfo::getROMInfo(&pcmFile);
	require(ci && pi && !std::strcmp(ci->shortName, "ctrl_mt32_1_07") &&
	        !std::strcmp(pi->shortName, "pcm_mt32"), "requires complete MT-32 control 1.07 and PCM ROMs");
	const auto *control = MT32Emu::ROMImage::makeROMImage(&ctrlFile);
	const auto *pcm = MT32Emu::ROMImage::makeROMImage(&pcmFile);
	MT32Emu::Synth synth;
	require(synth.open(*control, *pcm, 32, MT32Emu::AnalogOutputMode_DIGITAL_ONLY), "Munt open failed");
	static_assert(sizeof(MT32Emu::TimbreParam) == 246, "timbre layout changed");
	Bytes out = {'F','M','B','1'};
	section(out, "PTCH", memory(synth, MT32EMU_MEMADDR(0x050000), 128 * 8));
	section(out, "RHYT", memory(synth, MT32EMU_MEMADDR(0x030110), 85 * 4));
	section(out, "SYST", memory(synth, MT32EMU_MEMADDR(0x100000), 23));
	Bytes timbres;
	for (unsigned i = 0; i < 158; ++i) {
		if (i < 128) {
			synth.playMsgNow(0xc1 | (i << 8));
		} else {
			// Select rhythm-bank timbre into melodic part 1's temporary area.
			uint8_t msg[] = {0xf0,0x41,0x10,0x16,0x12,0x03,0,0,3,uint8_t(i-128),0,0xf7};
			msg[10] = uint8_t(-(3 + 3 + (i-128))) & 127;
			synth.playSysexNow(msg, sizeof(msg));
		}
		Bytes timbre = memory(synth, MT32EMU_MEMADDR(0x040000), 246);
		char name[11] = {};
		synth.getSoundName(name, uint8_t(i < 128 ? i / 64 : 3), uint8_t(i < 128 ? i % 64 : i - 128));
		require(!std::memcmp(name, timbre.data(), 10), "selected timbre/name mismatch");
		timbres.insert(timbres.end(), timbre.begin(), timbre.end());
	}
	Bytes custom;
	if (customPath) {
		MT32Emu::FileStream customFile;
		require(customFile.open(customPath) && customFile.getSize() % 246 == 0 && customFile.getSize() / 246 < 4096, "invalid custom definitions");
		custom.assign(customFile.getData(), customFile.getData() + customFile.getSize());
		for (size_t i = 0; i < custom.size(); i += 246) {
			Bytes msg = {0xf0,0x41,0x10,0x16,0x12,0x04,0,0};
			msg.insert(msg.end(), custom.begin() + i, custom.begin() + i + 246);
			unsigned sum = 0;
			for (size_t j = 5; j < msg.size(); ++j) { require(msg[j] < 128, "non-SysEx custom byte"); sum += msg[j]; }
			msg.push_back(uint8_t(-sum) & 127); msg.push_back(0xf7);
			synth.playSysexNow(msg.data(), uint32_t(msg.size()));
			Bytes normalized = memory(synth, MT32EMU_MEMADDR(0x040000), 246);
			require(!std::memcmp(normalized.data(), custom.data() + i, 10), "custom timbre load failed");
			timbres.insert(timbres.end(), normalized.begin(), normalized.end());
		}
	}
	section(out, "TIMB", timbres);
	section(out, "CDEF", custom);
	// This explicit ROM profile's 128 wave descriptors retain pitch and tuning
	// flags as well as addresses/loops. No ROM waves are resampled or recorded.
	const auto *ctrl = ctrlFile.getData();
	section(out, "WAVE", Bytes(ctrl + 0x3000, ctrl + 0x3200));
	Bytes logPCM;
	const uint8_t *raw = pcmFile.getData();
	const unsigned order[16] = {0,9,1,2,3,4,5,6,7,10,11,12,13,14,15,8};
	for (unsigned i = 0; i < 262144; ++i) {
		uint16_t value = 0;
		for (unsigned bit = 0; bit < 16; ++bit) {
			unsigned source = order[bit];
			value |= ((raw[2*i + source/8] >> (7-source%8)) & 1) << (15-bit);
		}
		put16(logPCM, value);
	}
	section(out, "PCML", logPCM);
	section(out, "RLUT", rampTable());
	const auto &tables = MT32Emu::Tables::getInstance();
	Bytes tableBytes;
	for (auto x : tables.exp9) put16(tableBytes, x);
	for (auto x : tables.logsin9) put16(tableBytes, x);
	for (auto x : tables.levelToAmpSubtraction) tableBytes.push_back(x);
	for (auto x : tables.envLogarithmicTime) tableBytes.push_back(x);
	for (auto x : tables.masterVolToAmpSubtraction) tableBytes.push_back(x);
	for (auto x : tables.pulseWidth100To255) tableBytes.push_back(x);
	for (unsigned i = 0; i < 8; ++i) tableBytes.push_back(tables.resAmpDecayFactors[i]);
	section(out, "TABL", tableBytes);
	FILE *file = std::fopen(output, "wbx");
	require(file != nullptr, "cannot create output (must not exist)");
	bool written = std::fwrite(out.data(), 1, out.size(), file) == out.size();
	int closed = std::fclose(file);
	require(written && closed == 0, "bank write failed");
	synth.close();
	MT32Emu::ROMImage::freeROMImage(control); MT32Emu::ROMImage::freeROMImage(pcm);
	std::printf("{\"timbres\":%zu,\"pcm_rom_words\":262144,\"rendered_audio_bytes\":0,\"bytes\":%zu}\n", timbres.size()/246, out.size());
}

static std::map<std::string, Bytes> readDataSections(const char *path, bool package) {
	MT32Emu::FileStream file;
	require(file.open(path), "cannot open verification input");
	const uint8_t *data = file.getData();
	size_t size = file.getSize();
	require(size >= 12 && !std::memcmp(data, package ? "FCM1" : "FMB1", 4), "bad verification input");
	if (package) require(FCM::be16(data + 4) == 1 && FCM::be32(data + 8) == size, "bad FCM header");
	std::map<std::string, Bytes> sections;
	size_t pos = package ? 12 : 4;
	unsigned count = package ? FCM::be16(data + 6) : 0;
	for (unsigned i = 0; package ? i < count : pos < size; ++i) {
		require(pos <= size && size - pos >= (package ? 16u : 8u), "short section header");
		std::string tag(reinterpret_cast<const char *>(data + pos), 4);
		size_t offset = package ? FCM::be32(data + pos + 4) : pos + 8;
		size_t length = FCM::be32(data + pos + (package ? 8 : 4));
		require(offset <= size && length <= size - offset && !sections.count(tag), "bad section bounds");
		sections[tag] = Bytes(data + offset, data + offset + length);
		pos = package ? pos + 16 : offset + length;
	}
	return sections; // scan.cpp separately verifies package CRCs and ordering.
}

static void checkBank(const char *packagePath, const char *bankPath) {
	auto compiled = readDataSections(packagePath, true);
	auto original = readDataSections(bankPath, false);
	for (const auto &section : original) {
		if (section.first != "TIMB") require(compiled.at(section.first) == section.second, "bank data changed");
	}
	const Bytes &inst = compiled.at("INST"), &params = compiled.at("PARM");
	const Bytes &initial = compiled.at("PINI"), &curves = compiled.at("PVEL");
	require(inst.size() % 22 == 0 && params.size() % 58 == 0 && curves.size() % 128 == 0, "bad bank records");
	require(initial.size() == params.size() / 58 * 8, "bad initialization records");
	Bytes rebuilt;
	for (size_t i = 0; i < inst.size(); i += 22) {
		rebuilt.insert(rebuilt.end(), inst.begin() + i, inst.begin() + i + 14);
		for (unsigned slot = 0; slot < 4; ++slot) {
			size_t offset = size_t(FCM::be16(inst.data() + i + 14 + slot * 2)) * 58;
			require(offset <= params.size() && params.size() - offset >= 58, "bad partial reference");
			rebuilt.insert(rebuilt.end(), params.begin() + offset, params.begin() + offset + 58);
		}
	}
	require(rebuilt == original.at("TIMB"), "normalized timbres changed");
	const auto &tables = MT32Emu::Tables::getInstance();
	static_assert(sizeof(MT32Emu::TimbreParam::PartialParam) == 58, "partial layout changed");
	for (size_t i = 0; i < params.size() / 58; ++i) {
		MT32Emu::TimbreParam::PartialParam param;
		std::memcpy(&param, params.data() + i * 58, 58);
		const uint8_t *init = initial.data() + i * 8;
		size_t curve = size_t(FCM::be16(init)) * 128;
		require(curve <= curves.size() && curves.size() - curve >= 128, "bad velocity curve");
		require(param.wg.pulseWidth <= 100 && param.wg.pulseWidthVeloSensitivity <= 14 && param.tvf.resonance <= 30, "bad normalized parameter");
		for (int velocity = 0; velocity < 128; ++velocity) {
			int width = (velocity - 64) * (int(param.wg.pulseWidthVeloSensitivity) - 7) + tables.pulseWidth100To255[param.wg.pulseWidth];
			width = width < 0 ? 0 : width > 255 ? 255 : width;
			require(curves[curve + velocity] == width, "velocity response differs from Munt formula");
		}
		unsigned resonance = param.tvf.resonance + 1;
		require(FCM::be16(init + 2) == (32 - resonance) << 10 &&
		        FCM::be16(init + 4) == tables.resAmpDecayFactors[resonance >> 2] << 2 && FCM::be16(init + 6) == 0,
		        "resonance constants differ from Munt formula");
	}
	std::printf("{\"timbres\":%zu,\"partial_parameters\":%zu,\"velocity_values\":%zu,\"passed\":true}\n",
	            inst.size()/22, params.size()/58, params.size()/58*128);
}

int main(int argc, char **argv) {
	try {
		if (argc == 2 && !std::strcmp(argv[1], "test")) rampTest();
		else if (argc == 4 && !std::strcmp(argv[1], "check-bank")) checkBank(argv[2], argv[3]);
		else if ((argc == 5 || argc == 6) && !std::strcmp(argv[1], "export")) exportBank(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : nullptr);
		else throw std::runtime_error("usage: oracle test | oracle check-bank PACKAGE BANK_DUMP | oracle export CONTROL_ROM PCM_ROM NEW_OUTPUT [CUSTOM_DEFINITIONS]");
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what()); return 1;
	}
	return 0;
}
