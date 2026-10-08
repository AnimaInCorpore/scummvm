// The DSP kernel's table and record image, as upload blocks.
//
// rt-fixture.cpp writes the same blocks, in the same order, into OPLDATA.BIN
// for the bench and stream gates; the MIDI tools use this header so that the
// host writer and the Falcon player, which sends the blocks to the DSP
// directly, cannot drift apart. midi-gate.py compares the host file's block
// section with the fixture's.
//
// Out must provide block(int space, uint16_t address, const uint32_t *words,
// unsigned count), with space 0 for X and 1 for Y.
#ifndef OPL_UPLOAD_H
#define OPL_UPLOAD_H

#include <stdint.h>

#include "opl-kernel.h"
#include "opl-practical.h"

namespace FalconOplTables {

enum { kUploadBlocks = 17, kUploadBlocksWithoutOpl3 = 16 };

// opl3Waves: the eight-waveform tables above Y:$2800. An OPL2 never selects
// them, so a player may leave them out and save 4,096 words of upload.
template <class Out>
void uploadTables(Out &out, int channelCount, bool opl3Waves) {
	namespace P = OplPractical;
	static uint32_t w[4 * 1024];

	w[0] = 4; w[1] = 0; w[2] = (uint32_t)channelCount; w[3] = 0x7fffff;
	out.block(0, P::SC_TREMOLO_SHIFT, w, 4);

	out.block(0, P::kGainTable, kOplGain, 512);

	P::Chip chip;
	P::reset(&chip, channelCount);
	for (int i = 0; i < P::kSlots; ++i)
		for (int k = 0; k < P::kOpStride; ++k)
			w[i * P::kOpStride + k] = (uint32_t)chip.op[i].w[k] & 0xffffff;
	out.block(0, P::kOpBase, w, P::kSlots * P::kOpStride);
	for (int i = 0; i < P::kSlots * P::kOpStride; ++i)   // the phase fractions
		w[i] = 0;
	out.block(1, P::kOpBase, w, P::kSlots * P::kOpStride);
	for (int i = 0; i < P::kChannels * P::kChannelStride; ++i)
		w[i] = 0;
	out.block(0, P::kChannelBase, w, P::kChannels * P::kChannelStride);

	out.block(0, P::kAttackTable, kOplAttackBlock, 64);
	out.block(0, P::kDecayTable, kOplDecayBlock, 64);
	for (int i = 0; i < 8; ++i)
		w[i] = (uint32_t)P::kVibratoOffset[i];
	out.block(0, P::kVibratoTable, w, 8);
	w[0] = P::kMuteRing; w[1] = P::kLeftRing; w[2] = P::kRightRing; w[3] = 0;
	out.block(0, P::kRouteTable, w, 4);

	for (int wf = 0; wf < 4; ++wf)
		for (uint16_t phase = 0; phase < 1024; ++phase)
			w[wf * 1024 + phase] = (uint32_t)P::waveSample((uint8_t)wf, phase) & 0xffffff;
	out.block(1, P::kWaveBase, w, 4 * 1024);
	if (opl3Waves) {
		for (int wf = 4; wf < P::kWaveforms; ++wf)
			for (uint16_t phase = 0; phase < 1024; ++phase)
				w[(wf - 4) * 1024 + phase] = (uint32_t)P::waveSample((uint8_t)wf, phase) & 0xffffff;
		out.block(1, P::kOpl3WaveBase, w, 4 * 1024);
	}

	out.block(1, P::kNoiseJump, kOplNoiseJump, 768);
	out.block(1, P::kNoisePowers, kOplNoisePowers, 529);

	for (uint16_t phase = 0; phase < 1024; ++phase)
		w[phase] = (uint32_t)P::rhythmHiHatRow(phase);
	out.block(0, P::kRhythmHiHat, w, 1024);
	for (uint16_t phase = 0; phase < 1024; ++phase)
		w[phase] = (uint32_t)P::rhythmCymbalColumn(phase);
	out.block(1, P::kRhythmCymbal, w, 1024);
	for (int i = 0; i < 32; ++i)
		w[i] = (uint32_t)P::rhythmSelect(i);
	out.block(0, P::kRhythmSelect, w, 32);
	for (int i = 0; i < 12; ++i)
		w[i] = (uint32_t)P::rhythmPhase(i);
	out.block(0, P::kRhythmPhases, w, 12);
}

} // namespace FalconOplTables

#endif
