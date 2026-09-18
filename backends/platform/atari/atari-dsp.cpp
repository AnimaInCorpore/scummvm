/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef ATARI_DSP_OPL

#define FORCE_TEXT_CONSOLE
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "backends/platform/atari/atari-dsp.h"

#include <mint/falcon.h>
#include <mint/osbind.h>

#include "common/debug.h"
#include "common/textconsole.h"

#include "backends/platform/atari/atari-critical.h"
#include "backends/platform/atari/dsp-opl-image.h"
#include "devtools/atari-falcon030/tools/foa-opl3/opl-practical.h"

// The transport's constants are literals in the header so that it needs no
// kernel header; they must be the kernel's.
static_assert(AtariDspAudio::kBlockFrames == OPL_PRACTICAL_BLOCK_FRAMES, "block frames differs from the kernel");
static_assert(AtariDspAudio::kPeriodBlocks == OPL_PRACTICAL_PERIOD_BLOCKS, "period blocks differs from the kernel");
static_assert(AtariDspAudio::kPeriodFrames == OPL_PRACTICAL_BLOCK_FRAMES * OPL_PRACTICAL_PERIOD_BLOCKS, "period frames differs from the kernel");
static_assert(AtariDspAudio::kPcmPerPeriod * OPL_PRACTICAL_PCM_DIVIDER == AtariDspAudio::kPeriodFrames, "pcm per period differs from the kernel");

// The interrupt handler's assembly refers to these by name.
extern "C" {
void *atari_dsp_saved_stack;
void *atari_dsp_stack_top;
volatile unsigned char atari_dsp_producing;   // a production call is in progress
void atari_dsp_timer_a();
long atari_dsp_timer_tick(unsigned long interruptedLevel, unsigned long interruptedPc);
void atari_dsp_produce();
}

