// Score the practical kernel against the exact one, quickly, for one build of
// the practical kernel (one block length, one synthesis rate). ablation-study.py
// builds this once per variant, each against its own generated tables, and
// compares the variants; nothing here changes the production kernel.
//
// The synthetic scenarios and the trace loader are practical-test.cpp's own,
// included below so the two cannot drift apart. What differs is the scoring:
// practical-gate.py checks partials and thresholds in pure Python, minutes a
// run, which does not do for a sweep. This reduces each rendering to
//
//   * band_db:  third-octave band levels in 8192-point windows (0.16 s), the
//               mean and 90th-percentile |level difference| over the bands
//               within 25 dB of the window's loudest, for windows above
//               -60 dBFS. Aliasing, a wrong envelope shape and a missing
//               partial all land here; chaotic feedback does too, which is
//               why the exact kernel against itself is the floor to compare.
//   * hf_db:    the signed mean difference of the bands above 8 kHz, so
//               aliasing excess reads positive and a dull sound negative.
//   * level_db: the signed mean difference of the windows' total level.
//   * env_db:   the mean |difference| of the 20 ms RMS envelope where the
//               reference is above -60 dBFS: attack and release shape, and
//               how early or late a note starts.
//   * env_corr: the correlation of the two 20 ms envelopes.
//   * env2_db, env2_corr: the same at 2 ms, which resolves a block (0.65 ms).
//
// usage: opl-ablation-test [--blocks] [--only name,name] [--trace file] [--seconds N]
//                          [--rhythm-trace file] [--rhythm-from S]
// --dump <dir> keeps both renderings as raw 16-bit PCM. --only takes whole scenario
// names. ABL_DRUM_MASK=<0..0x1f> keeps only those drums' keys (1 hi-hat, 2 cymbal,
// 4 tom-tom, 8 snare, 0x10 bass) in the traces; ABL_VERBOSE=1 prints every graded band.
// The cp-* scenarios are one Cruise instrument with a feature taken away each.
// --blocks applies a write at the boundary of its block, as the DSP does,
// rather than at its own frame.
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#define main practical_test_main
#include "practical-test.cpp"
#undef main

#include <algorithm>
#include <cmath>
#include <complex>

namespace {

const int kWindow = 8192;
// Bands this far below the window's loudest are not graded. A partial a few Hz from a
// fixed band edge puts its main-lobe tail into the neighbour, 15 to 40 dB down, and a
// 0.1 Hz difference in pitch moves that by several dB: noise, not sound.
const double kGradeDb = 25.0;
const double kEnvelopeS = 0.020;
const double kFineEnvelopeS = 0.002;   // resolves a 0.65 ms block, which the 20 ms one cannot   // 5 ms is shorter than the beat of detuned partials in a chord

void fft(std::vector<std::complex<double> > &a) {
	const size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; ++i) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(a[i], a[j]);
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const double angle = -2.0 * M_PI / (double)len;
		const std::complex<double> step(std::cos(angle), std::sin(angle));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w(1.0, 0.0);
			for (size_t k = 0; k < len / 2; ++k) {
				const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
				a[i + k] = u + v;
				a[i + k + len / 2] = u - v;
				w *= step;
			}
		}
	}
}

// Third-octave bands from 25 Hz to 16 kHz: centre 1000 * 2^(k/3).
const int kBandFirst = -16, kBandLast = 13;
const int kBands = kBandLast - kBandFirst + 1;

double bandCentre(int index) { return 1000.0 * std::pow(2.0, (kBandFirst + index) / 3.0); }

struct Window {
	double start;       // seconds
	double total_db;    // dBFS of the whole window
	double band_db[kBands];
};

// 4-term Blackman-Harris: sidelobes below -92 dB. A Hann window's skirts, 30 to
// 60 dB below a partial, fill the bands that hold none, and move with the
// partial's position against the bins: scored as signal, that is a 1 dB
// "error" between two renderings whose sound is the same.
std::vector<double> hann() {
	std::vector<double> w(kWindow);
	for (int i = 0; i < kWindow; ++i) {
		const double x = 2.0 * M_PI * i / (kWindow - 1);
		w[i] = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
	}
	return w;
}

