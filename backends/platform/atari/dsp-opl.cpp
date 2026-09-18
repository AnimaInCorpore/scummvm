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

#include "backends/platform/atari/dsp-opl.h"

#include "backends/platform/atari/atari-critical.h"
#include "common/debug.h"
#include "common/textconsole.h"

AtariDspOPL *AtariDspOPL::s_instance = nullptr;
uint32 AtariDspOPL::s_pending[AtariDspOPL::kPendingMax * 2];
bool AtariDspOPL::s_resync = false;
bool AtariDspOPL::s_resyncMachine = false;
uint32 AtariDspOPL::s_pendingCount = 0;

// The mixer manager sets this when the DSP owns the codec.
AtariDspAudio *g_atariDspAudio = nullptr;

::OPL::OPL *AtariDspOPL::create() {
	if (!g_atariDspAudio || !g_atariDspAudio->isStreaming()) {
		warning("AtariDspOPL: the DSP audio stream is not running");
		return nullptr;
	}
	return new AtariDspOPL(g_atariDspAudio);
}

AtariDspOPL::AtariDspOPL(AtariDspAudio *audio)
	: _audio(audio), _period(nullptr), _decoder(new OplPractical::Decoder), _address(0),
	  _running(false), _framesPerTick16(0), _nextTick16(0), _block(0), _resetting(false) {
	_sink.owner = this;
	// The kernel outlives every OPL and may hold an earlier one's patches:
	// the decoder's reset sends the whole reset state rather than trust it.
	AtariCriticalSection critical;
	s_pendingCount = 0;
	resetDecoder(0);
	s_instance = this;
}

// A reset supersedes every write before it, lost ones included, so it also
// ends a resync that is still due; if its own events are lost, noteLost
// schedules the resync again, with the machine's words.
void AtariDspOPL::resetDecoder(uint32 atBlock) {
	s_resync = s_resyncMachine = false;
	_resetting = true;
	_decoder->reset(&_sink, 9, atBlock);
	_resetting = false;
}

void AtariDspOPL::noteLost() {
	s_resync = true;
	if (_resetting)
		s_resyncMachine = true;
}

AtariDspOPL::~AtariDspOPL() {
	stop();
	{
		// Nothing writes to the kernel after this OPL, so it is left silent;
		// the reset supersedes the driver's closing key-offs still waiting.
		// The pending events outlive the instance and go out with the next
		// period (flushPending).
		AtariCriticalSection critical;
		s_pendingCount = 0;
		resetDecoder(0);
		// Unregistered before the critical section ends: the transport's
		// interrupt looks the instance up before every period it produces.
		s_instance = nullptr;
	}
	delete _decoder;
}

bool AtariDspOPL::init() {
	return true;
}

// The decoder is entered from the main loop (the driver's own writes) and
// from the transport's interrupt (the timer callbacks it runs); the
// critical section keeps the interrupt out while the main loop is inside.
void AtariDspOPL::reset() {
	AtariCriticalSection critical;
	// The kernel is reset with the decoder: clearing the shadow alone would
	// leave the DSP playing and then swallow the zeros a driver writes to
	// silence it, as equal to the fresh shadow. Writes still waiting for a
	// period belong to what the reset ends.
	if (!_period)
		s_pendingCount = 0;
	resetDecoder(_block);
}

void AtariDspOPL::write(int a, int v) {
	AtariCriticalSection critical;
	if (a & 1)
		writeReg(_address, v);
	else
		_address = v & 0xff;
}

void AtariDspOPL::writeReg(int r, int v) {
	AtariCriticalSection critical;
	_decoder->write(_block, (uint16)(r & 0x1ff), (uint8)v);
}

void AtariDspOPL::EventSink::write(uint32 block, uint16 address, int32 value) {
	owner->emit(block, address, value);
}

// A write outside period production (from the engine, between periods) is
// kept for the start of the next period.
//
// The decoder has already put the value in its shadow when it gets here, so
// dropping it silently would leave the two out of step for good: the next
// equal write is suppressed as unchanged, and a dropped key-off became a
// note that nothing could stop. A write that does not fit is noted instead,
// and the next period brings the kernel back into step (flushPending).
void AtariDspOPL::emit(uint32 block, uint16 address, int32 value) {
	if (_period) {
		if (!_audio->addEvent(_period, block, address, (uint32)value))
			noteLost();
		return;
	}
	if (s_pendingCount < kPendingMax) {
		s_pending[2 * s_pendingCount] = address;
		s_pending[2 * s_pendingCount + 1] = (uint32)value;
		++s_pendingCount;
		return;
	}
	noteLost();
}

namespace {

// A resend straight into a period, noting whether all of it fitted.
struct PeriodSink : OplPractical::Sink {
	AtariDspAudio *audio;
	AtariDspAudio::Period *period;
	bool complete;
	PeriodSink(AtariDspAudio *a, AtariDspAudio::Period *p) : audio(a), period(p), complete(true) {}
	void write(uint32 block, uint16 address, int32 value) override {
		if (!audio->addEvent(period, block, address, (uint32)value))
			complete = false;
	}
};

} // End of anonymous namespace

void AtariDspOPL::flushPending(AtariDspAudio *audio, AtariDspAudio::Period *period) {
	// A fresh period takes the whole queue, the largest resend (every slot,
	// the machine's words too), the master gain, and still has room.
	static_assert(kPendingMax + OplPractical::kSlots * 18 + OplPractical::kChannels * 2 + 2 + 1
	              <= AtariDspAudio::kMaxEvents, "a full queue and its resync must fit one period");
	for (uint32 i = 0; i < s_pendingCount; ++i) {
		if (!audio->addEvent(period, 0, (uint16)s_pending[2 * i], s_pending[2 * i + 1])) {
			s_resync = true;
			break;
		}
	}
	s_pendingCount = 0;
	// Replayed before the resend, so that a reset waiting at the head of the
	// queue still sends the machine its own words first. Without an instance
	// there is no shadow to resend; the next instance's reset supersedes it.
	if (s_resync && s_instance) {
		PeriodSink sink(audio, period);
		s_instance->_decoder->resend(&sink, 0, s_resyncMachine);
		if (sink.complete)
			s_resync = s_resyncMachine = false;
	}
}

void AtariDspOPL::setCallbackFrequency(int timerFrequency) {
	_framesPerTick16 = timerFrequency > 0
		? (uint32)((OPL_PRACTICAL_CODEC_RATE / timerFrequency) * 65536.0 + 0.5) : 0;
}

void AtariDspOPL::startCallbacks(int timerFrequency) {
	setCallbackFrequency(timerFrequency);
	_nextTick16 = 0;
	_running = true;
}

void AtariDspOPL::stopCallbacks() {
	_running = false;
}

void AtariDspOPL::producePeriod(AtariDspAudio::Period *period) {
	_period = period;
	_block = 0;

	const uint32 periodFrames16 = (uint32)AtariDspAudio::kPeriodFrames << 16;
	if (_running && _framesPerTick16) {
		while (_nextTick16 < periodFrames16) {
			_block = (_nextTick16 >> 16) / AtariDspAudio::kBlockFrames;
			if (_callback && _callback->isValid())
				(*_callback)();
			_nextTick16 += _framesPerTick16;
		}
		_nextTick16 -= periodFrames16;
	}
	_block = AtariDspAudio::kPeriodBlocks - 1;
	_period = nullptr;
	_block = 0;
}

#endif
