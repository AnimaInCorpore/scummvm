// Rewrites every 22,050 Hz SOL audio resource in a SCI 1.1 audio volume
// (RESOURCE.AUD / RESOURCE.SFX) as uncompressed unsigned 8-bit at 11,025 Hz,
// band-limited below the new Nyquist, in place.
//
// Why: the Falcon mixer runs at 12,292 Hz and ScummVM's rate converter has no
// anti-alias filter, so 22 kHz speech aliases ("blurred"). A DPCM8 record of n
// bytes holds 2n samples; halved and stored raw it is n bytes again, so every
// record keeps its offset and size, and the audio map stays valid. A raw 8-bit
// record shrinks to half and keeps its offset; the rest of its slot is unused.
//
// Decoding mirrors engines/sci/sound/decoders/sol.cpp for SCI < 2.1 mono
// (SOLStream<false, false, true>, "OLD" DPCM8 table order, no popfix).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int8_t tableDPCM8[16] = {0, 1, 2, 3, 6, 10, 15, 21, -21, -15, -10, -6, -3, -2, -1, -0};

enum { kCompressed = 1, k16Bit = 4, kStereo = 16, kTaps = 95 };

static double fir[kTaps];

static void makeFilter(void) {
	// Windowed sinc, cutoff 5,000 Hz at 22,050 Hz (new Nyquist 5,512 Hz), Blackman window.
	const double fc = 5000.0 / 22050.0;
	const int m = kTaps - 1;
	double sum = 0;
	for (int i = 0; i < kTaps; i++) {
		const double x = i - m / 2.0;
		const double s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
		const double w = 0.42 - 0.5 * cos(2 * M_PI * i / m) + 0.08 * cos(4 * M_PI * i / m);
		fir[i] = s * w;
		sum += fir[i];
	}
	for (int i = 0; i < kTaps; i++)
		fir[i] /= sum;
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s in.aud out.aud\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(size);
	if (fread(d, 1, size, f) != (size_t)size) { perror("read"); return 1; }
	fclose(f);
	makeFilter();

	long converted = 0, compressed = 0, raw = 0, skipped = 0, samplesIn = 0;
	float *pcm = NULL;
	long pcmCap = 0;
	for (long p = 2; p + 11 <= size; p++) {
		if (memcmp(d + p, "SOL\0", 4) != 0)
			continue;
		const uint8_t type = d[p - 2], headerSize = d[p - 1];
		if ((type & 0x7f) != 0x0d || (headerSize != 11 && headerSize != 12))
			continue;
		const unsigned rate = d[p + 4] | d[p + 5] << 8;
		const uint8_t flags = d[p + 6];
		const uint32_t dataSize = d[p + 7] | d[p + 8] << 8 | d[p + 9] << 16 | (uint32_t)d[p + 10] << 24;
		const long start = p + headerSize; // resource header (2) + headerSize, from the type byte
		if (start + (long)dataSize > size) {
			skipped++;
			continue;
		}
		if (rate != 22050 || (flags & ~kCompressed)) {
			if (rate > 12292)
				skipped++;
			continue;
		}
		const long n = (flags & kCompressed) ? 2L * dataSize : dataSize;
		if (n > pcmCap) {
			pcmCap = n * 2;
			pcm = realloc(pcm, pcmCap * sizeof(float));
		}
		const uint8_t *src = d + start;
		if (flags & kCompressed) {
			uint8_t sample = 0x80;
			for (uint32_t i = 0; i < dataSize; i++) {
				for (int half = 0; half < 2; half++) {
					const uint8_t delta = half ? src[i] & 0xf : src[i] >> 4;
					const uint8_t last = sample;
					if (delta & 8)
						sample -= tableDPCM8[7 - (delta & 7)];
					else
						sample += tableDPCM8[delta & 7];
					const int16_t out = (int16_t)((uint16_t)((last + sample) << 7) ^ 0x8000);
					pcm[2 * i + half] = out / 32768.0f;
				}
			}
			compressed++;
		} else {
			for (long i = 0; i < n; i++)
				pcm[i] = (src[i] - 128) / 128.0f;
			raw++;
		}
		// Filter and keep every second sample; edges are zero-padded.
		const long nOut = n / 2;
		uint8_t *dst = d + start;
		uint32_t rng = 0x12345678u + (uint32_t)p;
		for (long o = 0; o < nOut; o++) {
			const long c = 2 * o;
			double acc = 0;
			for (int k = 0; k < kTaps; k++) {
				const long j = c + k - (kTaps - 1) / 2;
				if (j >= 0 && j < n)
					acc += fir[k] * pcm[j];
			}
			// TPDF dither of one 8-bit step before quantizing.
			rng = rng * 1664525u + 1013904223u;
			const double r1 = (rng >> 8) / 16777216.0;
			rng = rng * 1664525u + 1013904223u;
			const double r2 = (rng >> 8) / 16777216.0;
			long q = lround(acc * 128.0 + 128.0 + (r1 - r2));
			if (q < 0) q = 0;
			if (q > 255) q = 255;
			dst[o] = (uint8_t)q;
		}
		d[p + 4] = 11025 & 0xff;
		d[p + 5] = 11025 >> 8;
		d[p + 6] = 0;
		d[p + 7] = nOut & 0xff;
		d[p + 8] = (nOut >> 8) & 0xff;
		d[p + 9] = (nOut >> 16) & 0xff;
		d[p + 10] = (nOut >> 24) & 0xff;
		converted++;
		samplesIn += n;
		p = start + dataSize - 1;
	}
	f = fopen(argv[2], "wb");
	if (!f || fwrite(d, 1, size, f) != (size_t)size) { perror(argv[2]); return 1; }
	fclose(f);
	printf("%s: %ld records halved to 11025 Hz (%ld DPCM8, %ld raw), %.1f s of audio; %ld above 12292 Hz left as is\n",
	       argv[1], converted, compressed, raw, samplesIn / 22050.0, skipped);
	return 0;
}
