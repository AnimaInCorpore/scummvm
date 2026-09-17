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

#include "backends/platform/atari/dsp-opl-image.h"
#include "devtools/atari-falcon030/tools/foa-opl3/opl-practical.h"

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

// MFP Timer A, about 1 kHz. The handler runs in supervisor mode, so the
// port is reachable directly; it ends by clearing its in-service bit.
void __attribute__((interrupt)) deliverTimer() {
	if (s_deliver)
		deliverStep();
	*((volatile uint8 *)0xFFFFFA0FL) = (uint8)~(1 << 5);
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

	const uint32 scalars[4] = { 4, 1, 9, 0x7fffff };
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
	for (int i = 0; i < 8; ++i) {
		buffer[i] = (uint32)kOplVibratoDeep[i] & 0xffffff;
		buffer[8 + i] = (uint32)kOplVibratoShallow[i] & 0xffffff;
	}
	uploadWords(false, P::kVibratoTable, buffer, 16);

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
	Devconnect(DSPXMIT, DAC, CLK25M, CLK33K, NO_SHAKE);

	uint32 reply;
	exchange(kCmdStreamStart, reply);
	s_head = s_tail = 0;
	s_pollState = 0;
	s_status = 0;
	s_protocolErrors = 0;
	_filling = -1;
	for (int i = 0; i < kPeriods; ++i)
		_periods[i].inFlight = false;
	// Timer A: 2,457,600 Hz / 64 / 38 = 1,010 Hz.
	s_deliver = true;
	Xbtimer(XB_TIMERA, 5, 38, deliverTimer);
	Jenabint(MFP_TIMERA);
	_streaming = true;
	return true;
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
	// Let the handler drain the queue so the kernel is back in its stream
	// loop, then take the port back.
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

AtariDspAudio::Period *AtariDspAudio::beginPeriod() {
	if (_filling >= 0)
		return &_periods[_filling];
	for (int i = 0; i < kPeriods; ++i) {
		if (!_periods[i].inFlight) {
			_filling = i;
			_periods[i].eventCount = 0;
			_periods[i].words[0] = 0;
			return &_periods[i];
		}
	}
	return nullptr;
}

void AtariDspAudio::addEvent(Period *period, uint32 block, uint16 address, uint32 value) {
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
	period->inFlight = true;
	s_queue[s_tail] = period;
	s_tail = (s_tail + 1) % kQueueSize;
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

#endif
