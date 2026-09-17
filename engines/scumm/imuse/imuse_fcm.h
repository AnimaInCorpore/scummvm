// ScummVM experimental FCM1 adapter. SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SCUMM_IMUSE_FCM_H
#define SCUMM_IMUSE_FCM_H

#include "audio/midiparser.h"
#include "common/array.h"
#include "common/path.h"

namespace Scumm {

// Score-only loader. Synthesis sections are not consumed by this adapter.
// One immutable SCOR cache belongs to the iMUSE engine, not to a global singleton.
class FCMScore {
	Common::Array<byte> _score;
public:
	bool open(const Common::Path &path);
	bool load(Common::SeekableReadStream &stream);
	bool isLoaded() const { return !_score.empty(); }
	const byte *cue(uint16 sound, uint32 &size) const;
};

// Preserve MidiParser's timing, smart jumps, active-note handling and rollback.
// Own immutable event payloads: a scratch SysEx buffer would invalidate the
// EventInfo snapshots made by jumpToTick and hangAllActiveNotes.
class MidiParser_FCM : public MidiParser {
	Common::Array<byte> _data;
	const byte *_trackEnds[MAXIMUM_TRACKS];
	void parseNextEvent(EventInfo &info) override;
public:
	MidiParser_FCM();
	~MidiParser_FCM() override;
	bool loadMusic(const byte *data, uint32 size) override;
	void unloadMusic() override;
	uint32 storageBytes() const { return _data.size(); }
};

} // namespace Scumm
#endif
