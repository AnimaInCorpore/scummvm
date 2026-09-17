// ScummVM experimental FCM1 adapter. SPDX-License-Identifier: GPL-3.0-or-later
#include "scumm/imuse/imuse_fcm.h"
#include "common/crc.h"
#include "common/file.h"
#include "common/fs.h"

namespace Scumm {

static bool region(uint32 size, uint32 offset, uint32 bytes) {
	return offset <= size && bytes <= size - offset;
}

static bool nibbled(uint16 op) {
	return op == 0x7d00 || (op >= 0x7d30 && op <= 0x7d35) || op == 0x7d50;
}

static bool rawIMuse(uint16 op) {
	return op == 0x7d01 || op == 0x7d02 || op == 0x7d40 || op == 0x7d51 || op == 0x7d60;
}

static uint16 minimumIMuseArgs(uint16 op) {
	// Fields consumed by sysex_scumm.cpp after its nibble decoding.
	switch (op) {
	case 0x7d00: return 9;
	case 0x7d30: return 8;
	case 0x7d31: case 0x7d35: return 4;
	case 0x7d32: case 0x7d33: case 0x7d34: return 3;
	case 0x7d50: return 11;
	case 0x7d01: case 0x7d40: return 1;
	case 0x7d60: return 5;
	default: return 0;
	}
}

bool FCMScore::open(const Common::Path &path) {
	Common::File file;
	_score.clear();
	return file.open(Common::FSNode(path)) && load(file);
}

bool FCMScore::load(Common::SeekableReadStream &stream) {
	_score.clear();
	byte header[12];
	if (!stream.seek(0) || stream.read(header, 12) != 12 || memcmp(header, "FCM1", 4) ||
	    READ_BE_UINT16(header + 4) != 1 || READ_BE_UINT32(header + 8) != stream.size())
		return false;
	uint32 fileSize = READ_BE_UINT32(header + 8), count = READ_BE_UINT16(header + 6);
	if (fileSize > 16 * 1024 * 1024 || count == 0 || count > 64 || !region(fileSize, 12, count * 16))
		return false;
	Common::Array<byte> directory;
	directory.resize(count * 16);
	if (stream.read(directory.data(), directory.size()) != directory.size()) return false;
	uint32 end = 12 + directory.size(), scoreOffset = 0, scoreSize = 0, scoreCRC = 0;
	for (uint32 i = 0; i < count; ++i) {
		const byte *entry = directory.data() + i * 16;
		uint32 offset = READ_BE_UINT32(entry + 4), length = READ_BE_UINT32(entry + 8);
		if (offset != ((end + 3) & ~3u) || !region(fileSize, offset, length)) return false;
		for (uint32 j = 0; j < i; ++j)
			if (!memcmp(entry, directory.data() + j * 16, 4)) return false;
		if (!stream.seek(end)) return false;
		for (uint32 p = end; p < offset; ++p)
			if (stream.readByte() != 0 || stream.err() || stream.eos()) return false;
		if (!memcmp(entry, "SCOR", 4)) {
			scoreOffset = offset; scoreSize = length; scoreCRC = READ_BE_UINT32(entry + 12);
		}
		end = offset + length;
	}
	if (end != fileSize || scoreSize < 4 || scoreSize > 8 * 1024 * 1024) return false;
	Common::Array<byte> score;
	score.resize(scoreSize);
	if (!stream.seek(scoreOffset) || stream.read(score.data(), scoreSize) != scoreSize) return false;
	Common::CRC32 crc;
	if (crc.crcFast(score.data(), scoreSize) != scoreCRC) return false;
	uint32 cues = READ_BE_UINT32(score.data());
	if (cues == 0 || cues > (scoreSize - 4) / 12) return false;
	end = 4 + cues * 12;
	uint16 previous = 0;
	for (uint32 i = 0; i < cues; ++i) {
		const byte *entry = score.data() + 4 + i * 12;
		uint16 id = READ_BE_UINT16(entry);
		uint32 offset = READ_BE_UINT32(entry + 4), length = READ_BE_UINT32(entry + 8);
		if ((i && id <= previous) || offset != ((end + 3) & ~3u) || length < 24 ||
		    !region(scoreSize, offset, length)) return false;
		for (uint32 p = end; p < offset; ++p) if (score[p]) return false;
		previous = id; end = offset + length;
	}
	if (end != scoreSize) return false;
	_score.swap(score);
	return true;
}

const byte *FCMScore::cue(uint16 sound, uint32 &size) const {
	size = 0;
	if (_score.empty()) return nullptr;
	uint32 lo = 0, hi = READ_BE_UINT32(_score.data());
	while (lo < hi) {
		uint32 mid = lo + (hi - lo) / 2;
		const byte *entry = _score.data() + 4 + mid * 12;
		uint16 id = READ_BE_UINT16(entry);
		if (id < sound) lo = mid + 1;
		else if (id > sound) hi = mid;
		else { size = READ_BE_UINT32(entry + 8); return _score.data() + READ_BE_UINT32(entry + 4); }
	}
	return nullptr;
}

MidiParser_FCM::MidiParser_FCM() { memset(_trackEnds, 0, sizeof(_trackEnds)); }
MidiParser_FCM::~MidiParser_FCM() { unloadMusic(); }

void MidiParser_FCM::unloadMusic() {
	MidiParser::unloadMusic(); // Stop tracking before freeing EventInfo targets.
	_data.clear();
	memset(_trackEnds, 0, sizeof(_trackEnds));
}

bool MidiParser_FCM::loadMusic(const byte *data, uint32 size) {
	unloadMusic();
	if (!data || size < 24 || size > 1024 * 1024) return false;
	uint16 format = READ_BE_UINT16(data), ppqn = READ_BE_UINT16(data + 2), tracks = READ_BE_UINT16(data + 4);
	// This adapter deliberately covers independent tracks, not format-1 merging.
	if ((format != 0 && format != 2) || (format == 0 && tracks != 1) || tracks == 0 ||
	    tracks > MAXIMUM_TRACKS || ppqn == 0 || ppqn >= 0x8000 || READ_BE_UINT16(data + 6) != 16 ||
	    memcmp(data + 8, "MDhd\0\0\0\10", 8) || !region(size, 24, tracks * 8)) return false;
	uint32 eventEnd = 24 + tracks * 8, extra = 0;
	for (uint32 t = 0; t < tracks; ++t) {
		uint32 n = READ_BE_UINT32(data + 24 + t * 8), offset = READ_BE_UINT32(data + 28 + t * 8);
		if (!n || offset != eventEnd || n > (size - eventEnd) / 12) return false;
		eventEnd += n * 12;
	}
	for (uint32 t = 0; t < tracks; ++t) {
		uint32 count = READ_BE_UINT32(data + 24 + t * 8), offset = READ_BE_UINT32(data + 28 + t * 8), prev = 0;
		for (uint32 i = 0; i < count; ++i) {
			const byte *event = data + offset + i * 12;
			uint32 tick = READ_BE_UINT32(event), arg = READ_BE_UINT32(event + 8);
			uint16 op = READ_BE_UINT16(event + 4), n = READ_BE_UINT16(event + 6);
			if (tick < prev || ((op == 0xff2f) != (i + 1 == count))) return false;
			prev = tick;
			if (op >= 0x80 && op < 0xf0) {
				uint16 expected = (op >> 4 == 12 || op >> 4 == 13) ? 1 : 2;
				if (n != expected || arg >= (1u << (8 * n))) return false;
				for (uint16 k = 0; k < n; ++k) if (event[12 - n + k] >= 128) return false;
			} else {
				if (arg < eventEnd || !region(size, arg, n)) return false;
				uint32 expanded = 0;
				if (nibbled(op)) {
					if (n < minimumIMuseArgs(op) || n > 128 || data[arg] >= 128) return false;
					expanded = 2u * n + 2;
				} else if (rawIMuse(op)) {
					if (n < minimumIMuseArgs(op)) return false;
					for (uint16 k = 0; k < n; ++k) if (data[arg + k] >= 128) return false;
					expanded = n + 3u;
				}
				else if (op == 0x4100) {
					if (n != 247 || data[arg] >= 16) return false;
					for (uint16 k = 0; k < n; ++k) if (data[arg + k] >= 128) return false;
					expanded = 255;
				} else if (op >> 8 == 0xff) {
					if (op == 0xff2f && n) return false;
					if (op == 0xff51 && (n != 3 || !(data[arg] || data[arg + 1] || data[arg + 2]))) return false;
					if (op == 0xff51) {
						uint32 tempo = (uint32(data[arg]) << 16) | (uint32(data[arg + 1]) << 8) | data[arg + 2];
						if ((tempo + (ppqn >> 2)) / ppqn == 0) return false;
					}
				} else {
					if (op != 0xf0f0 || n == 0 || n > 269) return false;
					// Known manufacturers must use validated native records. Do
					// not bypass field checks through opaque, malformed messages.
					if (data[arg] == 0x7d || data[arg] == 0x41 || (data[arg] == 0 && n < 3)) return false;
					if (n == 269 && data[arg + n - 1] != 0xf7) return false;
				}
				// MidiDriver supports at most 268 SysEx bytes without terminal F7.
				// Empty SysEx/SMF escapes are outside this game adapter's profile.
				if (expanded > 269 || extra + expanded > 1024 * 1024) return false;
				extra += expanded;
			}
		}
	}
	_data.resize(size + extra);
	memcpy(_data.data(), data, size);
	uint32 next = size;
	for (uint32 t = 0; t < tracks; ++t) {
		uint32 count = READ_BE_UINT32(data + 24 + t * 8), offset = READ_BE_UINT32(data + 28 + t * 8);
		for (uint32 i = 0; i < count; ++i) {
			byte *event = _data.data() + offset + i * 12;
			uint16 op = READ_BE_UINT16(event + 4), n = READ_BE_UINT16(event + 6);
			if (!nibbled(op) && !rawIMuse(op) && op != 0x4100) continue;
			const byte *args = data + READ_BE_UINT32(event + 8);
			uint32 start = next;
			if (op == 0x4100) {
				const byte prefix[] = {0x41, args[0], 0x16, 0x12, 4, 0, 0};
				memcpy(_data.data() + next, prefix, 7); next += 7;
				memcpy(_data.data() + next, args + 1, 246); next += 246;
				uint32 sum = 4;
				for (uint16 k = 1; k < n; ++k) sum += args[k];
				_data[next++] = byte(-sum) & 127;
			} else {
				_data[next++] = 0x7d; _data[next++] = byte(op);
				if (nibbled(op)) {
					_data[next++] = args[0];
					for (uint16 k = 1; k < n; ++k) { _data[next++] = args[k] >> 4; _data[next++] = args[k] & 15; }
				} else { memcpy(_data.data() + next, args, n); next += n; }
			}
			_data[next++] = 0xf7;
			WRITE_BE_UINT16(event + 4, 0xf0f0);
			WRITE_BE_UINT16(event + 6, next - start);
			WRITE_BE_UINT32(event + 8, start);
		}
		_tracks[t][0] = _data.data() + offset;
		_trackEnds[t] = _tracks[t][0] + count * 12;
		_numSubtracks[t] = 1;
	}
	_numTracks = tracks;
	_ppqn = ppqn;
	resetTracking();
	setTempo(500000);
	return setTrack(0);
}

void MidiParser_FCM::parseNextEvent(EventInfo &info) {
	uint8 subtrack = info.subtrack;
	const byte *event = _position._subtracks[subtrack]._playPos;
	info.clear();
	info.start = event;
	if (!event || event >= _trackEnds[_activeTrack]) {
		info.event = 0xff; info.ext.type = 0x2f;
		return;
	}
	// Use adjacent stored ticks, not mutable Tracker timestamps: the base
	// class rebases times during loops and snapshots positions during jumps.
	info.delta = READ_BE_UINT32(event) - (event == _tracks[_activeTrack][0] ? 0 : READ_BE_UINT32(event - 12));
	uint16 op = READ_BE_UINT16(event + 4), n = READ_BE_UINT16(event + 6);
	if (op < 0xf0) {
		info.event = byte(op);
		info.basic.param1 = event[12 - n];
		info.basic.param2 = n == 2 ? event[11] : 0;
		if (info.command() == 9 && !info.basic.param2) info.event = 0x80 | info.channel();
	} else {
		info.event = op == 0xf0f0 ? 0xf0 : 0xff;
		info.ext.type = byte(op);
		info.ext.data = _data.data() + READ_BE_UINT32(event + 8);
		info.length = n;
	}
	_position._subtracks[subtrack]._playPos = event + 12;
}

} // namespace Scumm