namespace {

// Kernel commands, in the command word's top byte.
enum {
	kCmdPing = 0x010000,
	kCmdWriteX = 0x020000,
	kCmdWriteY = 0x030000,
	kCmdStreamStart = 0x090000,
	kCmdRefill = 0x0a0000,
	kCmdStreamStop = 0x0b0000,
	kCmdStatus = 0x0c0000,
	kCmdChecksum = 0x0d0000,
	kReplyPing = 0x4f5052,
	kReplyReady = 0x524459
};

const int kDspAbility = 3;

// The Falcon host port. ISR bit 0 is RXDF, bit 1 TXDE; a long write to the
// data register covers all three bytes and the low byte strobes the
// transfer, and the low byte must be read last for the same reason.
volatile uint8 *const kHostIsr = (volatile uint8 *)0xFFFFA202L;
volatile uint32 *const kHostData = (volatile uint32 *)0xFFFFA204L;
volatile uint8 *const kHostDataBytes = (volatile uint8 *)0xFFFFA204L;

// Delivery state, owned by the interrupt handler once the stream runs. The
// producer enqueues finished periods at the tail; the handler walks the
// queue from the head, one protocol step per tick. Both sides only ever
// write their own index, so no lock is needed.
enum { kQueueSize = AtariDspAudio::kPeriods + 1 };
AtariDspAudio::Period *volatile s_queue[kQueueSize];
volatile int s_head, s_tail;
volatile int s_pollState;          // 0 idle, 1 announced, 2 sent
volatile uint32 s_status;          // the kernel's last acknowledgement word
volatile uint32 s_protocolErrors;
volatile bool s_deliver;           // the handler acts only while streaming
uint32 s_command;
uint32 s_reply;

// Production state. The producer runs inside the handler with interrupts
// enabled, so a nested tick delivers but never produces.
AtariDspAudio::Producer volatile s_producer;
void *s_producerContext;
#define s_producing atari_dsp_producing

// The producer's stack. Timer A interrupts whatever runs, and what runs
// may be TOS itself on its own small supervisor stack.
enum { kProductionStackBytes = 32 * 1024 };
uint8 s_productionStack[kProductionStackBytes] __attribute__((aligned(16)));

// Why production was refused, in ticks, for the log: the longest streak of
// refused ticks while a period was due, how many refusals were a mutex,
// the allocator or an interrupted handler, and the extension periods.
volatile uint32 s_refusedStreak, s_refusedStreakMax, s_refusedMutex, s_refusedAllocator, s_refusedLevel, s_extended;
volatile uint32 s_produceTicks, s_produceTicksMax;   // nested ticks during one production call: its length in ms
volatile uint32 s_emptyTicks, s_emptyStreak, s_emptyStreakMax;   // ticks with nothing queued
// Inside the longest production session: how many periods its loop produced,
// and how many register events they carried. A session that lasts because the
// loop cannot outrun delivery shows many periods at about a period's length
// each; one that lasts because a burst of iMUSE writes is expensive shows few
// periods and a large event count.
volatile uint32 s_produceLoops, s_produceEvents;
volatile uint32 s_produceLoopsAtMax, s_produceEventsAtMax;
// Which phase of a period the producer is in, sampled by the nested tick:
// the OPL timer callbacks, or the framing and the PCM copy around them.
// s_produceTicks is wall time, so this says where the main loop's freeze went.
volatile uint32 s_phaseTicks[2], s_callbackTicksAtMax, s_callbacksAtMax;
// DIAGNOSTIC: where the 68030 actually is once a production session has run
// long enough to be one of the pathological ones. The tick interrupts the
// producer, so the exception frame's PC is the producer's own code.
// Bucketed by 4 KB page, not by exact PC: a hot loop covers many addresses,
// and a table keyed on each one fills with whatever arrived first and then
// throws the rest away. Direct-mapped, and a page that is hit more often
// takes a contested slot, so the ones worth seeing survive.
enum { kPcBuckets = 512, kPcPageShift = 12, kPcSampleAfterMs = 10, kPcSessionMax = 512 };
struct PcHistogram {
	uint32 page[kPcBuckets], hits[kPcBuckets], total, lost, sessions;
};
// Kept apart by what the session produced: [0] sessions that emitted no
// register event, [1] the rest. The two slow kinds turned out to be
// different problems, and one histogram over both can only be read against
// neither - which is how a false explanation got into a commit once.
PcHistogram s_pcHist[2];
// A session's samples wait here until it ends, when its event count is final.
volatile uint32 s_pcSession[kPcSessionMax], s_pcSessionCount;

void samplePc(uint32 pc) {
	if (s_pcSessionCount < kPcSessionMax)
		s_pcSession[s_pcSessionCount++] = pc;
}

void histogramAdd(PcHistogram &h, uint32 pc) {
	++h.total;
	const uint32 page = pc >> kPcPageShift;
	const uint32 slot = page & (kPcBuckets - 1);
	if (h.hits[slot] == 0 || h.page[slot] == page) {
		h.page[slot] = page;
		++h.hits[slot];
		return;
	}
	// Another page owns the slot: take it over only once this one has been
	// seen more, so a busy page is not evicted by a passing one.
	++h.lost;
	if (--h.hits[slot] == 0)
		h.page[slot] = page;
}

// The end of a session: file its samples by whether it emitted anything.
void foldPcSession() {
	if (s_pcSessionCount == 0)
		return;
	PcHistogram &h = s_pcHist[s_produceEvents ? 1 : 0];
	++h.sessions;
	for (uint32 i = 0; i < s_pcSessionCount; ++i)
		histogramAdd(h, s_pcSession[i]);
	s_pcSessionCount = 0;
}

inline uint32 queued() {
	return (uint32)((s_tail - s_head + kQueueSize) % kQueueSize);
}

// The producer's queue push and buffer scan against the extension a nested
// tick may take: both run in supervisor mode, so the level can be raised.
typedef AtariInterruptsOff InterruptsOff;

inline bool txde() { return (*kHostIsr & 2) != 0; }
inline bool rxdf() { return (*kHostIsr & 1) != 0; }

inline uint32 readReply() {
	uint32 value = kHostDataBytes[1];
	value = (value << 8) | kHostDataBytes[2];
	value = (value << 8) | kHostDataBytes[3];
	return value;
}

// One delivery step: announce the head period, or blast it once the
// kernel has parked its receiver, or take its acknowledgement.
void deliverStep() {
	if (s_head == s_tail)
		return;
	AtariDspAudio::Period *period = s_queue[s_head];
	switch (s_pollState) {
	case 0:
		if (txde()) {
			*kHostData = kCmdRefill;
			s_pollState = 1;
		}
		break;
	case 1:
		if (rxdf()) {
			if (readReply() != kReplyReady) {
				++s_protocolErrors;
				s_pollState = 0;
				break;
			}
			const uint32 pcmWords = period->words[1 + 2 * period->eventCount] ? 1 + AtariDspAudio::kPcmPerPeriod : 1;
			const uint32 count = 1 + 2 * period->eventCount + pcmWords;
			const uint32 *word = period->words;
			for (uint32 i = 0; i < count; ++i) {
				while (!txde())
					;
				*kHostData = *word++;
			}
			s_pollState = 2;
		}
		break;
	default:
		if (rxdf()) {
			s_status = readReply();
			period->inFlight = false;
			s_head = (s_head + 1) % kQueueSize;
			s_pollState = 0;
		}
		break;
	}
}

// A blocking command exchange over the raw port, for the stream loop's
// stop command once delivery has been quiesced.
long exchangeSuper() {
	while (!txde())
		;
	*kHostData = s_command;
	while (!rxdf())
		;
	s_reply = readReply();
	return 0;
}

} // namespace

