// Render an actual post-iMUSE device trace with Munt. MIDI is queued at its
// native 32 kHz timestamp, so render block boundaries cannot move events early.
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "mt32emu.h"

struct Event { uint64_t us; std::vector<uint8_t> bytes; };

static bool readTrace(const char *path, std::vector<Event> &events) {
    std::ifstream in(path);
    std::string line;
    bool ended = false;
    while (std::getline(in, line)) {
        if (line == "# end") { ended = true; continue; }
        if (line.empty() || line[0] == '#') continue;
        if (ended || line[0] == '-') return false;
        Event e;
        std::string hex, extra;
        std::istringstream row(line);
        if (!(row >> e.us >> hex) || row >> extra || e.us > 3600000000ULL ||
            hex.size()%2 || hex.size()<4 || (!events.empty() && e.us<events.back().us)) return false;
        for (size_t p=0; p<hex.size(); p+=2) {
            const std::string pair = hex.substr(p, 2);
            if (pair.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
            e.bytes.push_back(uint8_t(std::strtoul(pair.c_str(), nullptr, 16)));
        }
        if (e.bytes[0] == 0xf0) {
            if (e.bytes.back()!=0xf7) return false;
            for (size_t p=1; p+1<e.bytes.size(); ++p) if (e.bytes[p]>=128) return false;
        } else {
            const unsigned hi=e.bytes[0]&0xf0;
            if (hi<0x80 || hi>0xe0 || e.bytes.size()!=((hi==0xc0 || hi==0xd0)?2U:3U)) return false;
            for (size_t p=1; p<e.bytes.size(); ++p) if (e.bytes[p]>=128) return false;
        }
        events.push_back(e);
    }
    return ended && !events.empty();
}

static void le32(FILE *f, uint32_t n) {
    for (unsigned i=0; i<4; ++i) std::fputc((n>>(i*8))&255, f);
}

int main(int argc, char **argv) {
    if (argc<5 || argc>6) {
        std::fprintf(stderr, "usage: render-trace CONTROL_ROM PCM_ROM TRACE OUTPUT.wav [BLOCK_FRAMES]\n"); return 1;
    }
    const unsigned block=argc==6 ? unsigned(std::atoi(argv[5])) : 48;
    if (!block || block>4096) return 1;
    std::vector<Event> events;
    if (!readTrace(argv[3], events)) { std::fprintf(stderr, "Invalid/incomplete trace\n"); return 1; }
    MT32Emu::FileStream cf, pf;
    if (!cf.open(argv[1]) || !pf.open(argv[2])) return 1;
    const auto *ci=MT32Emu::ROMInfo::getROMInfo(&cf), *pi=MT32Emu::ROMInfo::getROMInfo(&pf);
    if (!ci || !pi || ci->type!=MT32Emu::ROMInfo::Control || pi->type!=MT32Emu::ROMInfo::PCM ||
        ci->pairType!=MT32Emu::ROMInfo::Full || pi->pairType!=MT32Emu::ROMInfo::Full) return 1;
    // This experiment has one explicit reference model, including its DAC
    // wiring. Do not silently use that profile for a later-generation ROM.
    if (std::string(ci->shortName)!="ctrl_mt32_1_07" || std::string(pi->shortName)!="pcm_mt32") {
        std::fprintf(stderr, "Reference profile requires MT-32 control 1.07 and MT-32 PCM\n"); return 1;
    }
    const auto *control=MT32Emu::ROMImage::makeROMImage(&cf), *pcm=MT32Emu::ROMImage::makeROMImage(&pf);
    std::srand(1); // Fixed host-libc seed for Munt's pitch-timer jitter model.
    MT32Emu::Synth synth;
    synth.selectRendererType(MT32Emu::RendererType_BIT16S);
    synth.setDACInputMode(MT32Emu::DACInputMode_GENERATION1);
    synth.setNiceAmpRampEnabled(false);
    synth.setNicePanningEnabled(false);
    synth.setNicePartialMixingEnabled(false);
    if (!synth.open(*control, *pcm, 32, MT32Emu::AnalogOutputMode_ACCURATE)) return 1;
    synth.setReverbEnabled(true);
    synth.setMIDIDelayMode(MT32Emu::MIDIDelayMode_DELAY_SHORT_MESSAGES_ONLY);
    synth.setMIDIEventQueueSize(uint32_t(events.size()+1));
    unsigned noteOns=0, sysex=0;
    for (const auto &e:events) {
        const uint32_t at=uint32_t(e.us*32000/1000000);
        bool ok;
        if (e.bytes[0]==0xf0) {
            ok=synth.playSysex(e.bytes.data(), uint32_t(e.bytes.size()), at);
            ++sysex;
        } else {
            uint32_t msg=0;
            for (size_t i=0; i<e.bytes.size(); ++i) msg|=uint32_t(e.bytes[i])<<(8*i);
            ok=synth.playMsg(msg, at);
            if ((e.bytes[0]&0xf0)==0x90 && e.bytes[2]) ++noteOns;
        }
        if (!ok) { std::fprintf(stderr, "MIDI queue overflow\n"); return 1; }
    }
    const uint32_t rate=synth.getStereoOutputSampleRate();
    const uint32_t frames=uint32_t((events.back().us+8000000)*rate/1000000);
    FILE *out=std::fopen(argv[4], "wb");
    if (!out) return 1;
    std::fwrite("RIFF",1,4,out); le32(out,36+frames*4);
    std::fwrite("WAVEfmt ",1,8,out); le32(out,16); le32(out,0x00020001);
    le32(out,rate); le32(out,rate*4); le32(out,0x00100004);
    std::fwrite("data",1,4,out); le32(out,frames*4);
    std::vector<int16_t> buffer(block*2);
    std::vector<uint8_t> encoded(block*4);
    uint32_t rendered=0, peakPartials=0;
    uint64_t nonzero=0, saturated=0;
    int peak=0;
    while (rendered<frames) {
        const uint32_t count=std::min<uint32_t>(block, frames-rendered);
        synth.render(buffer.data(), count);
        for (uint32_t i=0; i<count*2; ++i) {
            int value=buffer[i];
            peak=std::max(peak, std::abs(value));
            nonzero+=value!=0;
            saturated+=value==32767 || value==-32768;
            encoded[i*2]=uint8_t(value); encoded[i*2+1]=uint8_t(uint16_t(value)>>8);
        }
        if (std::fwrite(encoded.data(), 4, count, out)!=count) return 1;
        MT32Emu::PartialState states[32];
        synth.getPartialStates(states);
        uint32_t active=0;
        for (auto state:states) active+=state!=MT32Emu::PartialState_INACTIVE;
        peakPartials=std::max(peakPartials,active);
        rendered+=count;
    }
    const bool active=synth.isActive();
    bool ok=std::fclose(out)==0;
    std::printf("{\"control_rom\":\"%s\",\"pcm_rom\":\"%s\",\"events\":%zu,\"note_ons\":%u,"
                "\"renderer\":\"BIT16S\",\"dac_input_mode\":\"GENERATION1\",\"analog_output_mode\":\"ACCURATE\","
                "\"nice_amp_ramp\":false,\"nice_panning\":false,\"nice_partial_mixing\":false,\"random_seed\":1,"
                "\"sysex\":%u,\"rate\":%u,\"frames\":%u,\"block_frames\":%u,"
                "\"peak_partials_sampled\":%u,\"peak_sample\":%d,\"nonzero_words\":%llu,"
                "\"saturated_words\":%llu,\"active_at_end\":%s}\n",
                ci->shortName,pi->shortName,events.size(),noteOns,sysex,rate,frames,block,peakPartials,peak,
                (unsigned long long)nonzero,(unsigned long long)saturated,active?"true":"false");
    if (active) std::fprintf(stderr, "Synth remains active after tail; reference is incomplete\n");
    synth.close();
    MT32Emu::ROMImage::freeROMImage(control); MT32Emu::ROMImage::freeROMImage(pcm);
    return ok && nonzero && !active ? 0 : 1;
}
