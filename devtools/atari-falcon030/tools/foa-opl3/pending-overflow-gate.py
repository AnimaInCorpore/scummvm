#!/usr/bin/env python3
"""Overflow gate for the DSP OPL event path (AtariDspOPL::emit/flushPending).

The decoder puts a value in its shadow before the sink sees it, so an event
that never reaches the kernel used to leave the two out of step for good:
the next equal write is suppressed as unchanged, and a dropped key-off was a
note nothing could stop. Now a lost event is noted, and the next period
resends the decoder's shadow.

This builds the real decoder, the real Chip model standing in for the DSP
kernel, and the real emit, noteLost, resetDecoder and flushPending taken
from dsp-opl.cpp, with only the transport stubbed (a period holds
kMaxEvents events, as on the machine). Scenarios:

  pending   more writes between periods than the queue holds, ending in a
            key-off - the reproduction from the review that found this
  period    more events inside one period than it holds
  reset     a reset whose own events do not fit the period it is made in
  normal    no overflow: the queue replays exactly as before, no resync

After the flush that follows each overflow, every word the host owns must
equal the decoder's shadow, the key must be off in the kernel as it is in
the decoder, and after a lost reset the machine's words must be the reset's.

A pass alone proves little, so the gate also runs a mutant with the resend
removed and requires the three overflow scenarios to FAIL on it.
"""
import json, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BUILD = HERE / 'build' / 'pending-overflow'

def grab(source, start_marker, end_marker=None):
    """A function body by brace matching, or the text between two markers."""
    i = source.index(start_marker)
    if end_marker:
        return source[i:source.index(end_marker, i) + len(end_marker)]
    depth, j = 0, source.index('{', i)
    while True:
        depth += {'{': 1, '}': -1}.get(source[j], 0)
        j += 1
        if depth == 0:
            return source[i:j]

def constant(text, name):
    return int(re.search(name + r'\s*=\s*(\d+)', text).group(1))

PREAMBLE = r'''
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "devtools/atari-falcon030/tools/foa-opl3/opl-practical.h"
using uint32 = uint32_t; using uint16 = uint16_t; using int32 = int32_t;
namespace P = OplPractical;
namespace OplPractical { using namespace ::OplPractical; }
struct AtariDspAudio {
 enum { kMaxEvents = %(maxEvents)d };
 struct Period { unsigned events = 0; };
 P::Chip chip;
 unsigned accepted = 0;
 AtariDspAudio() { P::reset(&chip, 9); }
 bool addEvent(Period *p, uint32, uint16 address, uint32 value) {
  if (p->events >= kMaxEvents) return false;
  ++p->events; ++accepted;
  P::poke(&chip, address, (int32)value);
  return true;
 }
};
struct AtariDspOPL {
 struct EventSink : P::Sink {
  AtariDspOPL *owner;
  void write(uint32 block, uint16 address, int32 value) override { owner->emit(block, address, value); }
 };
 AtariDspAudio *_audio;
 AtariDspAudio::Period *_period = nullptr;
 EventSink _sink;
 P::Decoder *_decoder = new P::Decoder;
 bool _resetting = false;
 enum { kPendingMax = %(pendingMax)d };
 static uint32 s_pending[kPendingMax * 2], s_pendingCount;
 static bool s_resync, s_resyncMachine;
 static AtariDspOPL *s_instance;
 explicit AtariDspOPL(AtariDspAudio *a) : _audio(a) { _sink.owner = this; s_pendingCount = 0; resetDecoder(0); s_instance = this; }
 void emit(uint32, uint16, int32);
 void noteLost();
 void resetDecoder(uint32);
 static void flushPending(AtariDspAudio *, AtariDspAudio::Period *);
};
uint32 AtariDspOPL::s_pending[AtariDspOPL::kPendingMax * 2];
uint32 AtariDspOPL::s_pendingCount;
bool AtariDspOPL::s_resync, AtariDspOPL::s_resyncMachine;
AtariDspOPL *AtariDspOPL::s_instance;
'''

