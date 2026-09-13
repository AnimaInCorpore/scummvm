#include <cxxtest/TestSuite.h>

#include "audio/audiostream.h"
#include "audio/mixer.h"
#include "audio/rate.h"

/**
 * A stream of consecutive integers, so that every sample in the output can be
 * traced back to the position it was read from. Sample numbering starts at 1,
 * so input frame f of a stereo stream holds the pair (2f + 1, 2f + 2).
 */
class CountingAudioStream : public Audio::AudioStream {
public:
	CountingAudioStream(int rate, bool stereo) : _rate(rate), _stereo(stereo), _pos(0) {}

	int readBuffer(int16 *buffer, const int numSamples) override {
		for (int i = 0; i < numSamples; ++i)
			buffer[i] = (int16)(++_pos);
		return numSamples;
	}

	bool isStereo() const override { return _stereo; }
	int getRate() const override { return _rate; }
	bool endOfData() const override { return false; }

private:
	int _rate;
	bool _stereo;
	int _pos;
};

class RateTestSuite : public CxxTest::TestSuite {
public:
	// Exercise cached input across uneven callbacks, signed samples, volume
	// changes and EOF. The first callback deliberately leaves most of a read
	// in the converter; muting must still consume that input.
	void test_copy_partitioned_signed_eof() {
		class FiniteStream : public Audio::AudioStream {
		public:
			FiniteStream(bool stereo) : _stereo(stereo), _pos(0) {}
			int readBuffer(int16 *buffer, int count) override {
				count = MIN(count, 1030 * (_stereo ? 2 : 1) - _pos);
				for (int i = 0; i < count; ++i, ++_pos)
					buffer[i] = sample(_pos);
				return count;
			}
			static int16 sample(int pos) { return (pos % 2) ? -30000 + pos : 30000 - pos; }
			bool isStereo() const override { return _stereo; }
			int getRate() const override { return 49170; }
			bool endOfData() const override { return _pos == 1030 * (_stereo ? 2 : 1); }
			bool _stereo;
			int _pos;
		};
		const int requests[] = { 3, 513, 17, 509, 1 };
		const int volumes[][2] = { {256, 256}, {128, 256}, {0, 0}, {256, 0}, {0, 256} };
		for (int inStereo = 0; inStereo < 2; ++inStereo) {
			for (int outStereo = 0; outStereo < 2; ++outStereo) {
				for (int reverse = 0; reverse <= (inStereo && outStereo); ++reverse) {
					FiniteStream input(inStereo);
					Audio::RateConverter *converter = Audio::makeRateConverter(49170, 49170, inStereo, outStereo, reverse);
					int position = 0;
					for (int call = 0; call < 5; ++call) {
						int32 out[1026];
						for (int i = 0; i < 1026; ++i)
							out[i] = 40000; // Preserve 32-bit accumulation above PCM16 range.
						const int frames = MIN(requests[call], 1030 - position);
						TS_ASSERT_EQUALS(converter->convert(input, (byte *)out, sizeof(int32), requests[call],
							volumes[call][0], volumes[call][1], Audio::MIX_ADD), frames);
						for (int i = 0; i < frames; ++i) {
							const int left = FiniteStream::sample((position + i) * (inStereo ? 2 : 1)) * volumes[call][0] / 256;
							const int right = FiniteStream::sample((position + i) * (inStereo ? 2 : 1) + inStereo) * volumes[call][1] / 256;
							if (outStereo) {
								TS_ASSERT_EQUALS(out[2 * i + reverse], 40000 + left);
								TS_ASSERT_EQUALS(out[2 * i + (reverse ^ 1)], 40000 + right);
							} else {
								TS_ASSERT_EQUALS(out[i], 40000 + (left + right) / 2);
							}
						}
						for (int i = frames * (outStereo ? 2 : 1); i < 1026; ++i)
							TS_ASSERT_EQUALS(out[i], 40000);
						position += frames;
					}
					TS_ASSERT(!converter->needsDraining());
					delete converter;
				}
			}
		}
	}

	/**
	 * When the output rate is an exact multiple of the input rate, every input
	 * frame is written out `factor` times in a row. A request whose frame count
	 * is not a multiple of `factor` leaves such a group half written, and the
	 * converter has to finish it on the next call rather than stall.
	 */
	void test_upsample_partial_group() {
		// 11025 Hz -> 44100 Hz, i.e. every input frame is repeated 4 times.
		Audio::RateConverter *converter = Audio::makeRateConverter(11025, 44100, true, true, false);
		CountingAudioStream input(11025, true);

		// 1882 = 4 * 470 + 2 and 1881 = 4 * 470 + 1, so both requests end in the
		// middle of a group. These are the frame counts SDL3 asks for on the
		// Emscripten backend, where the browser audio device runs at 48000 Hz.
		const int requests[3] = { 1882, 1881, 1882 };
		const int totalFrames = 1882 + 1881 + 1882;

		int16 *out = new int16[totalFrames * 2]();
		int16 *pos = out;

		for (int i = 0; i < 3; ++i) {
			const int written = converter->convert(input, (byte *)pos, sizeof(int16),
				requests[i], Audio::Mixer::kMaxMixerVolume, Audio::Mixer::kMaxMixerVolume,
				Audio::MIX_ADD);
			TS_ASSERT_EQUALS(written, requests[i]);
			pos += requests[i] * 2;
		}

		// Output frame k must be a copy of input frame k / 4, across call boundaries.
		for (int k = 0; k < totalFrames; ++k) {
			TS_ASSERT_EQUALS(out[k * 2 + 0], (int16)(2 * (k / 4) + 1));
			TS_ASSERT_EQUALS(out[k * 2 + 1], (int16)(2 * (k / 4) + 2));
		}

		// totalFrames % 4 != 0, so a group is still open.
		TS_ASSERT_EQUALS(converter->needsDraining(), true);

		delete[] out;
		delete converter;
	}

