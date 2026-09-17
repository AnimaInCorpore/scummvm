/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Opt-in Falcon PCM transport diagnostic. Only the supplemental probe build
 * includes this file. This is a linear test stream, not an iMUSE backend.
 */

#ifndef BACKENDS_MIXER_ATARI_PCM_PROBE_H
#define BACKENDS_MIXER_ATARI_PCM_PROBE_H

#include "audio/audiostream.h"
#include "audio/mixer_intern.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/system.h"
#include "backends/platform/atari/dlmalloc.h"
#include <mint/basepage.h>
#ifdef ATARI_FCM_LIVE_PROBE
#include "devtools/atari-falcon030/tools/foa-compiled-music/live-demo.h"
#endif

class AtariPcmProbe : public Audio::AudioStream {
public:
	AtariPcmProbe(Audio::MixerImpl *mixer, uint32 dmaBytes) : _mixer(mixer), _dmaBytes(dmaBytes) {
		debug("PCM probe target: '%s'", ConfMan.getActiveDomainName().c_str());
		_rate = mixer->getOutputRate();
		if (_rate != 49170 || !mixer->getOutputStereo())
			error("PCM probe requires obtained 49170 Hz stereo output");
		if (mixer->getOutputBufSize() < 512 || mixer->getOutputBufSize() > 16384)
			error("PCM probe supports 512 through 16384 frames per DMA half");
		_resident = ConfMan.getBool("pcm_probe_resident");
		_delay = ConfMan.getInt("pcm_probe_delay_ms");
		_duration = ConfMan.getInt("pcm_probe_duration_ms");
		_capacity = ConfMan.getInt("pcm_probe_buffer_bytes");
		_chunk = ConfMan.getInt("pcm_probe_chunk_bytes");
		_clickX = ConfMan.getInt("pcm_probe_click_x");
		_clickY = ConfMan.getInt("pcm_probe_click_y");
		if (_delay > 600000 || _duration < 1000 || _duration > 120000 ||
		    _capacity < 16384 || _capacity > 4194304 || (_capacity & 3) ||
		    _chunk < 4096 || _chunk > _capacity || (_chunk & 3))
			error("Invalid PCM probe limits");
		if (!_file.open(Common::FSNode(ConfMan.getPath("pcm_probe_file"))))
			error("Cannot open PCM probe file");
		_fileBytes = _file.size();
#ifdef ATARI_FCM_LIVE_PROBE
		_liveMode = ConfMan.getBool("fcm_live_probe");
		if (_liveMode) {
			if (_fileBytes < 12 || _fileBytes > 16 * 1024 * 1024)
				error("Invalid FCM live probe package size");
			_buffer = new byte[_fileBytes];
			if (_file.read(_buffer, _fileBytes) != _fileBytes || !_bank.open(_buffer, _fileBytes))
				error("FCM live probe bank/profile validation failed");
			_linearTable = new uint16[FCM::Bank::kLinearValues];
			_bank.makeLinearTable(_linearTable);
			if (!_demo.open(_bank)) error("FCM live probe timbre profile rejected");
			_file.close();
			_resident = true;
			_capacity = _chunk = 0; // No PCM ring or disk reads during synthesis.
		} else {
#endif
		if (!_fileBytes || (_fileBytes & 3) || (_resident && _fileBytes > _capacity))
			error("PCM probe needs whole stereo PCM16 frames; resident file must fit buffer");
		_buffer = new byte[_capacity];
		if (_resident) {
			if (_file.read(_buffer, _fileBytes) != _fileBytes)
				error("PCM resident preload failed");
		} else {
			while (_queued < _capacity)
				refill();
		}
#ifdef ATARI_FCM_LIVE_PROBE
		}
#endif
		_origin = g_system->getMillis();
		_minQueued = _capacity;
		_minLargestFree = Mxalloc(-1, MX_STRAM);
	}

	~AtariPcmProbe() override {
		_mixer->stopHandle(_handle);
		delete[] _buffer;
#ifdef ATARI_FCM_LIVE_PROBE
		delete[] _linearTable;
#endif
	}

	int getRate() const override { return _rate; }
	bool isStereo() const override { return true; }
	bool endOfData() const override { return false; }
	bool endOfStream() const override { return false; }

	int readBuffer(int16 *buffer, const int numSamples) override {
#ifdef ATARI_FCM_LIVE_PROBE
		if (_liveMode) {
			if (numSamples & 1) error("FCM stereo probe requires whole frames");
			_demo.read(buffer, numSamples / 2);
			_consumedBytes += numSamples * 2;
			return numSamples;
		}
#endif
		uint32 needed = numSamples * 2;
		uint32 available = _resident ? needed : MIN(needed, _queued);
		uint32 copied = 0;
		while (copied < available) {
			uint32 count = MIN(available - copied, (_resident ? _fileBytes : _capacity) - _read);
			memcpy((byte *)buffer + copied, _buffer + _read, count);
			_read += count;
			if (_read == (_resident ? _fileBytes : _capacity))
				_read = 0;
			copied += count;
		}
		if (!_resident) {
			_queued -= available;
			_minQueued = MIN(_minQueued, _queued);
		}
		if (available < needed) {
			// Keep the mixer clock moving, and make starvation an explicit failure.
			memset((byte *)buffer + available, 0, needed - available);
			_missingBytes += needed - available;
		}
		_consumedBytes += available;
		return numSamples;
	}

