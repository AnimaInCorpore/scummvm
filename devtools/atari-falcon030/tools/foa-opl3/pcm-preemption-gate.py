#!/usr/bin/env python3
"""Exhaustive preemption gate for AtariMixerManager::produceDspPeriod.

Extracts the real producer body, puts a preemption point before every
statement, and at each point (one per run) injects the nested tick's
extension call - unless an AtariInterruptsOff is alive there, which models
the masked hardware. After each run it checks the ring invariants: chunks
are consumed in order, none twice, none from past the tail, and the ring's
apparent fill stays consistent.

The producer runs with Timer A enabled, and a nested tick's extension
period re-enters it. Before the fix the ring check and the claim were
separate steps: an extension between them took the last chunk and left the
outer call reading past the tail (the ring then looked nearly full, the main
loop stopped refilling, and stale chunks played), and one between the claim
and the submit queued a later chunk ahead of an earlier one.

A pass on its own proves little - an earlier version of this harness
counted preemption points on the empty-ring path only and so never reached
the point before submit, passing on code that had the bug. So the gate also
runs a mutant with the interrupt guard removed and requires it to FAIL:
the harness must be able to see the race it is guarding against.
"""
import subprocess, sys, re
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE / 'build' / 'pcm-preemption'

PREAMBLE = r'''
#include <cstdint>
#include <cstdio>
#include <vector>
using uint32 = uint32_t; using uint64 = uint64_t; using int16 = int16_t;
namespace Audio { struct Mixer {
 enum { kPlainSoundType = 0, kMaxMixerVolume = 256 };
 bool isSoundTypeMuted(int) { return false; }
 int getVolumeForSoundType(int) { return 256; }
}; }
namespace OplPractical { enum { SC_MASTER_GAIN = 0x13 }; }
int g_mask, g_nested, g_point, g_target;
struct AtariInterruptsOff { AtariInterruptsOff() { ++g_mask; } ~AtariInterruptsOff() { --g_mask; } };
struct AtariDspAudio {
 enum { kPcmPerPeriod = 192 };
 struct Period { int sample = -1; } periods[8];
 int allocated = 0;
 std::vector<int> submitted;
 Period *beginPeriod(bool) { return &periods[allocated++]; }
 bool addEvent(Period *, int, int, uint32) { return true; }
 void setPcm(Period *p, const int16 *s) { p->sample = s ? *s : 0; }
 void submit(Period *p) { submitted.push_back(p->sample); }
};
struct AtariDspOPL {
 static void flushPending(AtariDspAudio *, AtariDspAudio::Period *) {}
 static AtariDspOPL *instance() { return nullptr; }
 void producePeriod(AtariDspAudio::Period *) {}
};
struct AtariMixerManager {
 enum { kDspPcmChunks = 20 };
 AtariDspAudio audio; Audio::Mixer mixer;
 AtariDspAudio *_dsp = &audio; Audio::Mixer *_mixer = &mixer;
 bool _audioSuspended = false; int _dspFmVolume = 256;
 int16 _dspPcmRing[kDspPcmChunks * AtariDspAudio::kPcmPerPeriod] = {};
 volatile int _dspPcmHead = 0, _dspPcmTail = 0;
 volatile bool _dspPcmTaking = false;
 int _dspPcmUnderruns = 0;
 static bool produceDspPeriod(void *, bool);
};
#define PREEMPT() do { if (!g_nested && ++g_point == g_target && g_mask == 0) { \
  g_nested = 1; produceDspPeriod(context, false); g_nested = 0; } } while (0)
'''

MAIN = r'''
int main() {
 int failures = 0, runs = 0, pointsSeen = 0;
 for (int filled = 0; filled <= 3; ++filled) {
  // Count this scenario's own path: an empty ring takes a shorter branch.
  int points = 0;
  { AtariMixerManager m; m._dspPcmTail = filled; g_point = 0; g_target = 0; g_nested = 0;
    AtariMixerManager::produceDspPeriod(&m, true); points = g_point; }
  pointsSeen += points;
  for (int target = 1; target <= points; ++target) {
   AtariMixerManager m;
   for (int i = 0; i < 20; ++i)   // live chunks 1000+i, stale slots 9000+i
    m._dspPcmRing[i * AtariDspAudio::kPcmPerPeriod] = (int16)(i < filled ? 1000 + i : 9000 + i);
   m._dspPcmTail = filled;
   g_point = 0; g_target = target; g_nested = 0; g_mask = 0;
   AtariMixerManager::produceDspPeriod(&m, true);
   ++runs;
   std::vector<int> taken;
   for (int v : m.audio.submitted) if (v) taken.push_back(v);
   bool ok = (int)taken.size() <= filled;
   for (size_t k = 0; k < taken.size(); ++k) if (taken[k] != 1000 + (int)k) ok = false;
   const int head = m._dspPcmHead, fill = (m._dspPcmTail - head + 20) % 20;
   if (head != (int)taken.size() || fill != filled - (int)taken.size()) ok = false;
   if (!ok) {
    ++failures;
    std::printf("FAIL filled=%d point=%d submitted=", filled, target);
    for (int v : m.audio.submitted) std::printf("%d ", v);
    std::printf("head=%d apparent_fill=%d\n", head, fill);
   }
  }
 }
 std::printf("%d preemption points over 4 fills, %d runs, %d failures\n", pointsSeen, runs, failures);
 return failures ? 1 : 0;
}
'''

def extract(source):
    start = source.index('bool AtariMixerManager::produceDspPeriod(')
    end = source.index('\n}\n#endif', start) + 2
    lines = source[start:end].split('\n')
    out, prev = [lines[0]], lines[0].rstrip()
    for line in lines[1:-1]:
        st = line.strip()
        boundary = prev.endswith((';', '{', '}'))
        if st and boundary and not st.startswith(('}', 'else', '//', '{', ':')):
            indent = line[:len(line) - len(line.lstrip())]
            out.append(indent + 'PREEMPT();')
        out.append(line)
        if st and not st.startswith('//'):
            prev = line.rstrip()
    out.append(lines[-1])
    return '\n'.join(out)

def run(label, source):
    BUILD.mkdir(parents=True, exist_ok=True)
    cpp = BUILD / f'{label}.cpp'
    exe = BUILD / label
    cpp.write_text(PREAMBLE + extract(source) + MAIN)
    subprocess.run(['c++', '-std=c++11', '-O1', str(cpp), '-o', str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(f'--- {label} (exit {r.returncode})')
    lines = r.stdout.strip().split('\n')
    for l in lines[:6] + (['  ...'] if len(lines) > 7 else []) + lines[-1:] if len(lines) > 7 else lines:
        print('  ' + l)
    return r.returncode

import json
ROOT = HERE.parents[3]
source = (ROOT / 'backends/mixer/atari/atari-mixer.cpp').read_text()
guard = 'AtariInterruptsOff off;'
if source.count(guard) != 1:
    sys.exit('the producer no longer holds exactly one AtariInterruptsOff; update this gate')
rc_current = run('current', source)
rc_mutant = run('mutant-no-guard', source.replace(guard, ''))
passed = rc_current == 0 and rc_mutant != 0
result = {
    'gate': 'produceDspPeriod under a nested extension at every statement boundary, 0-3 chunks queued',
    'current_passes': rc_current == 0,
    'mutant_without_guard_fails': rc_mutant != 0,
    'passed': passed,
}
(HERE / 'pcm-preemption-results.json').write_text(json.dumps(result, indent=2) + '\n')
print()
print('current passes and the unguarded mutant fails:', 'PASSED' if passed else 'FAILED')
sys.exit(0 if passed else 1)
