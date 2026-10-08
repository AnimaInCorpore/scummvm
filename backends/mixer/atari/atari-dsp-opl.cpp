/* ScummVM - Graphic Adventure Engine
 * Copyright (C) 2026 Sascha Springer
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DSP kernel and decoder adapted from F030OPL2, commit 6beb75a.
 */

#define FORCE_TEXT_CONSOLE
#include "backends/mixer/atari/atari-dsp-opl.h"
#include "backends/mixer/atari/atari-mixer.h"
#include "backends/mixer/atari/opl/dsp-image.h"
#include "backends/mixer/atari/opl/opl-upload.h"
#include "backends/mixer/atari/opl/stream.h"
#include "backends/mixer/atari/opl/queue.h"
#include "audio/mixer_intern.h"
#include "common/debug.h"
#include "common/system.h"
#include "common/textconsole.h"

#include <mint/cookie.h>
#include <mint/falcon.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>

namespace OPL {
namespace FalconDsp {

namespace {
enum {
	kPing = 0x010000, kWriteX = 0x020000, kWriteY = 0x030000,
	kStart = 0x090000, kRefill = 0x0a0000, kStop = 0x0b0000,
	kReplyPing = 0x4f5052, kReplyReady = 0x524459,
	kReplyTimeoutMs = 250
};

volatile byte *const kHostStatus = (volatile byte *)0xffffa202;
volatile uint32 *const kHostData = (volatile uint32 *)0xffffa204;

// Only the short port transactions execute on TOS's supervisor stack.
bool s_portFailed;

bool waitPort(byte mask) {
	const uint32 deadline = *(volatile uint32 *)0x4ba + 50; // 200 Hz system clock
	while (!(*kHostStatus & mask)) {
		if ((int32)(*(volatile uint32 *)0x4ba - deadline) >= 0) {
			s_portFailed = true;
			return false;
		}
	}
	return true;
}

bool sendWord(uint32 word) {
	if (s_portFailed || !waitPort(2))
		return false;
	*kHostData = word;
	return true;
}

uint32 receiveWord() {
	if (s_portFailed || !waitPort(1))
		return 0;
	volatile byte *data = (volatile byte *)kHostData;
	uint32 word = (uint32)data[1] << 16;
	word |= (uint32)data[2] << 8;
	word |= data[3]; // RXDF clears when the last byte is read
	return word;
}

uint32 s_command, s_reply;
long commandSuper() {
	sendWord(s_command);
	s_reply = receiveWord();
	return 0;
}
uint32 command(uint32 word) {
	s_command = word;
	Supexec(commandSuper);
	return s_reply;
}

long bootSuper() {
	for (uint i = 0; i < ATARI_DSP_OPL_STREAM_WORDS && !s_portFailed; ++i)
		sendWord(kAtariDspOplStream[i]);
	s_reply = receiveWord();
	return 0;
}

int s_space;
uint16 s_address;
const uint32 *s_words;
uint s_count;
long uploadSuper() {
	s_command = s_space ? kWriteY : kWriteX;
	commandSuper();
	s_command = s_address;
	commandSuper();
	s_command = s_count;
	commandSuper();
	for (uint i = 0; i < s_count && !s_portFailed; ++i) {
		s_command = s_words[i] & 0xffffff;
		commandSuper();
	}
	return 0;
}
struct Uploader {
	void block(int space, uint16 address, const uint32 *words, unsigned count) {
		s_space = space;
		s_address = address;
		s_words = words;
		s_count = count;
		Supexec(uploadSuper);
	}
};

PeriodQueue s_periods;
volatile uint s_deliveryState;
volatile uint32 s_ticks, s_progressTick, s_delivered, s_extensions, s_late;
volatile uint32 s_speechPeriods, s_pcmGaps;
volatile bool s_deliveryFailed, s_paused;
bool s_lastAudible;
const PeriodQueue::Period *s_activePeriod;
const uint32 *s_payload;
uint s_payloadCount, s_payloadPosition;
bool s_isExtension;
uint32 s_extension[5];

// Integer-only delivery. No mixer, iMUSE, allocation, file access or OS calls.
// A bounded port poll cannot hang the machine with interrupts masked.
void deliveryTick() {
	++s_ticks;
	if (s_deliveryFailed)
		return;
	uint spinBudget = 1024, wordBudget = PeriodQueue::kWords;
	for (uint transition = 0; transition < 4; ++transition) {
		switch (s_deliveryState) {
		case 0: {
			if (!(*kHostStatus & 2))
				return;
			const PeriodQueue::Period *period = s_periods.front();
			s_activePeriod = period;
			s_isExtension = !period;
			if (period) {
				s_payload = period->words;
				s_payloadCount = period->count;
			} else {
				// Keep oscillators running, with silence for unavailable PCM.
				s_extension[0] = 1;
				s_extension[1] = OplPractical::SC_PAUSED;
				s_extension[2] = s_paused;
				s_extension[3] = 0;
				s_payload = s_extension;
				s_payloadCount = 4;
			}
			s_payloadPosition = 0;
			*kHostData = kRefill;
			s_deliveryState = 1;
			s_progressTick = s_ticks;
			break;
		}
		case 1:
			if (!(*kHostStatus & 1))
				return;
			if (receiveWord() != kReplyReady) {
				s_deliveryFailed = true;
				return;
			}
			s_deliveryState = 2;
			break;
		case 2:
			while (s_payloadPosition < s_payloadCount && wordBudget) {
				while (!(*kHostStatus & 2)) {
					if (!--spinBudget)
						return;
				}
				*kHostData = s_payload[s_payloadPosition++];
				--wordBudget;
				s_progressTick = s_ticks;
			}
			if (s_payloadPosition != s_payloadCount)
				return;
			s_deliveryState = 3;
			break;
		case 3:
			if (!(*kHostStatus & 1))
				return;
			s_late = receiveWord() >> 12;
			if (s_isExtension) {
				++s_extensions;
				if (s_lastAudible)
					++s_pcmGaps;
				s_lastAudible = false;
			} else {
				s_speechPeriods += s_activePeriod->speech ? 1 : 0;
				s_lastAudible = s_activePeriod->audible;
				s_periods.release();
				++s_delivered;
			}
			s_deliveryState = 0;
			s_progressTick = s_ticks;
			break;
		}
	}
}

extern "C" void falcon_opl_timer_a();
extern "C" void falcon_opl_delivery_tick() {
	*((volatile byte *)0xfffffa0f) = (byte)~(1 << 5);
	deliveryTick();
}

long stopSuper() {
	// Timer A has been disabled. Finish only the in-flight exchange.
	if (s_deliveryState == 1) {
		if (receiveWord() != kReplyReady)
			s_portFailed = true;
		s_deliveryState = 2;
	}
	if (s_deliveryState == 2) {
		while (s_payloadPosition < s_payloadCount && !s_portFailed)
			sendWord(s_payload[s_payloadPosition++]);
		s_deliveryState = 3;
	}
	if (s_deliveryState == 3)
		receiveWord();
	if (!s_portFailed) {
		sendWord(kStop);
		receiveWord();
	}
	return 0;
}

// Snapshot the routing and format registers not covered by Soundcmd queries.
uint16 s_soundRegisters[5];
const uint32 kSoundAddresses[] = {0xffff8920, 0xffff8930, 0xffff8932, 0xffff8934, 0xffff8936};
long saveSoundSuper() {
	for (uint i = 0; i < ARRAYSIZE(kSoundAddresses); ++i)
		s_soundRegisters[i] = *(volatile uint16 *)kSoundAddresses[i];
	return 0;
}
long restoreSoundSuper() {
	for (uint i = 0; i < ARRAYSIZE(kSoundAddresses); ++i)
		*(volatile uint16 *)kSoundAddresses[i] = s_soundRegisters[i];
	return 0;
}
}

struct OPL::State {
	OplPractical::Decoder decoder;
	EventBuffer events;
	PcmClock pcmClock;
	int32 mixBuffer[256 * 2];
	int16 pcm[kPcmFrames];
	int32 firstSample, secondSample;
	uint mixPosition, mixFrames;
	uint frame;
	uint32 periods, nextReport;
	int oldAdder;
	bool stereo, primed, dspLocked, soundLocked, opened, updating;

