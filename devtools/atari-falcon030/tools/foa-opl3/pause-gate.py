#!/usr/bin/env python3
"""Check nested mixer pauses through the real mixer and DSP producer bodies.

The host audio device is stubbed; callbacks, PCM delivery and the pause event
are observed separately. A mutant that ignores pause must fail. The kernel's
state preservation is covered by practical-unit-test and the DSP paths bench.
"""
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BUILD = HERE / 'build/pause'


def extract(source, marker):
    start = source.index(marker)
    end = source.index('{', start)
    depth = 0
    while True:
        depth += {'{': 1, '}': -1}.get(source[end], 0)
        end += 1
        if not depth:
            return source[start:end]


PREAMBLE = r'''
#include <cstdint>
#include <cstdio>
#include "devtools/atari-falcon030/tools/foa-opl3/opl-practical.h"
using uint = unsigned; using uint32 = uint32_t; using uint64 = uint64_t; using int16 = int16_t;
int criticalDepth, failures;
void check(bool ok, const char *what) { if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
struct AtariCriticalSection {
 AtariCriticalSection() { ++criticalDepth; } ~AtariCriticalSection() { --criticalDepth; }
};
struct AtariInterruptsOff {};
namespace Audio {
struct Mixer { enum { kPlainSoundType = 0, kMaxMixerVolume = 256 }; };
struct MixerImpl : Mixer {
 unsigned pauseLevel = 0;
 MixerImpl(uint, bool, uint, uint, bool) {}
 virtual ~MixerImpl() {}
 virtual void pauseAll(bool paused) {
  check(criticalDepth > 0, "PCM and FM pause changes must be protected together");
  if (paused) ++pauseLevel; else if (pauseLevel) --pauseLevel;
 }
 bool isSoundTypeMuted(int) { return false; }
 int getVolumeForSoundType(int) { return 256; }
};
}
'''

STUBS = r'''
struct AtariDspAudio {
 enum { kPeriodFrames = 768, kBlockFrames = 64, kPeriodBlocks = 12, kPcmPerPeriod = 192 };
 struct Period { int paused = -1; int sample = -1; } period;
 int submitted = 0, lastPause = -1, lastSample = -1;
 Period *beginPeriod(bool) { period = Period(); return &period; }
 bool addEvent(Period *p, uint32, uint16_t address, uint32 value) {
  if (address == OplPractical::SC_PAUSED) p->paused = value;
  return true;
 }
 void setPcm(Period *p, const int16 *samples) { p->sample = samples ? *samples : 0; }
 void submit(Period *p) { ++submitted; lastPause = p->paused; lastSample = p->sample; }
};
struct Callback {
 int calls = 0;
 bool isValid() { return true; }
 void operator()() { ++calls; }
};
struct Decoder { void flush() {} };
struct AtariDspOPL {
 AtariDspAudio::Period *_period = nullptr;
 uint32 _block = 0;
 bool _running = true;
 uint32 _framesPerTick16 = (uint32)((OPL_PRACTICAL_CODEC_RATE / 250) * 65536.0 + 0.5);
 uint32 _nextTick16 = 0;
 Callback callback, *_callback = &callback;
 Decoder decoder, *_decoder = &decoder;
 static AtariDspOPL *current;
 static AtariDspOPL *instance() { return current; }
 static void flushPending(AtariDspAudio *, AtariDspAudio::Period *) {}
 void producePeriod(AtariDspAudio::Period *);
};
AtariDspOPL *AtariDspOPL::current;
struct AtariMixerManager {
 enum { kDspPcmChunks = 20 };
 volatile uint _dspPauseLevel = 0;
 AtariDspAudio audio, *_dsp = &audio;
 AtariDspMixer mixer{12292, 192, _dspPauseLevel};
 Audio::MixerImpl *_mixer = &mixer;
 bool _audioSuspended = false;
 int _dspFmVolume = -1, _dspPcmHead = 0, _dspPcmTail = 0, _dspPcmUnderruns = 0;
 int16 _dspPcmRing[kDspPcmChunks * AtariDspAudio::kPcmPerPeriod] = {};
 static bool produceDspPeriod(void *, bool);
 void period(bool callbacks = true) {
  _dspPcmRing[_dspPcmHead * AtariDspAudio::kPcmPerPeriod] = 123;
  _dspPcmTail = (_dspPcmHead + 1) % kDspPcmChunks;
  const int before = audio.submitted;
  check(produceDspPeriod(this, callbacks), "period must still be produced");
  check(audio.submitted == before + 1 && audio.lastSample == 123, "pause must keep PCM delivery running");
 }
};
'''

MAIN = r'''
int main() {
 AtariDspOPL opl; AtariDspOPL::current = &opl;
 AtariMixerManager mixer;
 mixer.period();
 check(opl.callback.calls > 0 && mixer.audio.lastPause == 0, "unpaused callbacks");
 const int calls = opl.callback.calls;
 const uint32 nextTick = opl._nextTick16;
 mixer.mixer.pauseAll(true);
 mixer.mixer.pauseAll(true);
 for (int i = 0; i < 16; ++i) {
  mixer.period();
  check(mixer.audio.lastPause == 1, "paused kernel event");
 }
 mixer.period(false);
 check(mixer.audio.lastPause == 1, "extensions must also keep FM paused");
 mixer.mixer.pauseAll(false);
 mixer.period();
 check(mixer._dspPauseLevel == 1 && mixer.mixer.pauseLevel == 1, "nested pause depth");
 check(opl.callback.calls == calls && opl._nextTick16 == nextTick, "paused callbacks and timer phase must hold");
 check(mixer.audio.lastPause == 1, "one unpause must not resume a nested pause");
 mixer.mixer.pauseAll(false);
 mixer.period();
 check(opl.callback.calls > calls && mixer.audio.lastPause == 0, "resume callbacks and kernel together");
 mixer.mixer.pauseAll(false);
 check(mixer._dspPauseLevel == 0 && mixer.mixer.pauseLevel == 0, "unmatched resume must not underflow");
 check(criticalDepth == 0, "balanced critical sections");
 std::printf("%d failures; %d periods delivered\n", failures, mixer.audio.submitted);
 return failures ? 1 : 0;
}
'''


def run(label, mixer_source, opl_source):
    code = (PREAMBLE + extract(mixer_source, 'class AtariDspMixer final') + ';\n' + STUBS
            + extract(opl_source, 'void AtariDspOPL::producePeriod(')
            + extract(mixer_source, 'bool AtariMixerManager::produceDspPeriod(') + MAIN)
    BUILD.mkdir(parents=True, exist_ok=True)
    cpp, binary = BUILD / (label + '.cpp'), BUILD / label
    cpp.write_text(code)
    subprocess.run(['c++', '-std=c++11', '-O2', '-I' + str(ROOT), str(cpp), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(label + ': ' + result.stdout.strip())
    return result.returncode


def main():
    mixer = (ROOT / 'backends/mixer/atari/atari-mixer.cpp').read_text()
    opl = (ROOT / 'backends/platform/atari/dsp-opl.cpp').read_text()
    pause = 'const bool paused = self->_dspPauseLevel != 0;'
    if mixer.count(pause) != 1:
        raise SystemExit('producer pause expression changed; update the gate')
    current = run('current', mixer, opl)
    mutant = run('mutant-ignore-pause', mixer.replace(pause, 'const bool paused = false;'), opl)
    result = {'current_passes': current == 0, 'mutant_ignoring_pause_fails': mutant != 0,
              'passed': current == 0 and mutant != 0}
    (BUILD / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