std::vector<Window> analyze(const std::vector<int16> &pcm, double rate, double hopSeconds) {
	static const std::vector<double> window = hann();
	std::vector<Window> out;
	double windowEnergy = 0.0;
	for (int i = 0; i < kWindow; ++i)
		windowEnergy += window[i] * window[i];
	const double powerNorm = 2.0 / ((double)kWindow * windowEnergy) / (32768.0 * 32768.0);   // a full-scale sine reads -3 dB
	for (double t = 0.0;; t += hopSeconds) {
		const size_t start = (size_t)(t * rate);
		if (start + kWindow > pcm.size())
			break;
		std::vector<std::complex<double> > a(kWindow);
		double energy = 0.0;
		for (int i = 0; i < kWindow; ++i) {
			a[i] = pcm[start + i] * window[i];
			energy += (double)pcm[start + i] * pcm[start + i];
		}
		fft(a);
		Window w;
		w.start = t;
		w.total_db = 10.0 * std::log10(energy / kWindow / (32768.0 * 32768.0) + 1e-18);
		for (int b = 0; b < kBands; ++b)
			w.band_db[b] = 0.0;
		double power[kBands] = {};
		for (int k = 1; k < kWindow / 2; ++k) {
			const double hz = k * rate / kWindow;
			if (hz < bandCentre(0) / std::pow(2.0, 1.0 / 6.0))
				continue;
			// Triangular weights between the two nearest band centres, in log
			// frequency. A hard edge would hand a whole bin to one band, and the
			// two renderings' bins (rate / 8192 apart) fall at different
			// frequencies: a partial on an edge then flips bands with the rate.
			const double position = 3.0 * std::log2(hz / 1000.0) - kBandFirst;
			const int band = (int)std::floor(position);
			if (band < -1 || band >= kBands)
				continue;
			const double upper = position - band, p = std::norm(a[k]) * powerNorm;
			if (band >= 0)
				power[band] += (1.0 - upper) * p;
			if (band + 1 < kBands)
				power[band + 1] += upper * p;
		}
		for (int b = 0; b < kBands; ++b)
			w.band_db[b] = 10.0 * std::log10(power[b] + 1e-18);
		out.push_back(w);
	}
	return out;
}

std::vector<double> envelope(const std::vector<int16> &pcm, double rate, double seconds, double step) {
	const int n = (int)(seconds / step);
	std::vector<double> env(n, 0.0);
	for (int i = 0; i < n; ++i) {
		const size_t from = (size_t)(i * step * rate), to = std::min(pcm.size(), (size_t)((i + 1) * step * rate));
		double sum = 0.0;
		for (size_t s = from; s < to; ++s)
			sum += (double)pcm[s] * pcm[s];
		env[i] = to > from ? std::sqrt(sum / (to - from)) / 32768.0 : 0.0;
	}
	return env;
}

void envelopeScore(const std::vector<double> &ea, const std::vector<double> &eb, double *db, double *corr) {
	double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, dbSum = 0.0;
	int dbCount = 0;
	const size_t m = std::min(ea.size(), eb.size());
	for (size_t i = 0; i < m; ++i) {
		sa += ea[i]; sb += eb[i]; saa += ea[i] * ea[i]; sbb += eb[i] * eb[i]; sab += ea[i] * eb[i];
		if (ea[i] > 0.001) {
			dbSum += std::fabs(20.0 * std::log10((eb[i] + 1e-6) / ea[i]));
			++dbCount;
		}
	}
	const double den = std::sqrt((m * saa - sa * sa) * (m * sbb - sb * sb));
	*corr = den > 0.0 ? (m * sab - sa * sb) / den : 1.0;
	*db = dbCount ? dbSum / dbCount : 0.0;
}

struct Score {
	double bandMean, bandLo, bandHi, bandP90, hf, level, envDb, envCorr, fineDb, fineCorr;
	int windows;
};