MAIN = r'''
static const int kHostOp[] = { P::OP_TRIG, P::OP_FLAGS, P::OP_SL, P::OP_RATE_A, P::OP_RATE_D, P::OP_RATE_S,
 P::OP_RATE_R, P::OP_TLKSL, P::OP_INCBASE, P::OP_WFBASE, P::OP_VIBDELTA, P::OP_VIBDELTA + 1,
 P::OP_VIBDELTA + 2, P::OP_VIBDELTA + 3, P::OP_VIBDELTA + 4 };
static const int kMachineOp[] = { P::OP_TRIGSEEN, P::OP_STATE, P::OP_ENV };

// Words where the kernel and the decoder disagree; machine words too if asked.
static int mismatches(AtariDspAudio &a, AtariDspOPL &o, bool machine) {
 int bad = 0;
 for (int i = 0; i < 18; ++i) {
  for (int w : kHostOp) if (a.chip.op[i].w[w] != o._decoder->shadowOp[i][w]) ++bad;
  if (machine) for (int w : kMachineOp) if (a.chip.op[i].w[w] != o._decoder->shadowOp[i][w]) ++bad;
 }
 for (int c = 0; c < 9; ++c)
  for (int w : { P::CH_CONN, P::CH_FBMUL })
   if (a.chip.ch[c].w[w] != o._decoder->shadowChannel[c][w]) ++bad;
 return bad;
}

// One register write, its words derived at once: every write here stands for
// one in a block of its own, so each reaches the sink - the queue, or the
// period - as it did before the decoder coalesced a block's writes.
static void write(AtariDspOPL &o, uint16 reg, uint8_t value) {
 o._decoder->write(0, reg, value);
 o._decoder->flush();
}

static void keyedNote(AtariDspAudio &a, AtariDspOPL &o) {
 AtariDspAudio::Period p;
 write(o, 0x23, 0x21); write(o, 0x63, 0xf0);
 write(o, 0xa0, 0x41); write(o, 0xb0, 0x32);
 AtariDspOPL::flushPending(&a, &p);
}

int main() {
 int failures = 0;
 auto report = [&](const char *name, bool ok, const char *detail) {
  std::printf("%-8s %s  %s\n", name, ok ? "ok  " : "FAIL", detail);
  if (!ok) ++failures;
 };
 char d[160];

 { // pending: the review's reproduction - more writes between periods than the queue holds
  AtariDspAudio a; AtariDspOPL o(&a); keyedNote(a, o);
  for (int i = 0; i < 2 * AtariDspOPL::kPendingMax; ++i) write(o, 0x43, (i & 1) ? 0 : 1);
  write(o, 0xb0, 0x12);                    // key off, lost with the overflow
  AtariDspAudio::Period p; AtariDspOPL::flushPending(&a, &p);
  const int key = a.chip.op[1].w[P::OP_FLAGS] & 1, bad = mismatches(a, o, false);
  std::snprintf(d, sizeof d, "decoder key %d, kernel key %d, %d words out of step, resync still due %d",
   o._decoder->channel[0].key, key, bad, (int)AtariDspOPL::s_resync);
  report("pending", key == 0 && bad == 0 && !AtariDspOPL::s_resync, d);
 }
 { // period: more events inside one period than it holds
  AtariDspAudio a; AtariDspOPL o(&a); keyedNote(a, o);
  AtariDspAudio::Period full; o._period = &full;
  for (int i = 0; i < 2 * AtariDspAudio::kMaxEvents; ++i) write(o, 0x43, (i & 1) ? 0 : 1);
  write(o, 0xb0, 0x12);
  o._period = nullptr;
  AtariDspAudio::Period next; AtariDspOPL::flushPending(&a, &next);
  const int key = a.chip.op[1].w[P::OP_FLAGS] & 1, bad = mismatches(a, o, false);
  std::snprintf(d, sizeof d, "decoder key %d, kernel key %d, %d words out of step", o._decoder->channel[0].key, key, bad);
  report("period", key == 0 && bad == 0 && !AtariDspOPL::s_resync, d);
 }
 { // reset: a reset whose own events do not fit the period it is made in
  AtariDspAudio a; AtariDspOPL o(&a); keyedNote(a, o);
  AtariDspAudio::Period nearlyFull; o._period = &nearlyFull;
  for (int i = 0; nearlyFull.events < AtariDspAudio::kMaxEvents - 20; ++i) write(o, 0x43, (i & 1) ? 0 : 1);
  o.resetDecoder(0);                                   // most of it will not fit
  const bool machineDue = AtariDspOPL::s_resyncMachine;
  o._period = nullptr;
  AtariDspAudio::Period next; AtariDspOPL::flushPending(&a, &next);
  const int bad = mismatches(a, o, true);
  std::snprintf(d, sizeof d, "machine resync scheduled %d, %d words out of step incl. machine words",
   (int)machineDue, bad);
  report("reset", machineDue && bad == 0 && !AtariDspOPL::s_resync, d);
 }
 { // normal: no overflow, the queue replays exactly as before and nothing is resent
  AtariDspAudio a; AtariDspOPL o(&a); keyedNote(a, o);
  for (int i = 0; i < 100; ++i) write(o, 0x43, (i & 1) ? 0 : 1);
  const unsigned queued = AtariDspOPL::s_pendingCount, before = a.accepted;
  AtariDspAudio::Period p; AtariDspOPL::flushPending(&a, &p);
  const unsigned sent = a.accepted - before;
  std::snprintf(d, sizeof d, "queued %u, sent %u, %d words out of step", queued, sent, mismatches(a, o, false));
  report("normal", sent == queued && mismatches(a, o, false) == 0 && !AtariDspOPL::s_resync, d);
 }
 std::printf("%d failures\n", failures);
 return failures ? 1 : 0;
}
'''

