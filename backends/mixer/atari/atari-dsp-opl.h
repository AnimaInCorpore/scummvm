/* ScummVM - Graphic Adventure Engine
 *
 * Copyright (C) 2026 Sascha Springer
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BACKENDS_MIXER_ATARI_DSP_OPL_H
#define BACKENDS_MIXER_ATARI_DSP_OPL_H

#include "audio/fmopl.h"

class AtariMixerManager;
namespace Audio { class MixerImpl; }

namespace OPL {
namespace FalconDsp {

/** OPL2 synthesis and mixed PCM share the Falcon DSP's SSI output. */
class OPL final : public ::OPL::OPL {
public:
	explicit OPL(AtariMixerManager *mixer);
	~OPL() override;
	bool init() override;
	void reset() override;
	void write(int port, int value) override;
	void writeReg(int reg, int value) override;
	void setCallbackFrequency(int frequency) override;

	// Called by the Atari mixer, outside interrupt context.
	bool open(int inputRate, bool stereo);
	void close(bool drain = true);
	bool update(Audio::MixerImpl *mixer);
	void suspend(bool suspended);

protected:
	void startCallbacks(int frequency) override;
	void stopCallbacks() override;

private:
	struct State;
	State *_state;
	AtariMixerManager *_mixer;
	int _address;
	int _callbackFrequency;
	uint32 _callbackPhase;
	bool _callbacksRunning;
	bool _suspended;
};

::OPL::OPL *create(Config::OplType type);

} // namespace FalconDsp
} // namespace OPL

#endif