Score compare(const std::vector<int16> &ref, double refRate, const std::vector<int16> &cand, double candRate, double seconds) {
	Score s = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	const double hop = 0.25;
	const std::vector<Window> a = analyze(ref, refRate, hop), b = analyze(cand, candRate, hop);
	std::vector<double> diffs;
	double hfSum = 0.0, levelSum = 0.0, loSum = 0.0, hiSum = 0.0;
	int hfCount = 0, loCount = 0;
	const size_t n = std::min(a.size(), b.size());
	for (size_t w = 0; w < n; ++w) {
		if (a[w].total_db < -60.0)
			continue;
		double top = -200.0;
		for (int k = 0; k < kBands; ++k)
			top = std::max(top, a[w].band_db[k]);
		++s.windows;
		if (std::getenv("ABL_VERBOSE")) {
			std::fprintf(stderr, "t=%.2f total %+.1f/%+.1f dB\n", a[w].start, a[w].total_db, b[w].total_db);
			for (int k = 0; k < kBands; ++k)
				if (a[w].band_db[k] > top - kGradeDb)
					std::fprintf(stderr, "   %7.0f Hz  exact %7.2f  variant %7.2f  diff %+.2f\n", bandCentre(k), a[w].band_db[k], b[w].band_db[k], b[w].band_db[k] - a[w].band_db[k]);
		}
		levelSum += b[w].total_db - a[w].total_db;
		for (int k = 0; k < kBands; ++k) {
			if (a[w].band_db[k] < top - kGradeDb)
				continue;
			const double d = b[w].band_db[k] - a[w].band_db[k];
			diffs.push_back(std::fabs(d));
			if (bandCentre(k) >= 8000.0) {
				hfSum += d;
				hiSum += std::fabs(d);
				++hfCount;
			} else {
				loSum += std::fabs(d);
				++loCount;
			}
		}
	}
	if (!diffs.empty()) {
		double sum = 0.0;
		for (size_t i = 0; i < diffs.size(); ++i)
			sum += diffs[i];
		s.bandMean = sum / diffs.size();
		std::sort(diffs.begin(), diffs.end());
		s.bandP90 = diffs[(size_t)(0.9 * (diffs.size() - 1))];
	}
	s.bandLo = loCount ? loSum / loCount : 0.0;
	s.bandHi = hfCount ? hiSum / hfCount : 0.0;
	s.hf = hfCount ? hfSum / hfCount : 0.0;
	s.level = s.windows ? levelSum / s.windows : 0.0;

	envelopeScore(envelope(ref, refRate, seconds, kEnvelopeS), envelope(cand, candRate, seconds, kEnvelopeS),
	              &s.envDb, &s.envCorr);
	envelopeScore(envelope(ref, refRate, seconds, kFineEnvelopeS), envelope(cand, candRate, seconds, kFineEnvelopeS),
	              &s.fineDb, &s.fineCorr);
	return s;
}

// The practical kernel with writes applied at the boundary of the block they
// fall in (the DSP's behaviour), or at their own frame as practical-test does.
std::vector<int16> renderBlocks(const Script &s) {
	OplPractical::Chip chip;
	OplPractical::reset(&chip, 9);
	OplPractical::DirectSink sink(&chip);
	OplPractical::Decoder decoder;
	decoder.reset(&sink, 9);
	const uint64 blocks = ((uint64)(s.seconds * kCodecRate) + OplPractical::kBlockFrames - 1) / OplPractical::kBlockFrames;
	std::vector<int16> pcm;
	pcm.reserve(blocks * OplPractical::kBlockFrames);
	size_t next = 0;
	int32 out[OplPractical::kBlockFrames];
	int32 right[OplPractical::kBlockFrames];
	for (uint64 block = 0; block < blocks; ++block) {
		const uint64 start = block * OplPractical::kBlockFrames, end = start + OplPractical::kBlockFrames;
		while (next < s.events.size() && (uint64)(s.events[next].seconds * kCodecRate) < end) {
			decoder.write((uint32)start, s.events[next].reg, s.events[next].value);
			++next;
		}
		decoder.flush();
		OplPractical::renderSpanStereo(&chip, nullptr, out, right, OplPractical::kBlockFrames, true);
		for (int i = 0; i < OplPractical::kBlockFrames; ++i)
			pcm.push_back((int16)(out[i] >> 8));
	}
	return pcm;
}


// Cruise for a Corpse's melodic instrument in the stretch where the two kernels
// part in the 8 kHz bands (feedback 4, shallow tremolo on both operators), and
// versions of it with one thing taken away, to find which one the bias needs.
Script cruisePatch(const char *name, uint8 operatorFlags, uint8 feedbackConnection, uint8 modLevel, uint8 modSr) {
	Script s(name);
	s.write(0x01, 0x20);
	s.write(0xbd, 0x00);
	const uint8 mod = kModOffset[1], car = mod + 3;
	s.write(0x20 + mod, operatorFlags); s.write(0x20 + car, operatorFlags);
	s.write(0x40 + mod, modLevel);       s.write(0x40 + car, 0x05);
	s.write(0x60 + mod, 0x85);           s.write(0x60 + car, 0x85);
	s.write(0x80 + mod, modSr);          s.write(0x80 + car, 0x03);
	s.write(0xe0 + mod, 0x00);           s.write(0xe0 + car, 0x00);
	s.write(0xc1, feedbackConnection);
	static const struct { uint16 fnum; uint8 block; } notes[4] = { { 0x1e6, 3 }, { 0x181, 3 }, { 0x2aa, 2 }, { 0x241, 4 } };
	for (int i = 0; i < 4; ++i) {
		s.write(0xa1, (uint8)(notes[i].fnum & 0xff));
		s.write(0xb1, (uint8)(0x20 | (notes[i].block << 2) | (notes[i].fnum >> 8)));
		s.wait(1.2);
		s.write(0xb1, (uint8)((notes[i].block << 2) | (notes[i].fnum >> 8)));
		s.wait(0.4);
	}
	s.finish(0.0);
	return s;
}

} // namespace