// Outside the anonymous namespace above: dsp-opl.cpp sets these around the
// OPL timer callbacks, and the tick samples them.
volatile uint8 g_atariDspInCallback;
volatile uint32 g_atariDspCallbacks;

// MFP Timer A, about 1 kHz, entered at interrupt level 6 in supervisor
// mode. The assembly below has already moved to the handler's own stack
// (unless this tick is nested inside a production call, which is on that
// stack already), so the interrupted stack, which may be TOS's small one
// inside a BIOS or XBIOS call, carries only the exception frame and the
// saved registers. Every tick takes one delivery step; a tick that finds
// production due prepares for it here and leaves the rest to the assembly:
// the FPU state is the interrupted context's, and the interrupt level must
// drop so that delivery and the system keep running while the producer
// works.
//
// The in-service bit is cleared first so that a tick nested inside
// production is delivered at all.
long atari_dsp_timer_tick(unsigned long interruptedLevel, unsigned long interruptedPc) {
	*((volatile uint8 *)0xFFFFFA0FL) = (uint8)~(1 << 5);
	if (s_deliver)
		deliverStep();
	// Production only from the program's own level (TOS runs programs at
	// interrupt level 3, so that the level-2 HBL stays masked; anything
	// above that is another handler), outside every critical section of
	// the main loop, and never nested.
	if (!s_producer)
		return 0;
	if (queued() == 0) {
		++s_emptyTicks;
		if (++s_emptyStreak > s_emptyStreakMax)
			s_emptyStreakMax = s_emptyStreak;
	} else {
		s_emptyStreak = 0;
	}
	if (s_producing) {
		// A long production call (iMUSE can spend hundreds of milliseconds
		// in one callback): extension periods keep the kernel fed meanwhile.
		++s_phaseTicks[g_atariDspInCallback & 1];
		if (s_produceTicks >= kPcSampleAfterMs)
			samplePc((uint32)interruptedPc);
		if (++s_produceTicks > s_produceTicksMax) {
			s_produceTicksMax = s_produceTicks;
			s_produceLoopsAtMax = s_produceLoops;
			s_produceEventsAtMax = s_produceEvents;
			s_callbackTicksAtMax = s_phaseTicks[1];
			s_callbacksAtMax = g_atariDspCallbacks;
		}
		if (queued() < AtariDspAudio::kExtendBelow && s_producer(s_producerContext, false))
			++s_extended;
		return 0;
	}
	if (queued() >= AtariDspAudio::kProduceAhead) {
		s_refusedStreak = 0;
		return 0;
	}
	if (interruptedLevel > 0x0300 || g_atariCriticalDepth != 0 || g_atariAllocatorDepth != 0) {
		if (interruptedLevel > 0x0300)
			++s_refusedLevel;
		else if (g_atariCriticalDepth != 0)
			++s_refusedMutex;
		else
			++s_refusedAllocator;
		if (++s_refusedStreak > s_refusedStreakMax)
			s_refusedStreakMax = s_refusedStreak;
		// Refused for long enough that the queue is nearly empty: an
		// extension period keeps the kernel fed. It touches nothing the
		// main loop guards, so it is fine here, at level 6 on the
		// interrupted stack.
		if (queued() < AtariDspAudio::kExtendBelow && s_producer(s_producerContext, false))
			++s_extended;
		return 0;
	}
	s_refusedStreak = 0;
	s_produceTicks = 0;
	s_produceLoops = s_produceEvents = 0;
	s_phaseTicks[0] = s_phaseTicks[1] = 0;
	g_atariDspCallbacks = 0;
	s_pcSessionCount = 0;
	s_producing = true;
	return 1;
}