	State() : firstSample(0), secondSample(0), mixPosition(0), mixFrames(0), frame(0),
		periods(0), nextReport(512), oldAdder(0), stereo(false),
		primed(false), dspLocked(false), soundLocked(false), opened(false),
		updating(false) {}

	int32 nextSample(Audio::MixerImpl *mixer) {
		if (mixPosition == mixFrames) {
			memset(mixBuffer, 0, sizeof(mixBuffer));
			mixer->mixCallback((byte *)mixBuffer, 256 * (stereo ? 2 : 1) * sizeof(int32));
			mixPosition = 0;
			mixFrames = 256;
		}
		int64 sample = mixBuffer[mixPosition * (stereo ? 2 : 1)];
		if (stereo)
			sample = (sample + mixBuffer[mixPosition * 2 + 1]) / 2;
		++mixPosition;
		return sample < -32768 ? -32768 : sample > 32767 ? 32767 : (int32)sample;
	}

	void mix(Audio::MixerImpl *mixer, bool suspended) {
		bool active = false;
		for (int type = Audio::Mixer::kPlainSoundType; type <= Audio::Mixer::kSpeechSoundType; ++type)
			active |= mixer->hasActiveChannelOfType((Audio::Mixer::SoundType)type);
		// A mixer callback can remove a finished channel while its last decoded
		// samples still reside in our cache. Preserve that speech tail.
		if (!active && primed) {
			active = firstSample != 0 || secondSample != 0;
			for (uint i = mixPosition * (stereo ? 2 : 1); !active && i < mixFrames * (stereo ? 2 : 1); ++i)
				active |= mixBuffer[i] != 0;
		}
		if (suspended || !active) {
			memset(pcm, 0, sizeof(pcm));
			primed = false;
			mixPosition = mixFrames = 0;
			return;
		}
		if (!primed) {
			firstSample = nextSample(mixer);
			secondSample = nextSample(mixer);
			primed = true;
		}
		for (uint i = 0; i < kPcmFrames; ++i) {
			pcm[i] = PcmClock::interpolate(firstSample, secondSample, pcmClock.fraction());
			uint consumed = pcmClock.advance();
			while (consumed--) {
				firstSample = secondSample;
				secondSample = nextSample(mixer);
			}
		}
	}
};

OPL::OPL(AtariMixerManager *mixer) : _state(new State()), _mixer(mixer), _address(0),
	_callbackFrequency(0), _callbackPhase(0), _callbacksRunning(false), _suspended(false) {}

OPL::~OPL() {
	stop();
	_mixer->stopDspOpl(this);
	close();
	delete _state;
}

bool OPL::init() {
	if (_state->opened)
		return true;
	long machine = 0;
	if (Getcookie(C__MCH, &machine) != C_FOUND || (machine >> 16) != 3) {
		warning("Falcon DSP OPL requires Falcon hardware");
		return false;
	}
	return _mixer->startDspOpl(this);
}

bool OPL::open(int inputRate, bool stereo) {
	debug(1, "Falcon DSP OPL: booting, PCM mixer %d Hz", inputRate);
	_state->events.clear();
	_state->frame = _state->mixPosition = _state->mixFrames = 0;
	_state->primed = _state->updating = false;
	_state->periods = 0;
	_state->nextReport = 512;
	_state->stereo = stereo;
	_state->pcmClock.reset(inputRate);
	s_portFailed = false;
	if (Dsp_Lock() != 0)
		return false;
	_state->dspLocked = true;
	if (Dsp_Reserve(16, 16) < 0 || Locksnd() != 1) {
		close();
		return false;
	}
	_state->soundLocked = true;
	_state->oldAdder = Soundcmd(ADDERIN, -1);
	Supexec(saveSoundSuper);
	Dsp_ExecBoot(kAtariDspOplBoot, ATARI_DSP_OPL_BOOT_WORDS, 3);
	Supexec(bootSuper);
	if (s_portFailed || s_reply != ATARI_DSP_OPL_STREAM_REPLY_OK || command(kPing) != kReplyPing) {
		close();
		return false;
	}
	Uploader uploader;
	FalconOplTables::uploadTables(uploader, 9, false);
	const uint32 ahead = 1;
	uploader.block(0, OplPractical::SC_RENDER_AHEAD, &ahead, 1);
	if (s_portFailed) {
		close();
		return false;
	}
	Buffoper(0);
	Setmode(STEREO16);
	Settracks(0, 0);
	Setmontracks(0);
	Soundcmd(ADDERIN, MATIN);
	Dsptristate(1, 0);
	Devconnect(DSPXMIT, DAC, CLK25M, CLK50K, NO_SHAKE);
	command(kStart);
	if (s_portFailed) {
		close();
		return false;
	}
	_state->opened = true;
	reset();
	s_periods.clear();
	s_deliveryState = 0;
	s_ticks = s_progressTick = s_delivered = s_extensions = s_late = 0;
	s_speechPeriods = s_pcmGaps = 0;
	s_lastAudible = false;
	s_deliveryFailed = s_paused = false;
	// 2,457,600 / 64 / 38 = 1,010 Hz, independent of game-loop stalls.
	Xbtimer(XB_TIMERA, 5, 38, falcon_opl_timer_a);
	Jenabint(MFP_TIMERA);
	debug(1, "Falcon DSP OPL: Timer A delivery started (render-ahead enabled)");
	return true;
}

void OPL::close(bool drain) {
	if (_state->opened)
		Jdisint(MFP_TIMERA);
	if (drain && _state->opened && !s_portFailed) {
		// Complete any outstanding protocol exchange before issuing STOP.
		// Shutdown is the only path allowed to wait for a period.
		Supexec(stopSuper);
	}
	if (_state->soundLocked) {
		Buffoper(0);
		Dsptristate(0, 0);
		Supexec(restoreSoundSuper);
		Soundcmd(ADDERIN, _state->oldAdder);
		Unlocksnd();
		_state->soundLocked = false;
	}
	if (_state->dspLocked) {
		Dsp_Unlock();
		_state->dspLocked = false;
	}
	if (drain && _state->opened)
		debug("Falcon DSP OPL: %lu delivered, %lu extensions, %lu late, %lu speech, %lu PCM gaps",
			(unsigned long)s_delivered, (unsigned long)s_extensions, (unsigned long)s_late,
			(unsigned long)s_speechPeriods, (unsigned long)s_pcmGaps);
	_state->opened = false;
}

void OPL::reset() {
	_state->decoder.reset(&_state->events, 9, _state->frame);
	_address = 0;
}

void OPL::write(int port, int value) {
	if (port & 1)
		writeReg(_address, value);
	else
		_address = value & 0xff;
}

void OPL::writeReg(int reg, int value) {
	if (reg >= 0 && reg < 256 && _state->opened)
		_state->decoder.write(_state->frame, reg, value & 0xff);
}

void OPL::setCallbackFrequency(int frequency) {
	assert(frequency > 0 && frequency <= 1000);
	_callbackFrequency = frequency;
}

void OPL::startCallbacks(int frequency) {
	setCallbackFrequency(frequency);
	_callbackPhase = kCodecClock;
	_callbacksRunning = true;
}

void OPL::stopCallbacks() {
	_callbacksRunning = false;
	if (_state->opened)
		reset();
}

void OPL::suspend(bool suspended) {
	_suspended = suspended;
	s_paused = suspended;
	if (_state->opened)
		_state->events.write(_state->frame, OplPractical::SC_PAUSED, suspended ? 1 : 0);
}

bool OPL::update(Audio::MixerImpl *mixer) {
	if (!_state->opened || _state->updating)
		return _state->opened;
	// Read progress first: an interrupt between two loads must not make
	// progress appear to be in the future and wrap the unsigned timeout.
	const uint32 progress = s_progressTick;
	const uint32 now = s_ticks;
	if (s_deliveryFailed || now - progress > 250)
		return false;
	_state->updating = true;
	for (uint produced = 0; produced < PeriodQueue::kAhead && s_periods.size() < PeriodQueue::kAhead; ++produced) {
		PeriodQueue::Period *period = s_periods.begin();
		if (!period)
			break;
		// Jump between iMUSE ticks rather than walking all 768 codec frames.
		uint frame = 0;
		while (_callbacksRunning && !_suspended && frame < kPeriodFrames) {
			_state->frame = frame;
			while (_callbacksRunning && _callbackPhase >= kCodecClock) {
				_callbackPhase -= kCodecClock;
				if (_callback && _callback->isValid())
					(*_callback)();
			}
			const uint step = _callbackFrequency * kCodecDivider;
			const uint untilTick = (kCodecClock - _callbackPhase + step - 1) / step;
			const uint advance = MIN<uint>(untilTick, kPeriodFrames - frame);
			_callbackPhase += advance * step;
			frame += advance;
		}
		_state->decoder.flush();
		period->speech = !_suspended && mixer->hasActiveChannelOfType(Audio::Mixer::kSpeechSoundType);
		_state->mix(mixer, _suspended);
		if (_state->events.overflowed()) {
			_state->updating = false;
			warning("Falcon DSP OPL parameter queue overflow");
			return false;
		}
		uint words = 0;
		period->words[words++] = _state->events.count();
		for (uint i = 0; i < _state->events.count(); ++i) {
			period->words[words++] = _state->events.data()[i].address;
			period->words[words++] = _state->events.data()[i].value;
		}
		period->words[words++] = 1;
		period->audible = false;
		for (uint i = 0; i < kPcmFrames; ++i) {
			period->words[words++] = ((uint32)(int32)_state->pcm[i] << 7) & 0xffffff;
			period->audible |= _state->pcm[i] != 0;
		}
		period->count = words;
		s_periods.publish();
		_state->events.clear();
		_state->frame = 0;
		++_state->periods;
	}
	_state->updating = false;
	if (s_delivered >= _state->nextReport) {
		_state->nextReport = s_delivered + 512;
		debug(1, "Falcon DSP OPL: %lu delivered, %lu extensions, %lu late, %lu speech, %lu PCM gaps",
			(unsigned long)s_delivered, (unsigned long)s_extensions, (unsigned long)s_late,
			(unsigned long)s_speechPeriods, (unsigned long)s_pcmGaps);
	}
	return !s_deliveryFailed;
}

::OPL::OPL *create(Config::OplType type) {
	if (type != Config::kOpl2 || !AtariMixerManager::instance())
		return nullptr;
	return new OPL(AtariMixerManager::instance());
}

} // namespace FalconDsp
} // namespace OPL
