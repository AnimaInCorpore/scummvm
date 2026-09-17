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

#define FORCE_TEXT_CONSOLE

#include "backends/mixer/atari/atari-mixer.h"

#include <mint/falcon.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>
// https://github.com/mikrosk/usound
// This build image ships usound.h >= 2, against which usound_compat.h #errors
// by design. The shim stays in the tree for images still on uSound v1.
#include <usound.h>

#include "common/config-manager.h"
#include "common/debug.h"
#include "common/textconsole.h"

#ifdef ATARI_PCM_PROBE
#include "backends/mixer/atari/atari-pcm-probe.h"
#endif

#ifdef ATARI_DSP_OPL
#include "audio/mixer_intern.h"
#include "backends/platform/atari/atari-dsp.h"
#include "backends/platform/atari/dsp-opl.h"
#include "devtools/atari-falcon030/tools/foa-opl3/opl-practical.h"

extern AtariDspAudio *g_atariDspAudio;
#endif

#ifdef DISABLE_FANCY_THEMES
#define DEFAULT_OUTPUT_RATE			11025
#define DEFAULT_OUTPUT_CHANNELS		1
#define DEFAULT_SAMPLES				512		// 2 * 46ms (42ms at 12292 Hz) latency
#else
#define DEFAULT_OUTPUT_RATE			22050
#define DEFAULT_OUTPUT_CHANNELS		2
#define DEFAULT_SAMPLES				1024	// 2 * 46ms (42ms at 24585 Hz) latency
#endif

static USoundContext usoundContext;
static bool s_usoundActive = false;

void AtariAudioShutdown() {
	if (!s_usoundActive)
		return;
	Jdisint(MFP_TIMERA);
	USoundDeinitXbios(&usoundContext);
	s_usoundActive = false;
}

static volatile enum {
	kPlaybackStopped,	// DMA not playing (initial or after starvation)
	kPlay1stHalf,		// DMA looping [Beg, Mid)
	kPlay2ndHalf		// DMA looping [Mid, End)
} s_playbackState = kPlaybackStopped;

static volatile uint32 s_updatePulse;
static volatile bool s_dmaWrapped;
static volatile bool s_isrStoppedDma;

#ifdef ATARI_FALCON_GAME_ONLY
// Zeroed whole frames that DMA loops while update() is starved. The ISR
// switches to them instead of stopping DMA: an immediate stop can land between
// the two words of a stereo frame, and Hatari's crossbar keeps that left/right
// phase across the restart, swapping the channels for the rest of the run.
// A buffer change applies at the next frame boundary, which keeps the phase.
// Whether a physical Falcon also keeps the phase is unverified.
static byte *s_silenceStart, *s_silenceEnd;

// update() and the ISR both write the playback frame registers. Timer A is
// masked while the XBIOS calls run; masking (IMRA) rather than disabling
// (IERA) keeps a pending wrap latched. The MFP is supervisor-only.
static byte *s_queueStart, *s_queueEnd;
static bool s_queueStartDma, s_queueAccepted;

static long queuePlayBufferSuper() {
	volatile byte *const imra = (volatile byte *)0xFFFFFA13L;
	*imra &= ~(1 << 5);
	// Never queue audio behind the ISR's silence; the next update()
	// restarts from the stopped state.
	s_queueAccepted = s_queueStartDma || !s_isrStoppedDma;
	if (s_queueAccepted) {
		Setbuffer(SR_PLAY, s_queueStart, s_queueEnd);
		if (s_queueStartDma)
			Buffoper(SB_PLA_ENA | SB_PLA_RPT);
	}
	*imra |= (1 << 5);
	return 0;
}

static bool queuePlayBuffer(byte *start, byte *end, bool startDma) {
	s_queueStart = start;
	s_queueEnd = end;
	s_queueStartDma = startDma;
	Supexec(queuePlayBufferSuper);
	return s_queueAccepted;
}
#endif