// On the production stack, at interrupt level 3. The assembly clears the
// producing flag itself, once the level is back at 6: a tick between the
// clear and the level change would take this stack for its own.
void atari_dsp_produce() {
	while (s_producer && queued() < AtariDspAudio::kProduceAhead) {
		++s_produceLoops;
		if (!s_producer(s_producerContext, true))
			break;
	}
	InterruptsOff off;
	foldPcSession();
}

// The vector: save the integer registers, and unless nested inside a
// production call move to the handler's own stack; ask the tick whether to
// produce, and if so save the FPU state, lower the level to 3 for the
// producer, and undo it all. The exception frame's SR sits above the
// fifteen saved registers on the interrupted stack, which is all that
// stack ever carries. A nested tick stays on the production stack it was
// interrupted on, and the tick refuses production while nested.
asm(
"	.text\n"
"	.align	2\n"
"	.globl	atari_dsp_timer_a\n"
"atari_dsp_timer_a:\n"
"	movem.l	%d0-%d7/%a0-%a6,-(%sp)\n"
"	tst.b	atari_dsp_producing\n"
"	bne	2f\n"
"	move.l	%sp,atari_dsp_saved_stack\n"
"	move.l	atari_dsp_stack_top,%sp\n"
"	move.l	atari_dsp_saved_stack,%a0\n"
"	move.l	62(%a0),%d1\n"
"	moveq	#0,%d0\n"
"	move.w	60(%a0),%d0\n"
"	and.l	#0x0700,%d0\n"
"	move.l	%d1,-(%sp)\n"
"	move.l	%d0,-(%sp)\n"
"	jsr	atari_dsp_timer_tick\n"
"	addq.l	#8,%sp\n"
"	tst.l	%d0\n"
"	beq	1f\n"
"	fsave	-(%sp)\n"
"	fmovem.x	%fp0-%fp7,-(%sp)\n"
"	fmove.l	%fpcr,-(%sp)\n"
"	fmove.l	%fpsr,-(%sp)\n"
"	fmove.l	%fpiar,-(%sp)\n"
"	move.w	#0x2300,%sr\n"
"	jsr	atari_dsp_produce\n"
"	move.w	#0x2600,%sr\n"
"	clr.b	atari_dsp_producing\n"
"	fmove.l	(%sp)+,%fpiar\n"
"	fmove.l	(%sp)+,%fpsr\n"
"	fmove.l	(%sp)+,%fpcr\n"
"	fmovem.x	(%sp)+,%fp0-%fp7\n"
"	frestore	(%sp)+\n"
"1:\n"
"	move.l	atari_dsp_saved_stack,%sp\n"
"	movem.l	(%sp)+,%d0-%d7/%a0-%a6\n"
"	rte\n"
"2:\n"
"	move.l	62(%sp),%d1\n"
"	moveq	#0,%d0\n"
"	move.w	60(%sp),%d0\n"
"	and.l	#0x0700,%d0\n"
"	move.l	%d1,-(%sp)\n"
"	move.l	%d0,-(%sp)\n"
"	jsr	atari_dsp_timer_tick\n"
"	addq.l	#8,%sp\n"
"	movem.l	(%sp)+,%d0-%d7/%a0-%a6\n"
"	rte\n"
);

