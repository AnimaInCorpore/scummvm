// Scripted input for the live-renderer gate, not a replacement game score.
#ifndef FCM_LIVE_DEMO_H
#define FCM_LIVE_DEMO_H
#include "live-pcm.h"
#include "live-resampler.h"

namespace FCM {

class LiveDemo {
	const Bank *_bank = nullptr;
	PCMVoice _voice;
	FalconResampler _resampler;
	uint32_t _nativeFrames = 0, _checksum = 0;
	uint32_t _tick = 0, _noteIndex = 0;
	unsigned _key = 60, _offTick = 12000;
	bool _bendNote = false;
	bool _primed = false;
	int16_t _buffer[256];
	unsigned _read = 256;
	void event() {
		if (!_tick) {
			// Vary notes across the minute so waveform alignment is unambiguous.
			_key = 48 + (_noteIndex * 7 + _noteIndex / 7) % 37;
			_offTick = _noteIndex % 6 == 3 ? 1600 : 12000;
			_bendNote = _noteIndex % 6 == 4;
			_voice.noteOn(*_bank, 103, _key, 32 + (_noteIndex * 17) % 96);
		}
		if (_tick == _offTick) _voice.noteOff(_key);
		if (_bendNote && _tick == 2000) _voice.bend(12288);
		if (_bendNote && _tick == 6000) _voice.bend(8192);
	}
	__attribute__((noinline)) void refill() {
		unsigned used = 0;
		while (used < 256) {
			event();
			uint32_t end = 32000;
			if (_tick < _offTick) end = _offTick;
			if (_bendNote && _tick < 6000 && end > 6000) end = 6000;
			if (_bendNote && _tick < 2000 && end > 2000) end = 2000;
			unsigned span = end - _tick;
			if (span > 256 - used) span = 256 - used;
			_voice.render(_buffer + used, span);
			used += span; _tick += span;
			if (_tick == 32000) { _tick = 0; ++_noteIndex; }
		}
		// Hash in its own small loop. Read-ahead is reported explicitly below.
		for (unsigned i = 0; i < 256; ++i)
			_checksum = ((_checksum << 5) | (_checksum >> 27)) ^ uint16_t(_buffer[i]);
		_nativeFrames += 256; _read = 0;
	}
	int16_t nativeSample() {
		if (_read == 256) refill();
		return _buffer[_read++];
	}
public:
	bool open(const Bank &bank) {
		*this = LiveDemo();
		if (!_voice.noteOn(bank, 103, 60, 32)) return false;
		_resampler.open();
		_bank = &bank; _voice.stop(); return true;
	}
	uint32_t frames() const { return _nativeFrames; }
	uint32_t checksum() const { return _checksum; }
	void read(int16_t *stereo, unsigned frames) {
		// Exact Falcon codec divider, independent 32 kHz synthesis clock.
		// The rational interpolation schedule never accumulates clock drift.
		if (!frames || !_bank) return;
		if (!_primed) {
			int16_t a = nativeSample(), b = nativeSample();
			_resampler.prime(a, b); _primed = true;
		}
		// Unequal stereo cancels centered CD speech in the transport gate.
		// This is diagnostic routing, not MT-32 panning/reverb emulation.
		while (frames) {
			if (_read == 256) refill();
			const int16_t *cursor = _buffer + _read;
			unsigned count = _resampler.render(stereo, frames, cursor, _buffer + 256);
			_read = cursor - _buffer;
			stereo += count * 2; frames -= count;
		}
	}
};
} // namespace FCM
#endif