static void __attribute__((interrupt)) timerA(void) {
	static uint32 s_lastPulseSeen;

	if (s_updatePulse == s_lastPulseSeen) {
#ifdef ATARI_FALCON_GAME_ONLY
		// update() didn't run since the previous wrap: loop silence from the
		// next frame boundary. update() restarts as if DMA had stopped.
		volatile byte *const dma = (volatile byte *)0xFFFF8900L;
		dma[0x01] &= 0x7f;	// select the playback frame registers
		dma[0x03] = (uint32)s_silenceStart >> 16;
		dma[0x05] = (uint32)s_silenceStart >> 8;
		dma[0x07] = (uint32)s_silenceStart;
		dma[0x0f] = (uint32)s_silenceEnd >> 16;
		dma[0x11] = (uint32)s_silenceEnd >> 8;
		dma[0x13] = (uint32)s_silenceEnd;
#else
		// update() didn't run since the previous wrap: stop the playback
		*((volatile byte *)0xFFFF8901L) = 0;
#endif
		s_isrStoppedDma = true;
	} else {
		s_dmaWrapped = true;
	}
#ifdef ATARI_PCM_PROBE
	if (s_pcmProbe)
		s_pcmProbe->irq(s_updatePulse == s_lastPulseSeen);
#endif
	s_lastPulseSeen = s_updatePulse;

	// clear in-service bit
	*((volatile byte *)0xFFFFFA0FL) = ~(1 << 5);
}

AtariMixerManager::AtariMixerManager() : MixerManager() {
	debug("AtariMixerManager()");

	suspendAudio();

	ConfMan.registerDefault("output_rate", DEFAULT_OUTPUT_RATE);
	_outputRate = ConfMan.getInt("output_rate");
	if (_outputRate <= 0)
		_outputRate = DEFAULT_OUTPUT_RATE;

	ConfMan.registerDefault("output_channels", DEFAULT_OUTPUT_CHANNELS);
	_outputChannels = ConfMan.getInt("output_channels");
	if (_outputChannels <= 0 || _outputChannels > 2)
		_outputChannels = DEFAULT_OUTPUT_CHANNELS;

	ConfMan.registerDefault("audio_buffer_size", DEFAULT_SAMPLES);
	_samples = ConfMan.getInt("audio_buffer_size");
	if (_samples <= 0)
		_samples = DEFAULT_SAMPLES;

	g_system->getEventManager()->getEventDispatcher()->registerObserver(this, 10, false);
}

AtariMixerManager::~AtariMixerManager() {
	debug("~AtariMixerManager()");

	g_system->getEventManager()->getEventDispatcher()->unregisterObserver(this);

	AtariAudioShutdown();

#ifdef ATARI_DSP_OPL
	if (_dsp) {
		_dsp->setProducer(nullptr, nullptr);
		uint32 rendered = 0, late = 0;
		_dsp->poll();
		_dsp->queryCounters(rendered, late);
		debug("AtariDspAudio: %u periods submitted, %u rendered, %u late, %u protocol errors, %u pcm underruns",
		      _dsp->periodsSubmitted(), rendered, late, _dsp->protocolErrors(), _dspPcmUnderruns);
		g_atariDspAudio = nullptr;
		delete _dsp;
		_dsp = nullptr;
	}
	delete[] _dspPcmRing;
	_dspPcmRing = nullptr;
#endif

#ifdef ATARI_PCM_PROBE
	if (s_pcmProbe) {
		s_pcmProbe->report();
		delete s_pcmProbe;
		s_pcmProbe = nullptr;
	}
#endif

	if (_atariSampleBuffer)
		Mfree(_atariSampleBuffer);
	_atariSampleBuffer = nullptr;

	delete[] _sampleBuffer;
	_sampleBuffer = nullptr;
}