AtariDspAudio::AtariDspAudio()
	: _filling(-1), _booted(false), _streaming(false), _submitted(0), _rendered(0), _late(0) {
	memset(_periods, 0, sizeof(_periods));
}

AtariDspAudio::~AtariDspAudio() {
	shutdown();
}

bool AtariDspAudio::exchange(uint32 command, uint32 &reply) {
	long tx = (long)command;
	long rx = 0;
	Dsp_BlkUnpacked(&tx, 1, &rx, 1);
	reply = (uint32)rx & 0xffffff;
	return true;
}

// Every word is acknowledged by the kernel, because TOS's block transfer
// handshakes only its first word.
bool AtariDspAudio::uploadWords(bool ySpace, uint32 address, const uint32 *words, uint32 count) {
	uint32 reply;
	exchange(ySpace ? kCmdWriteY : kCmdWriteX, reply);
	exchange(address, reply);
	exchange(count, reply);
	for (uint32 i = 0; i < count; ++i)
		exchange(words[i] & 0xffffff, reply);
	return true;
}

bool AtariDspAudio::uploadTables() {
	namespace P = OplPractical;
	static uint32 buffer[4096];

	const uint32 scalars[4] = { 4, 0, 9, 0x7fffff };
	uploadWords(false, P::SC_TREMOLO_SHIFT, scalars, 4);

	for (int i = 0; i < 512; ++i)
		buffer[i] = kOplGain[i];
	uploadWords(false, P::kGainTable, buffer, 512);

	P::Chip *chip = new P::Chip;
	P::reset(chip, 9);
	for (int i = 0; i < P::kSlots; ++i)
		for (int w = 0; w < P::kOpStride; ++w)
			buffer[i * P::kOpStride + w] = (uint32)chip->op[i].w[w] & 0xffffff;
	delete chip;
	uploadWords(false, P::kOpBase, buffer, P::kSlots * P::kOpStride);
	memset(buffer, 0, sizeof(buffer));
	uploadWords(true, P::kOpBase, buffer, P::kSlots * P::kOpStride);
	uploadWords(false, P::kChannelBase, buffer, P::kChannels * P::kChannelStride);

	for (int i = 0; i < 64; ++i)
		buffer[i] = kOplAttackBlock[i];
	uploadWords(false, P::kAttackTable, buffer, 64);
	for (int i = 0; i < 64; ++i)
		buffer[i] = kOplDecayBlock[i];
	uploadWords(false, P::kDecayTable, buffer, 64);
	for (int i = 0; i < 8; ++i)
		buffer[i] = (uint32)P::kVibratoOffset[i];
	uploadWords(false, P::kVibratoTable, buffer, 8);

	for (int wf = 0; wf < P::kWaveforms; ++wf)
		for (int phase = 0; phase < 1024; ++phase)
			buffer[wf * 1024 + phase] = (uint32)P::waveSample((uint8)wf, (uint16)phase) & 0xffffff;
	uploadWords(true, P::kWaveBase, buffer, P::kWaveforms * 1024);
	return true;
}

bool AtariDspAudio::boot() {
	if (_booted)
		return true;
	if (Dsp_Lock() != 0) {
		warning("AtariDspAudio: the DSP is locked by another program");
		return false;
	}
	if (Dsp_Reserve(16, 16) < 0) {
		warning("AtariDspAudio: Dsp_Reserve failed");
		Dsp_Unlock();
		return false;
	}
	// XBIOS boots at most 512 internal words: the loader, which then streams
	// the sparse program and acknowledges once.
	Dsp_ExecBoot((char *)kAtariDspOplBoot, ATARI_DSP_OPL_BOOT_WORDS, kDspAbility);
	long reply = 0;
	Dsp_BlkUnpacked((long *)kAtariDspOplStream, ATARI_DSP_OPL_STREAM_WORDS, &reply, 1);
	if ((reply & 0xffffff) != ATARI_DSP_OPL_STREAM_REPLY_OK) {
		warning("AtariDspAudio: the kernel loader answered %06lx", reply & 0xffffff);
		Dsp_Unlock();
		return false;
	}
	uint32 ping;
	exchange(kCmdPing, ping);
	if (ping != kReplyPing) {
		warning("AtariDspAudio: the kernel answered %06x to a ping", ping);
		Dsp_Unlock();
		return false;
	}
	uploadTables();
	_booted = true;
	debug("AtariDspAudio: kernel booted, %d program words", ATARI_DSP_OPL_STREAM_WORDS);
	return true;
}