	void service() {
		uint32 now = g_system->getMillis();
		if (!_started && now - _origin >= _delay) {
			_started = true;
			_start = now;
			_maxReadMs = _readMs = _readBytes = 0;
			_mixer->playStream(Audio::Mixer::kMusicSoundType, &_handle, this, -1,
			                  Audio::Mixer::kMaxChannelVolume, 0, DisposeAfterUse::NO, true, false);
			__asm__ volatile("move.l #0x50434d31,%%d0" : : : "d0"); // Hatari start marker
		}
		if (!_started || _finished)
			return;
		if (_clickX >= 0 && _inputStep < 3 && now - _start >= 2000 + _inputStep * 100) {
			Common::Event input;
			input.type = _inputStep == 0 ? Common::EVENT_MOUSEMOVE :
			             _inputStep == 1 ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP;
			input.mouse = Common::Point(_clickX, _clickY);
			g_system->getEventManager()->pushEvent(input);
			++_inputStep;
		}
		if (now - _start >= _duration) {
			_finished = true;
			_end = now;
			__asm__ volatile("move.l #0x50434d32,%%d0" : : : "d0"); // Hatari end marker
			Common::Event event;
			event.type = Common::EVENT_QUIT;
			g_system->getEventManager()->pushEvent(event);
			return;
		}
		// A mixer block can consume several disk chunks. Replenish all whole
		// chunks now: under game load there may be no idle service() calls
		// between consecutive mixes. The ring capacity bounds the work here.
		while (!_resident && _capacity - _queued >= _chunk)
			refill(); // Cooperative main-thread I/O, never in readBuffer() or the ISR.
		if (now - _lastMemoryMs >= 1000) {
			_lastMemoryMs = now;
			long largestFree = Mxalloc(-1, MX_STRAM);
			if (largestFree >= 0)
				_minLargestFree = MIN(_minLargestFree, (uint32)largestFree);
		}
	}

	void updatePulse() {
		uint32 now = g_system->getMillis();
		if (_started && !_finished)
			_maxUpdateGapMs = MAX(_maxUpdateGapMs, now - _lastUpdate);
		_lastUpdate = now;
	}

	void irq(bool stopped) {
		if (!_started) {
			// Starvation while loading is outside the measurement, but it is
			// where a mid-frame DMA stop would have swapped the channels.
			_stopsBeforeStart += stopped;
			return;
		}
		if (_finished)
			return;
		uint32 now = g_system->getMillis();
		if (stopped)
			++_stops;
		uint32 index = _irqCount;
		if (index < kLogSize) {
			_irqLog[index][0] = now;
			_irqLog[index][1] = stopped;
			_irqLog[index][2] = now - _lastUpdate;
			_irqCount = index + 1;
		} else {
			++_irqOverflow;
		}
	}

	void mixed(uint32 begin, uint32 end, uint32 processed) {
		if (!_started || _finished)
			return;
		_maxMixMs = MAX(_maxMixMs, end - begin);
		_mixMs += end - begin;
		++_mixCalls;
		bool speech = _mixer->hasActiveChannelOfType(Audio::Mixer::kSpeechSoundType);
		_speechMixes += speech;
		if (_mixCount < kLogSize) {
			uint32 *row = _mixLog[_mixCount++];
			row[0] = begin;
			row[1] = end;
			row[2] = _consumedBytes;
			row[3] = _queued;
			row[4] = speech;
			row[5] = processed;
		} else {
			++_mixOverflow;
		}
	}

	void dmaPosition(byte *destination, uint32 bytes, bool beforeWrite) {
		if (!_started || _finished)
			return;
		SndBufPtr pointers;
		Buffptr(&pointers);
		uint32 position = (uint32)pointers.play;
		if (position >= (uint32)destination && position < (uint32)destination + bytes) {
			if (beforeWrite)
				++_activeBeforeWrite;
			else
				++_activeAfterWrite;
		}
	}