#ifdef ATARI_DSP_OPL
bool AtariMixerManager::initDsp() {
	ConfMan.registerDefault("atari_dsp_audio", true);
	if (!ConfMan.getBool("atari_dsp_audio"))
		return false;
	_dsp = new AtariDspAudio();
	if (!_dsp->boot() || !_dsp->startStream()) {
		warning("AtariMixerManager: DSP audio unavailable, using DMA playback");
		delete _dsp;
		_dsp = nullptr;
		return false;
	}
	g_atariDspAudio = _dsp;
	_dspMode = true;
	_outputRate = AtariDspAudio::kPcmRateHz;
	_outputChannels = 1;
	_samples = AtariDspAudio::kPcmPerPeriod;
	_sampleBufferSize = _samples * 4;   // the mixer produces 32-bit samples
	_sampleBuffer = new uint8[_sampleBufferSize];
	_dspPcmRing = new int16[kDspPcmChunks * AtariDspAudio::kPcmPerPeriod];
	_dspPcmHead = _dspPcmTail = 0;
	debug("AtariMixerManager: DSP audio at %d Hz mono, %d-frame periods at %d Hz",
	      _outputRate, AtariDspAudio::kPeriodFrames, AtariDspAudio::kCodecRateHz);
	_mixer = new Audio::MixerImpl(_outputRate, false, _samples, 4, false);
	_mixer->setReady(true);
	// Fills the ring, then the interrupt takes over the periods.
	resumeAudio();
	_dsp->setProducer(produceDspPeriod, this);
	return true;
}

// The main loop's share: speech and effects mixed ahead into the ring.
// The mixer's read path may stream from disk, which only the main loop
// can do; it also holds the mixer's mutex, which keeps the interrupt's
// producer out for the duration of a chunk.
void AtariMixerManager::updateDsp() {
	if (_audioSuspended)
		return;
	_dsp->poll();
	while (true) {
		const int filled = (_dspPcmTail - _dspPcmHead + kDspPcmChunks) % kDspPcmChunks;
		if (filled >= kDspPcmAhead)
			break;
		const int processed = _mixer->mixCallback(_sampleBuffer, _sampleBufferSize);
		const int32 *src = (const int32 *)_sampleBuffer;
		int16 *chunk = _dspPcmRing + _dspPcmTail * AtariDspAudio::kPcmPerPeriod;
		for (int i = 0; i < _samples; ++i) {
			int32 v = i < processed ? src[i] : 0;
			if (v > 32767)
				v = 32767;
			else if (v < -32768)
				v = -32768;
			chunk[i] = (int16)v;
		}
		_dspPcmTail = (_dspPcmTail + 1) % kDspPcmChunks;
	}
	// About every seven seconds, the transport's view of the stream.
	const uint32 submitted = _dsp->periodsSubmitted();
	if (submitted - _dspLoggedPeriods >= 512) {
		_dspLoggedPeriods = submitted;
		uint32 rendered = 0, late = 0, refusedStreak = 0, refusedMutex = 0, refusedAllocator = 0, refusedLevel = 0;
		uint32 extended = 0, produceMax = 0, emptyTicks = 0, emptyStreak = 0;
		_dsp->queryCounters(rendered, late);
		_dsp->productionStats(refusedStreak, refusedMutex, refusedAllocator, refusedLevel, extended, produceMax, emptyTicks, emptyStreak);
		debug("AtariDspAudio: %u periods submitted, %u rendered, %u late, %u protocol errors, %u pcm underruns, "
		      "%u extended, refused %u ms max (%u mutex, %u allocator, %u level), production %u ms max, empty %u ticks (%u max)",
		      submitted, rendered, late, _dsp->protocolErrors(), _dspPcmUnderruns,
		      extended, refusedStreak, refusedMutex, refusedAllocator, refusedLevel, produceMax, emptyTicks, emptyStreak);
	}
}