bool AtariDspAudio::startStream() {
	if (!_booted || _streaming)
		return _streaming;
	if (Locksnd() != 1) {
		warning("AtariDspAudio: Locksnd failed");
		return false;
	}
	// The matrix state outlives whatever set it: pin every route.
	Buffoper(0);
	Sndstatus(SND_RESET);
	Soundcmd(ADDERIN, MATIN);
	Setmode(STEREO16);
	Settracks(0, 0);
	Setmontracks(0);
	Dsptristate(1, 0);
	Devconnect(DSPXMIT, DAC, CLK25M, CLK50K, NO_SHAKE);

	uint32 reply;
	exchange(kCmdStreamStart, reply);
	s_head = s_tail = 0;
	s_pollState = 0;
	s_status = 0;
	s_protocolErrors = 0;
	_filling = -1;
	for (int i = 0; i < kPeriods; ++i)
		_periods[i].inFlight = false;
	s_producer = nullptr;
	s_producing = false;
	s_refusedStreak = s_refusedStreakMax = s_refusedMutex = s_refusedAllocator = s_refusedLevel = s_extended = 0;
	s_produceTicks = s_produceTicksMax = s_emptyTicks = s_emptyStreak = s_emptyStreakMax = 0;
	s_produceLoops = s_produceEvents = s_produceLoopsAtMax = s_produceEventsAtMax = 0;
	s_phaseTicks[0] = s_phaseTicks[1] = s_callbackTicksAtMax = s_callbacksAtMax = 0;
	memset(s_pcHist, 0, sizeof(s_pcHist));
	s_pcSessionCount = 0;
	g_atariDspInCallback = 0;
	g_atariDspCallbacks = 0;
	atari_dsp_stack_top = s_productionStack + kProductionStackBytes;
	// Timer A: 2,457,600 Hz / 64 / 38 = 1,010 Hz.
	s_deliver = true;
	Xbtimer(XB_TIMERA, 5, 38, atari_dsp_timer_a);
	Jenabint(MFP_TIMERA);
	_streaming = true;
	return true;
}

void AtariDspAudio::setProducer(Producer producer, void *context) {
	s_producer = nullptr;
	s_producerContext = context;
	s_producer = producer;
}

uint32 AtariDspAudio::periodsQueued() const {
	return queued();
}

void AtariDspAudio::productionStats(uint32 &refusedStreakMax, uint32 &refusedMutex, uint32 &refusedAllocator,
                                    uint32 &refusedLevel, uint32 &extended, uint32 &produceMax,
                                    uint32 &produceLoopsAtMax, uint32 &produceEventsAtMax,
                                    uint32 &callbackTicksAtMax, uint32 &callbacksAtMax,
                                    uint32 &emptyTicks, uint32 &emptyStreakMax) const {
	refusedStreakMax = s_refusedStreakMax;
	refusedMutex = s_refusedMutex;
	refusedAllocator = s_refusedAllocator;
	refusedLevel = s_refusedLevel;
	extended = s_extended;
	produceMax = s_produceTicksMax;
	produceLoopsAtMax = s_produceLoopsAtMax;
	produceEventsAtMax = s_produceEventsAtMax;
	callbackTicksAtMax = s_callbackTicksAtMax;
	callbacksAtMax = s_callbacksAtMax;
	emptyTicks = s_emptyTicks;
	emptyStreakMax = s_emptyStreakMax;
}

// The stream loop answers the status query; the command loop does not, so
// the counters are read before the stop and cached.
// Every acknowledgement refreshes the counters, so they trail the kernel
// by at most one period.
bool AtariDspAudio::queryCounters(uint32 &rendered, uint32 &late) {
	rendered = _rendered;
	late = _late;
	return true;
}

