// Dry, complete instrument captures plus independent held-out references.
// Usage: capture CONTROL_ROM PCM_ROM OUTPUT_DIRECTORY
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "mt32emu.h"

static bool capture(const MT32Emu::ROMImage &control, const MT32Emu::ROMImage &pcm,
                    const std::string &dir, FILE *manifest, int program, int key,
                    int velocity, bool early, bool heldOut) {
    MT32Emu::Synth synth;
    if (!synth.open(control, pcm, MT32Emu::DEFAULT_MAX_PARTIALS,
                    MT32Emu::AnalogOutputMode_DIGITAL_ONLY)) return false;
    synth.setReverbEnabled(false);
    synth.playMsgNow(0xC1 | (program << 8)); // MIDI channel 2, MT-32 part 1
    synth.playMsgNow(0xB1 | (10 << 8) | (64 << 16));
    synth.playMsgNow(0x91 | (key << 8) | (velocity << 16));
    const int hold = early ? 11200 : 256000; // 350 ms or eight seconds
    const int tail = 128000;
    std::vector<int16_t> stereo((hold + tail) * 2);
    synth.render(stereo.data(), hold);
    synth.playMsgNow(0x81 | (key << 8));
    synth.render(stereo.data() + hold * 2, tail);
    char file[128], name[32] = {};
    std::snprintf(file, sizeof file, "p%03d-k%03d-v%03d-%s.s16le", program,
                  key, velocity, early ? "early" : "long");
    synth.getSoundName(name, program / 64, program % 64);
    FILE *out = std::fopen((dir + "/" + file).c_str(), "wb");
    if (!out) return false;
    bool ok = true;
    // Explicit little endian mono: dry pan law is baked into this reference.
    for (int i = 0; i < hold + tail; ++i) {
        const int16_t mono = (int(stereo[2*i]) + int(stereo[2*i+1])) / 2;
        const uint8_t bytes[2] = {uint8_t(mono), uint8_t(uint16_t(mono) >> 8)};
        if (std::fwrite(bytes, 1, 2, out) != 2) { ok = false; break; }
    }
    ok = std::fclose(out) == 0 && ok;
    if (!ok) return false;
    std::fprintf(manifest, "%d\t%d\t%d\t%d\t%d\t%s\t%s\n", program,
                 key, velocity, hold, heldOut || early, file, name);
    std::printf("%s: %s\n", file, name);
    return true;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: capture CONTROL_ROM PCM_ROM OUTPUT_DIRECTORY\n");
        return 1;
    }
    MT32Emu::FileStream ctrlFile, pcmFile;
    if (!ctrlFile.open(argv[1]) || !pcmFile.open(argv[2])) {
        std::fprintf(stderr, "Cannot open ROM pair\n"); return 1;
    }
    const auto *ci = MT32Emu::ROMInfo::getROMInfo(&ctrlFile);
    const auto *pi = MT32Emu::ROMInfo::getROMInfo(&pcmFile);
    if (!ci || !pi || ci->type != MT32Emu::ROMInfo::Control ||
        pi->type != MT32Emu::ROMInfo::PCM || ci->pairType != MT32Emu::ROMInfo::Full ||
        pi->pairType != MT32Emu::ROMInfo::Full) {
        std::fprintf(stderr, "Unrecognised or incomplete ROM pair\n"); return 1;
    }
    std::printf("ROMs: %s + %s\n", ci->shortName, pi->shortName);
    const auto *control = MT32Emu::ROMImage::makeROMImage(&ctrlFile);
    const auto *pcm = MT32Emu::ROMImage::makeROMImage(&pcmFile);
    FILE *manifest = std::fopen((std::string(argv[3]) + "/captures.tsv").c_str(), "w");
    bool ok = manifest != nullptr;
    for (int program : {50, 92, 32}) {
        for (int key : {48, 60, 72})
            for (int velocity : {64, 100})
                if (ok) ok = capture(*control, *pcm, argv[3], manifest, program, key, velocity, false, false);
        if (ok) ok = capture(*control, *pcm, argv[3], manifest, program, 67, 80, false, true);
        if (ok) ok = capture(*control, *pcm, argv[3], manifest, program, 60, 100, true, true);
    }
    if (manifest) ok = std::fclose(manifest) == 0 && ok;
    MT32Emu::ROMImage::freeROMImage(control);
    MT32Emu::ROMImage::freeROMImage(pcm);
    return ok ? 0 : 1;
}