// The interrupt's share, one period: the music volume as the kernel's
// master gain when it changed, the OPL's timer callbacks for the period
// (the AdLib driver and iMUSE's sequencing, whose register writes become
// the period's events), and the next PCM chunk, or silence when the main
// loop has not mixed one in time. No I/O and no OS calls happen here; see
// atari-dsp.h. An extension period (runCallbacks false) skips the
// callbacks and must touch nothing the main loop guards.
bool AtariMixerManager::produceDspPeriod(void *context, bool runCallbacks) {
	AtariMixerManager *self = (AtariMixerManager *)context;
	if (self->_audioSuspended)
		return false;
	AtariDspAudio::Period *period = self->_dsp->beginPeriod(!runCallbacks);
	if (!period)
		return false;
	const int volume = self->_mixer->getVolumeForSoundType(Audio::Mixer::kMusicSoundType);
	if (volume != self->_dspMusicVolume) {
		self->_dspMusicVolume = volume;
		const uint32 gain = volume >= Audio::Mixer::kMaxMixerVolume ? 0x7fffffu
			: (uint32)((uint64)volume * 0x7fffffu / Audio::Mixer::kMaxMixerVolume);
		self->_dsp->addEvent(period, 0, OplPractical::SC_MASTER_GAIN, gain);
	}
	if (runCallbacks && AtariDspOPL::instance())
		AtariDspOPL::instance()->producePeriod(period);
	// An extension nested in a production that is between reading the ring
	// head and advancing it takes silence rather than the same chunk twice.
	if (self->_dspPcmHead != self->_dspPcmTail && !(!runCallbacks && self->_dspPcmTaking)) {
		self->_dspPcmTaking = true;
		self->_dsp->setPcm(period, self->_dspPcmRing + self->_dspPcmHead * AtariDspAudio::kPcmPerPeriod);
		self->_dspPcmHead = (self->_dspPcmHead + 1) % kDspPcmChunks;
		self->_dspPcmTaking = false;
	} else {
		self->_dsp->setPcm(period, nullptr);
		if (runCallbacks)
			++self->_dspPcmUnderruns;
	}
	self->_dsp->submit(period);
	return true;
}
#endif

void AtariMixerManager::init() {
#ifdef ATARI_DSP_OPL
	if (initDsp())
		return;
#endif
	USoundSpec desired, obtained;

	desired.frequency = _outputRate;
	desired.channels = _outputChannels;
	desired.format = USoundFormatSigned16MSB;
	desired.samples = _samples;

	if (!USoundInitXbios(&desired, &obtained, &usoundContext)) {
		error("Sound system is not available");
	}
	s_usoundActive = true;

	if (obtained.format != USoundFormatSigned8 && obtained.format != USoundFormatSigned16MSB) {
		error("Sound system currently supports only 8/16-bit signed big endian samples");
	}

	// don't use the recommended number of samples
	obtained.size = obtained.size * desired.samples / obtained.samples;
	obtained.samples = desired.samples;

	_outputRate = obtained.frequency;
	if (desired.channels == 1 && obtained.channels == 2 && obtained.format == USoundFormatSigned16MSB) {
		_outputChannels = 1;
		_emulated16bitMono = true;
	} else {
		_outputChannels = obtained.channels;
		_emulated16bitMono = false;
	}
	_downsample = (obtained.format == USoundFormatSigned8);
	_samples = obtained.samples;

	debug("setting %d Hz mixing frequency, %d-bit, %s",
		_outputRate,
		obtained.format == USoundFormatSigned8 ? 8 : 16,
		_outputChannels == 2
			? "stereo"
			: _emulated16bitMono
				? "mono (emulated)"
				: "mono");
	debug("audio buffer size: %d", _samples);

	_atariSampleBufferSize = obtained.size * 2;	// two buffers
#ifdef ATARI_FALCON_GAME_ONLY
	// Followed by a quarter of a half of zeroed whole frames for starvation.
	uint32 silenceBytes = MAX<uint32>(4, (obtained.size / 4) & ~3);
	_atariSampleBuffer = (byte *)Mxalloc(_atariSampleBufferSize + silenceBytes, MX_STRAM);
#else
	_atariSampleBuffer = (byte *)Mxalloc(_atariSampleBufferSize, MX_STRAM);
#endif
	if (!_atariSampleBuffer) {
		_atariSampleBufferSize = 0;
		error("Failed to allocate memory in ST RAM");
	}
#ifdef ATARI_FALCON_GAME_ONLY
	s_silenceStart = _atariSampleBuffer + _atariSampleBufferSize;
	s_silenceEnd = s_silenceStart + silenceBytes;
	memset(s_silenceStart, 0, silenceBytes);
#endif

	Setinterrupt(SI_TIMERA, SI_PLAY);
	Xbtimer(XB_TIMERA, 1<<3, 1, timerA);	// event count mode, count to '1'
	Jenabint(MFP_TIMERA);

	// route both mic channels to the ADC
	Soundcmd(ADCINPUT, 0);
	// enable and mix both sources (ADC and connection matrix) to the output
	Soundcmd(ADDERIN, MATIN|ADCIN);

	_sampleBufferSize = _samples * _outputChannels * 4;	// always 32-bit
	_sampleBuffer = new uint8[_sampleBufferSize];

	_mixer = new Audio::MixerImpl(_outputRate, _outputChannels == 2, _samples, 4, false);
	_mixer->setReady(true);

#ifdef ATARI_PCM_PROBE
	ConfMan.registerDefault("pcm_probe_resident", false);
	ConfMan.registerDefault("pcm_probe_delay_ms", 20000);
	ConfMan.registerDefault("pcm_probe_duration_ms", 60000);
	ConfMan.registerDefault("pcm_probe_buffer_bytes", 262144);
	ConfMan.registerDefault("pcm_probe_chunk_bytes", 32768);
	ConfMan.registerDefault("pcm_probe_click_x", -1);
	ConfMan.registerDefault("pcm_probe_click_y", -1);
	if (ConfMan.hasKey("pcm_probe_file"))
		s_pcmProbe = new AtariPcmProbe(_mixer, _atariSampleBufferSize);
#endif

	resumeAudio();
}