void AtariDspAudio::stopStream() {
	if (!_streaming)
		return;
	// Stop production, let the handler drain the queue so the kernel is
	// back in its stream loop, then take the port back.
	s_producer = nullptr;
	for (int i = 0; i < 1000000 && (s_head != s_tail || s_pollState != 0); ++i)
		;
	s_deliver = false;
	Jdisint(MFP_TIMERA);
	s_command = kCmdStreamStop;
	Supexec(exchangeSuper);
	s_command = 0;
	Unlocksnd();
	_streaming = false;
}

void AtariDspAudio::shutdown() {
	if (_streaming)
		stopStream();
	if (_booted) {
		Dsp_Unlock();
		_booted = false;
	}
}

AtariDspAudio::Period *AtariDspAudio::beginPeriod(bool extension) {
	if (!extension && _filling >= 0)
		return &_periods[_filling];
	InterruptsOff off;
	for (int i = 0; i < kPeriods; ++i) {
		if (!_periods[i].inFlight && i != _filling) {
			if (!extension)
				_filling = i;
			_periods[i].inFlight = true;   // claimed; submit() queues it
			_periods[i].eventCount = 0;
			_periods[i].words[0] = 0;
			return &_periods[i];
		}
	}
	return nullptr;
}

void AtariDspAudio::addEvent(Period *period, uint32 block, uint16 address, uint32 value) {
	++s_produceEvents;
	if (period->eventCount >= kMaxEvents) {
		++s_protocolErrors;
		return;
	}
	period->words[1 + 2 * period->eventCount] = ((block & 0xff) << 16) | address;
	period->words[2 + 2 * period->eventCount] = value & 0xffffff;
	++period->eventCount;
}

void AtariDspAudio::setPcm(Period *period, const int16 *samples) {
	uint32 *word = &period->words[1 + 2 * period->eventCount];
	if (!samples) {
		*word = 0;
		return;
	}
	bool silent = true;
	for (int i = 0; i < kPcmPerPeriod; ++i)
		if (samples[i])
			silent = false;
	if (silent) {
		*word = 0;
		return;
	}
	*word++ = 1;
	// 16 bits into bits 7-22: the kernel doubles the sum on output.
	for (int i = 0; i < kPcmPerPeriod; ++i)
		*word++ = ((uint32)(int32)samples[i] << 7) & 0xffffff;
}

void AtariDspAudio::submit(Period *period) {
	period->words[0] = period->eventCount;
	InterruptsOff off;
	s_queue[s_tail] = period;
	s_tail = (s_tail + 1) % kQueueSize;
	if (period == &_periods[_filling])
		_filling = -1;
	++_submitted;
}

void AtariDspAudio::poll() {
	if (!_streaming)
		return;
	const uint32 status = s_status;
	_rendered = status & 0xfff;
	_late = status >> 12;
}

uint32 AtariDspAudio::protocolErrors() const {
	return s_protocolErrors;
}

// DIAGNOSTIC: the sampled PCs, commonest first, with a reference symbol so
// they can be resolved against the unstripped binary offline.
uint32 AtariDspAudio::pcSamples(int silent, uint32 *addr, uint32 *hits, uint32 count, uint32 &lost,
                                uint32 &sessions, uint32 &reference) const {
	const PcHistogram &h = s_pcHist[silent ? 0 : 1];
	reference = (uint32)&atari_dsp_timer_tick;
	lost = h.lost;
	sessions = h.sessions;
	uint32 taken = 0;
	for (uint32 slot = 0; slot < count; ++slot) {
		int best = -1;
		for (int i = 0; i < kPcBuckets; ++i) {
			if (h.hits[i] == 0)
				continue;
			bool already = false;
			for (uint32 j = 0; j < taken; ++j)
				if (addr[j] == (h.page[i] << kPcPageShift))
					already = true;
			if (already)
				continue;
			if (best < 0 || h.hits[i] > h.hits[best])
				best = i;
		}
		if (best < 0)
			break;
		addr[taken] = h.page[best] << kPcPageShift;
		hits[taken] = h.hits[best];
		++taken;
	}
	return h.total;
}

#endif