	/**
	 * The same, for a mono stream upsampled into a stereo buffer.
	 */
	void test_upsample_partial_group_mono() {
		Audio::RateConverter *converter = Audio::makeRateConverter(11025, 22050, false, true, false);
		CountingAudioStream input(11025, false);

		// 7 = 2 * 3 + 1, so the last group is cut in half.
		int16 out[7 * 2] = {};
		const int written = converter->convert(input, (byte *)out, sizeof(int16), 7,
			Audio::Mixer::kMaxMixerVolume, Audio::Mixer::kMaxMixerVolume, Audio::MIX_ADD);

		TS_ASSERT_EQUALS(written, 7);
		for (int k = 0; k < 7; ++k) {
			TS_ASSERT_EQUALS(out[k * 2 + 0], (int16)(k / 2 + 1));
			TS_ASSERT_EQUALS(out[k * 2 + 1], (int16)(k / 2 + 1));
		}

		delete converter;
	}

	/**
	 * The field configuration of the SegaCD Monkey Island 1 hang on OpenBSD:
	 * an 11025 Hz mono sound effect upsampled into a 44100 Hz stereo mixer at
	 * non-maximum volume with clamped mixing, in sndio-sized blocks of 882
	 * frames. 882 = 4 * 220 + 2, so every request ends in the middle of a
	 * group.
	 */
	void test_upsample_partial_group_sndio_blocks() {
		Audio::RateConverter *converter = Audio::makeRateConverter(11025, 44100, false, true, false);
		CountingAudioStream input(11025, false);

		const int frames = 882;
		const int totalFrames = 3 * frames;
		const uint16 volume = 192;

		int16 *out = new int16[totalFrames * 2]();
		int16 *pos = out;

		for (int i = 0; i < 3; ++i) {
			const int written = converter->convert(input, (byte *)pos, sizeof(int16),
				frames, volume, volume, Audio::MIX_CLAMPED_ADD);
			TS_ASSERT_EQUALS(written, frames);
			pos += frames * 2;
		}

		// Output frame k must be a volume-scaled copy of input frame k / 4,
		// across call boundaries.
		for (int k = 0; k < totalFrames; ++k) {
			const int16 expected = (int16)(((k / 4 + 1) * volume) / Audio::Mixer::kMaxMixerVolume);
			TS_ASSERT_EQUALS(out[k * 2 + 0], expected);
			TS_ASSERT_EQUALS(out[k * 2 + 1], expected);
		}

		// totalFrames % 4 != 0, so a group is still open.
		TS_ASSERT_EQUALS(converter->needsDraining(), true);

		delete[] out;
		delete converter;
	}

	/**
	 * A 1:1 conversion never groups, and must not hold anything back.
	 */
	void test_copy() {
		Audio::RateConverter *converter = Audio::makeRateConverter(22050, 22050, true, true, false);
		CountingAudioStream input(22050, true);

		int16 out[5 * 2] = {};
		const int written = converter->convert(input, (byte *)out, sizeof(int16), 5,
			Audio::Mixer::kMaxMixerVolume, Audio::Mixer::kMaxMixerVolume, Audio::MIX_ADD);

		TS_ASSERT_EQUALS(written, 5);
		for (int i = 0; i < 10; ++i)
			TS_ASSERT_EQUALS(out[i], (int16)(i + 1));

		delete converter;
	}

	void test_copy_clamped_reversed_stereo() {
		CountingAudioStream input(49170, true);
		Audio::RateConverter *converter = Audio::makeRateConverter(49170, 49170, true, true, true);
		int16 out[600 * 2];
		for (int i = 0; i < 1200; ++i)
			out[i] = 32000;
		TS_ASSERT_EQUALS(converter->convert(input, (byte *)out, sizeof(int16), 600,
			128, 256, Audio::MIX_CLAMPED_ADD), 600);
		for (int frame = 0; frame < 600; ++frame) {
			TS_ASSERT_EQUALS(out[frame * 2], MIN(32767, 32000 + frame * 2 + 2));
			TS_ASSERT_EQUALS(out[frame * 2 + 1], MIN(32767, 32000 + (frame * 2 + 1) / 2));
		}
		delete converter;
	}
};