void AtariMixerManager::suspendAudio() {
	debug("suspendAudio");

#ifdef ATARI_DSP_OPL
	if (_dspMode) {
		_audioSuspended = true;
		return;
	}
#endif
	Buffoper(0x00);
	s_playbackState = kPlaybackStopped;
	_audioSuspended = true;
}

int AtariMixerManager::resumeAudio() {
	debug("resumeAudio");

	_audioSuspended = false;
	update();
	return 0;
}

bool AtariMixerManager::notifyEvent(const Common::Event &event) {
	switch (event.type) {
	case Common::EVENT_QUIT:
	case Common::EVENT_RETURN_TO_LAUNCHER:
		if (s_playbackState != kPlaybackStopped) {
			debug("silencing the mixer");
			suspendAudio();
		}
		return false;
	default:
		break;
	}

	return false;
}

void AtariMixerManager::update() {
	if (_audioSuspended) {
		return;
	}

	assert(_mixer);

#ifdef ATARI_DSP_OPL
	if (_dspMode) {
		updateDsp();
		return;
	}
#endif

	s_updatePulse++;
#ifdef ATARI_PCM_PROBE
	if (s_pcmProbe)
		s_pcmProbe->updatePulse();
#endif

	// Translate ISR's starvation signal into a state transition. Done
	// here so that update() is the only writer of s_playbackState.
	if (s_isrStoppedDma) {
		s_isrStoppedDma = false;
		s_playbackState = kPlaybackStopped;
	}

	byte *atariSampleBuffer1stHalf = _atariSampleBuffer;
	byte *atariSampleBuffer2ndHalf = _atariSampleBuffer + _atariSampleBufferSize/2;
	byte *atariSampleBufferEnd     = _atariSampleBuffer + _atariSampleBufferSize;

	bool needsMix = false;

	if (s_playbackState == kPlaybackStopped) {
		memset(_atariSampleBuffer, 0, _atariSampleBufferSize);
#ifdef ATARI_FALCON_GAME_ONLY
		_queuedDmaBuffer = atariSampleBuffer1stHalf;
		// After starvation DMA is still looping silence; the new half is
		// queued behind it and the position check below waits for it.
		queuePlayBuffer(atariSampleBuffer1stHalf, atariSampleBuffer2ndHalf, true);
#else
		Setbuffer(SR_PLAY, atariSampleBuffer1stHalf, atariSampleBuffer2ndHalf);
		Buffoper(SB_PLA_ENA | SB_PLA_RPT);
#endif
		s_playbackState = kPlay1stHalf;
		// Buffoper's 0->ENA transition can also fire SI_PLAY. On Falcon,
		// read the actual DMA position below instead of counting this as
		// a completed buffer.
		needsMix = true;
	}

	if (s_dmaWrapped) {
		s_dmaWrapped = false;
#ifndef ATARI_FALCON_GAME_ONLY
		if (s_playbackState == kPlay1stHalf)
			s_playbackState = kPlay2ndHalf;
		else if (s_playbackState == kPlay2ndHalf)
			s_playbackState = kPlay1stHalf;
#endif
		needsMix = true;
	}

#ifdef ATARI_FALCON_GAME_ONLY
	byte *buf = nullptr;
	if (needsMix) {
		SndBufPtr pointers;
		Buffptr(&pointers);
		byte *playing = (byte *)pointers.play;
		byte *playingHalf = playing >= atariSampleBuffer1stHalf && playing < atariSampleBufferEnd
			? (playing < atariSampleBuffer2ndHalf ? atariSampleBuffer1stHalf : atariSampleBuffer2ndHalf)
			: nullptr;
		// A wrap during the previous mix (or the startup interrupt) can
		// leave another notification pending. Do not overwrite the queued
		// half before DMA has actually started reading it.
		needsMix = playingHalf && playingHalf == _queuedDmaBuffer;
		if (needsMix)
			buf = playingHalf == atariSampleBuffer1stHalf ? atariSampleBuffer2ndHalf : atariSampleBuffer1stHalf;
	}
#endif

	if (!needsMix) {
#ifdef ATARI_PCM_PROBE
		if (s_pcmProbe)
			s_pcmProbe->service();
#endif
		return;
	}

#ifdef ATARI_PCM_PROBE
	uint32 probeMixBegin = g_system->getMillis();
#endif

#ifndef ATARI_FALCON_GAME_ONLY
	// Legacy backend: queue the next half before mixing.
	byte *buf;
	if (s_playbackState == kPlay1stHalf) {
		buf = atariSampleBuffer2ndHalf;
		Setbuffer(SR_PLAY, atariSampleBuffer2ndHalf, atariSampleBufferEnd);
	} else {
		buf = atariSampleBuffer1stHalf;
		Setbuffer(SR_PLAY, atariSampleBuffer1stHalf, atariSampleBuffer2ndHalf);
	}
#endif

	int processed = _mixer->mixCallback(_sampleBuffer, _sampleBufferSize);

#ifdef ATARI_PCM_PROBE
	if (s_pcmProbe)
		s_pcmProbe->dmaPosition(buf, _atariSampleBufferSize / 2, true);
#endif

	// WARNING: loopCount, src and dst are modified by the asm code
	int loopCount = processed * _outputChannels;
	const byte *src = _sampleBuffer;
	byte *dst = buf;

	if (_downsample) {
		__asm__ volatile(
			"	subq.l	#1,%0\n"
			"	bmi.b	3f\n"
			"	move.l	#32768,%%d2\n"
			"	move.l	#65535,%%d3\n"
			"	moveq	#31,%%d4\n"
			"	moveq	#0x7f,%%d5\n"
			"1:	move.l	(%1)+,%%d0\n"
			"	move.l	%%d0,%%d1\n"
			"	add.l	%%d2,%%d1\n"
			"	cmp.l	%%d3,%%d1\n"
			"	bhi.b	2f\n"
			"	asr.l	#8,%%d0\n"	// TODO: tweak (there were reports that >> 8 is too quiet)
			"	move.b	%%d0,(%2)+\n"
			"	dbra	%0,1b\n"
			"	bra.b	3f\n"
			"2:	asr.l	%%d4,%%d0\n"
			"	eor.b	%%d5,%%d0\n"
			"	move.b	%%d0,(%2)+\n"
			"	dbra	%0,1b\n"
			"3:\n"
			: "+d"(loopCount), "+a"(src), "+a"(dst) // outputs
			: // inputs
			: "d0", "d1", "d2", "d3", "d4", "d5", "cc" AND_MEMORY
		);
		memset(buf + processed * _outputChannels *2/2, 0, (_samples - processed) * _outputChannels * 2/2);
	} else {
		if (!_emulated16bitMono) {
			__asm__ volatile(
				"	subq.l	#1,%0\n"
				"	bmi.b	3f\n"
				"	move.l	#32768,%%d2\n"
				"	move.l	#65535,%%d3\n"
				"	moveq	#31,%%d4\n"
				"	move.w	#0x7fff,%%d5\n"
				"1:	move.l	(%1)+,%%d0\n"
				"	move.l	%%d0,%%d1\n"
				"	add.l	%%d2,%%d1\n"
				"	cmp.l	%%d3,%%d1\n"
				"	bhi.b	2f\n"
				"	move.w	%%d0,(%2)+\n"
				"	dbra	%0,1b\n"
				"	bra.b	3f\n"
				"2:	asr.l	%%d4,%%d0\n"
				"	eor.w	%%d5,%%d0\n"
				"	move.w	%%d0,(%2)+\n"
				"	dbra	%0,1b\n"
				"3:\n"
				: "+d"(loopCount), "+a"(src), "+a"(dst) // outputs
				: // inputs
				: "d0", "d1", "d2", "d3", "d4", "d5", "cc" AND_MEMORY
			);
			memset(buf + processed * _outputChannels * 2, 0, (_samples - processed) * _outputChannels * 2);
		} else {
			__asm__ volatile(
				"	subq.l	#1,%0\n"
				"	bmi.b	3f\n"
				"	move.l	#32768,%%d2\n"
				"	move.l	#65535,%%d3\n"
				"	moveq	#31,%%d4\n"
				"	move.w	#0x7fff,%%d5\n"
				"1:	move.l	(%1)+,%%d0\n"
				"	move.l	%%d0,%%d1\n"
				"	add.l	%%d2,%%d1\n"
				"	cmp.l	%%d3,%%d1\n"
				"	bhi.b	2f\n"
				"	move.w	%%d0,(%2)+\n"
				"	move.w	%%d0,(%2)+\n"
				"	dbra	%0,1b\n"
				"	bra.b	3f\n"
				"2:	asr.l	%%d4,%%d0\n"
				"	eor.w	%%d5,%%d0\n"
				"	move.w	%%d0,(%2)+\n"
				"	move.w	%%d0,(%2)+\n"
				"	dbra	%0,1b\n"
				"3:\n"
				: "+d"(loopCount), "+a"(src), "+a"(dst) // outputs
				: // inputs
				: "d0", "d1", "d2", "d3", "d4", "d5", "cc" AND_MEMORY
			);
			memset(buf + processed * _outputChannels * 2*2, 0, (_samples - processed) * _outputChannels * 2*2);
		}
	}

#ifdef ATARI_PCM_PROBE
	if (s_pcmProbe)
		s_pcmProbe->dmaPosition(buf, _atariSampleBufferSize / 2, false);
#endif

#ifdef ATARI_FALCON_GAME_ONLY
	// Until conversion is complete DMA continues to loop the old half.
	// A missed deadline can repeat completed audio, but cannot expose a
	// partially written buffer. The probe's waveform/deadline gate still
	// rejects repeats.
	if (queuePlayBuffer(buf, buf + _atariSampleBufferSize / 2, false))
		_queuedDmaBuffer = buf;
#endif

	if (processed > 0 && processed != _samples) {
		warning("processed: %d, _samples: %d", processed, _samples);
	}
#ifdef ATARI_PCM_PROBE
	if (s_pcmProbe) {
		s_pcmProbe->mixed(probeMixBegin, g_system->getMillis(), processed);
		s_pcmProbe->service();
	}
#endif
}