	// Called after the backend stops DMA and disables Timer A. All report I/O
	// is outside the measured interval; IRQ/main logs have separate writers.
	void report() {
		Common::DumpFile output;
		if (!output.open("PCMSTAT.TXT"))
			return;
		output.writeString(Common::String::format(
			"rate=%u\nresident=%u\nstart_ms=%u\nend_ms=%u\nrequested_ms=%u\n"
			"ring_bytes=%u\nchunk_bytes=%u\nfile_bytes=%u\ndma_bytes=%u\n"
			"consumed_bytes=%u\nmissing_bytes=%u\nmin_queued_bytes=%u\n"
			"read_bytes=%u\nread_ms=%u\nmax_read_ms=%u\n"
			"mix_calls=%u\nmix_ms=%u\nmax_mix_ms=%u\nmax_update_gap_ms=%u\n"
			"speech_mixes=%u\ndma_stops=%u\nheap_peak_bytes=%u\nmin_largest_free_stram=%u\n"
			"program_static_bytes=%u\n"
			"irq_log_overflow=%u\nmix_log_overflow=%u\n",
			_rate, _resident, _start, _end, _duration, _capacity, _chunk, _fileBytes, _dmaBytes,
			_consumedBytes, _missingBytes, _minQueued, _readBytes, _readMs, _maxReadMs,
			_mixCalls, _mixMs, _maxMixMs, _maxUpdateGapMs, _speechMixes, (uint32)_stops,
			(uint32)dlmalloc_max_footprint(), _minLargestFree,
			(uint32)(_base->p_tlen + _base->p_dlen + _base->p_blen + 256), (uint32)_irqOverflow, _mixOverflow));
		output.writeString(Common::String::format("dma_active_before_write=%u\ndma_active_after_write=%u\n",
			_activeBeforeWrite, _activeAfterWrite));
		output.writeString(Common::String::format("stops_before_start=%u\n", (uint32)_stopsBeforeStart));
#ifdef ATARI_FCM_LIVE_PROBE
		output.writeString(Common::String::format("live_synthesis=%u\nlive_native_frames=%u\nlive_native_checksum=%u\n",
			_liveMode, _demo.frames(), _demo.checksum()));
#endif
		output.writeString("irq_ms,stopped,update_age_ms\n");
		for (uint32 i = 0; i < _irqCount; ++i)
			output.writeString(Common::String::format("%u,%u,%u\n", _irqLog[i][0], _irqLog[i][1], _irqLog[i][2]));
		output.writeString("mix_begin_ms,mix_end_ms,consumed_bytes,queued_bytes,speech,frames\n");
		for (uint32 i = 0; i < _mixCount; ++i) {
			const uint32 *row = _mixLog[i];
			output.writeString(Common::String::format("%u,%u,%u,%u,%u,%u\n", row[0], row[1], row[2], row[3], row[4], row[5]));
		}
		output.writeString("probe_report_complete=1\n");
		output.finalize();
	}

private:
	void refill() {
		uint32 begin = g_system->getMillis();
		uint32 bytes = MIN(_chunk, _capacity - _queued);
		bytes = MIN(bytes, _capacity - _write);
		bytes = MIN(bytes, _fileBytes - (uint32)_file.pos());
		if (_file.read(_buffer + _write, bytes) != bytes)
			error("PCM stream read failed");
		if (_file.pos() == _fileBytes && !_file.seek(0))
			error("PCM stream rewind failed");
		uint32 elapsed = g_system->getMillis() - begin;
		_maxReadMs = MAX(_maxReadMs, elapsed);
		_readMs += elapsed;
		_readBytes += bytes;
		_write = (_write + bytes) % _capacity;
		_queued += bytes;
	}

	enum { kLogSize = 4096 };
	Audio::MixerImpl *_mixer;
	Audio::SoundHandle _handle;
	Common::File _file;
	byte *_buffer = nullptr;
	uint32 _dmaBytes, _rate, _capacity, _chunk, _fileBytes;
	uint32 _delay, _duration, _origin;
	bool _resident, _started = false, _finished = false;
	uint32 _start = 0, _end = 0, _read = 0, _write = 0, _queued = 0;
	uint32 _consumedBytes = 0, _missingBytes = 0, _minQueued = 0;
	uint32 _readBytes = 0, _readMs = 0, _maxReadMs = 0;
	uint32 _mixMs = 0, _mixCalls = 0, _maxMixMs = 0, _speechMixes = 0;
	uint32 _maxUpdateGapMs = 0, _lastMemoryMs = 0, _minLargestFree = 0;
	volatile uint32 _lastUpdate = 0, _stops = 0, _irqCount = 0, _irqOverflow = 0;
	volatile uint32 _stopsBeforeStart = 0;
	uint32 _mixCount = 0, _mixOverflow = 0;
	uint32 _activeBeforeWrite = 0, _activeAfterWrite = 0;
	int _clickX = -1, _clickY = -1;
	uint32 _inputStep = 0;
	uint32 _irqLog[kLogSize][3];
	uint32 _mixLog[kLogSize][6];
#ifdef ATARI_FCM_LIVE_PROBE
	bool _liveMode = false;
	FCM::Bank _bank;
	FCM::LiveDemo _demo;
	uint16 *_linearTable = nullptr;
#endif
};

static AtariPcmProbe *s_pcmProbe;

#endif
