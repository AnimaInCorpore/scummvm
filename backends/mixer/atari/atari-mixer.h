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

#ifndef BACKENDS_MIXER_ATARI_H
#define BACKENDS_MIXER_ATARI_H

#include "backends/mixer/mixer.h"
#include "common/events.h"

#ifdef ATARI_DSP_OPL
class AtariDspAudio;
#endif

/**
 *  Atari XBIOS based audio mixer.
 */

class AtariMixerManager : public MixerManager, Common::EventObserver {
public:
	AtariMixerManager();
	virtual ~AtariMixerManager();

	void init() override;
	void update();

	void suspendAudio() override;
	int resumeAudio() override;

	bool notifyEvent(const Common::Event &event) override;

private:
	int _outputRate = 0;
	int _outputChannels = 0;
	bool _emulated16bitMono = false;
	bool _downsample = false;

	int _samples = 0;
	int _sampleBufferSize = 0;
	byte *_sampleBuffer = nullptr;

	int _atariSampleBufferSize = 0;
	byte *_atariSampleBuffer = nullptr;
#ifdef ATARI_FALCON_GAME_ONLY
	byte *_queuedDmaBuffer = nullptr;
#endif
#ifdef ATARI_DSP_OPL
	// The DSP owns the codec and synthesizes the AdLib voices; its
	// interrupt produces the periods (backends/platform/atari/atari-dsp.h).
	// The main loop only mixes speech and effects ahead into a ring of
	// period-sized chunks, from which the interrupt takes one per period.
	bool initDsp();
	void updateDsp();
	static bool produceDspPeriod(void *context, bool runCallbacks);
	enum {
		kDspPcmChunks = 12,   // ring capacity, one more than it ever holds
		kDspPcmAhead = 8      // chunks mixed ahead: 117 ms of loop stall before a gap
	};
	AtariDspAudio *_dsp = nullptr;
	bool _dspMode = false;
	int16 *_dspPcmRing = nullptr;
	volatile int _dspPcmHead = 0;          // the interrupt's next chunk
	volatile int _dspPcmTail = 0;          // the main loop's next chunk
	volatile uint32 _dspPcmUnderruns = 0;  // periods produced with no chunk to take
	volatile bool _dspPcmTaking = false;   // a production is between reading the head and advancing it
	int _dspMusicVolume = -1;
	uint32 _dspLoggedPeriods = 0;
#endif
};

#endif