def run(label, opl_source, header, audio_header):
    body = '\n\n'.join([
        grab(opl_source, 'void AtariDspOPL::resetDecoder('),
        grab(opl_source, 'void AtariDspOPL::noteLost('),
        grab(opl_source, 'void AtariDspOPL::emit('),
        grab(opl_source, 'namespace {\n\n// A resend straight into a period', '} // End of anonymous namespace'),
        grab(opl_source, 'void AtariDspOPL::flushPending('),
    ])
    pre = PREAMBLE % {'maxEvents': constant(audio_header, 'kMaxEvents'),
                      'pendingMax': constant(header, 'kPendingMax')}
    BUILD.mkdir(parents=True, exist_ok=True)
    cpp, exe = BUILD / f'{label}.cpp', BUILD / label
    cpp.write_text(pre + body + MAIN)
    subprocess.run(['c++', '-std=c++11', '-O1', '-I' + str(ROOT), str(cpp), '-o', str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(f'--- {label} (exit {r.returncode})')
    for line in r.stdout.strip().split('\n'):
        print('  ' + line)
    return r.returncode

source = (ROOT / 'backends/platform/atari/dsp-opl.cpp').read_text()
header = (ROOT / 'backends/platform/atari/dsp-opl.h').read_text()
audio_header = (ROOT / 'backends/platform/atari/atari-dsp.h').read_text()
resend = 's_instance->_decoder->resend(&sink, 0, s_resyncMachine);'
if source.count(resend) != 1:
    sys.exit('flushPending no longer resends exactly once; update this gate')
rc_current = run('current', source, header, audio_header)
rc_mutant = run('mutant-no-resend', source.replace(resend, ''), header, audio_header)
passed = rc_current == 0 and rc_mutant != 0
(HERE / 'pending-overflow-results.json').write_text(json.dumps({
    'gate': 'emit/flushPending with the real decoder and Chip: queue, period and reset overflow, and no overflow',
    'current_passes': rc_current == 0,
    'mutant_without_resend_fails': rc_mutant != 0,
    'passed': passed,
}, indent=2) + '\n')
print()
print('current passes and the mutant without the resend fails:', 'PASSED' if passed else 'FAILED')
sys.exit(0 if passed else 1)