int main(int argc, char **argv) {
	const char *trace = nullptr, *rhythmTrace = nullptr, *only = nullptr, *dump = nullptr;
	double seconds = 60.0, rhythmFrom = 0.0;
	bool blocks = false;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--trace") && i + 1 < argc)
			trace = argv[++i];
		else if (!std::strcmp(argv[i], "--rhythm-trace") && i + 1 < argc)
			rhythmTrace = argv[++i];
		else if (!std::strcmp(argv[i], "--rhythm-from") && i + 1 < argc)
			rhythmFrom = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--only") && i + 1 < argc)
			only = argv[++i];
		else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc)
			dump = argv[++i];
		else if (!std::strcmp(argv[i], "--blocks"))
			blocks = true;
	}
	std::vector<Script> all = scenarios();
	all.push_back(cruisePatch("cp-full", 0x80, 0x08, 0x09, 0x24));
	all.push_back(cruisePatch("cp-no-tremolo", 0x00, 0x08, 0x09, 0x24));
	all.push_back(cruisePatch("cp-no-feedback", 0x80, 0x00, 0x09, 0x24));
	all.push_back(cruisePatch("cp-loud-modulator", 0x80, 0x08, 0x00, 0x24));
	all.push_back(cruisePatch("cp-feedback-7", 0x80, 0x0e, 0x09, 0x24));
	const char *mask = std::getenv("ABL_DRUM_MASK");   // keep only these drums of register 0xbd (bit mask 0x1f)
	if (trace) {
		Script s("atlantis");
		if (!loadTrace(trace, seconds, s)) {
			std::fprintf(stderr, "cannot read trace %s\n", trace);
			return 1;
		}
		all.push_back(s);
	}
	if (rhythmTrace) {
		Script s("cruise");
		if (!loadTrace(rhythmTrace, seconds, s, rhythmFrom)) {
			std::fprintf(stderr, "cannot read trace %s\n", rhythmTrace);
			return 1;
		}
		if (mask) {
			const uint8 keep = (uint8)(0xe0 | (std::strtoul(mask, nullptr, 0) & 0x1f));
			for (size_t e = 0; e < s.events.size(); ++e)
				if (s.events[e].reg == 0xbd)
					s.events[e].value &= keep;
		}
		all.push_back(s);
	}
	std::printf("[\n");
	bool first = true;
	for (size_t i = 0; i < all.size(); ++i) {
		const Script &s = all[i];
		if (only && (std::string(",") + only + ",").find("," + s.name + ",") == std::string::npos)
			continue;
		const std::vector<int16> exact = renderExact(s);
		unsigned long long writes = 0;
		Lead lead;
		const std::vector<int16> practical = blocks ? renderBlocks(s) : renderPractical(s, &writes, &lead);
		if (dump) {
			writePcm(std::string(dump) + "/" + s.name + "-exact.pcm", exact);
			writePcm(std::string(dump) + "/" + s.name + "-variant.pcm", practical);
		}
		const Score sc = compare(exact, kNativeRate, practical, kCodecRate, s.seconds);
		std::printf("%s  {\"name\": \"%s\", \"windows\": %d, \"band_db\": %.3f, \"band_lo_db\": %.3f, \"band_hi_db\": %.3f, \"band_p90_db\": %.3f,"
		            " \"hf_db\": %.3f, \"level_db\": %.3f, \"env_db\": %.3f, \"env_corr\": %.5f, \"env2_db\": %.3f, \"env2_corr\": %.5f}",
		            first ? "" : ",\n", s.name.c_str(), sc.windows, sc.bandMean, sc.bandLo, sc.bandHi, sc.bandP90, sc.hf, sc.level,
		            sc.envDb, sc.envCorr, sc.fineDb, sc.fineCorr);
		first = false;
	}
	std::printf("\n]\n");
	return 0;
}
