#!/usr/bin/env python3
"""Which approximation of the practical OPL kernel costs how much sound?

The practical kernel differs from the exact one in several ways at once: it
advances envelopes and both LFOs once a block, it renders at the codec's
49,170 Hz instead of the chip's 49,716 Hz. (Writes are sample-timed: the span
splits at each event. "counterfactual-block-writes" is the earlier behaviour,
where a write took effect at its block's start, up to a block early.) This builds the host kernel once per variant (a block
length and a synthesis rate, each with its own generated tables, in a scratch
directory: the committed tables and sources are not touched), renders the
synthetic scenarios and any captured traces through it and through the exact
kernel, and scores each rendering with ablation-test.cpp's third-octave band,
level and envelope metrics.

Read the table down a column: the step between two rows that differ in one
thing is that thing's cost. The row "native rate, 1 frame" is as close to the
exact kernel as this model can be made, so what it still shows is everything
the sweep does not vary: the table fits, the missing one-sample output delays,
the rhythm noise, and the exact kernel's own chaos under strong feedback.

usage: ablation-study.py --output results.json [--trace opl-writes.ev]
                         [--rhythm-trace ev --rhythm-from S] [--seconds N]
                         [--variants name,name] [--jobs N]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from gate_env import source_sha256

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[3]
CODEC = None            # the committed 49,169.92 Hz
NATIVE = "49716.0"

# name, block frames, FOA_CODEC_RATE, writes at the block boundary, [patch name]
VARIANTS = [
    ("shipped",              32, CODEC,  False),   # what the Falcon path renders
    ("counterfactual-block-writes", 32, CODEC, True),   # the earlier behaviour: a write waits for its block's start
    ("control-block-256",   256, CODEC,  False),   # controls: coarser than anything shipped,
    ("control-block-64",     64, CODEC,  False),   # so the metrics must get worse here
    ("block-16",             16, CODEC,  False),
    ("block-8",               8, CODEC,  False),
    ("block-4",               4, CODEC,  False),
    ("block-2",               2, CODEC,  False),
    ("block-1",               1, CODEC,  False),
    ("native-rate-32",       32, NATIVE, False),   # the rate alone
    ("native-rate-8",         8, NATIVE, False),
    ("native-rate-1",         1, NATIVE, False),   # nearest this model gets to the exact kernel
    # Experiments: the shipped kernel with one change, made to a scratch copy of the header.
    ("hat-previous-cymbal",  32, CODEC,  False, "hat-previous-cymbal"),
    ("native-hat-previous-cymbal", 32, NATIVE, False, "hat-previous-cymbal"),
]

def _between(text, start, end, replacement):
    """Replace text[start:end) by the replacement; the markers must exist exactly once."""
    if text.count(start) != 1 or text.count(end) != 1:
        sys.exit(f"patch marker missing or repeated: {start!r} / {end!r}")
    first = text.index(start)
    return text[:first] + replacement + text[text.index(end):]


def hat_sees_previous_cymbal(text):
    """The chip builds the hi-hat's combined phase bit in slot 13, before the cymbal
    (slot 17) updates its bits, so it reads the cymbal's bits of the previous sample;
    the snare and the cymbal see this sample's. The shipped kernel gives all three the
    same current-sample bit, through one 16-entry table of drum sums. This keeps three
    small tables (hi-hat by noise and combined bit, cymbal by combined bit, snare by
    hi-hat bit 8 and noise) and sums them a frame at a time."""
    text = text.replace("	int32_t drumTable[16];",
                        "	int64_t hatTab[4], cymTab[2], snareTab[4];\n	int32_t cymbalBits;   // the cymbal's bits 3 and 5 of the previous frame\n	int32_t drumTable[16];", 1)
    text = _between(text, "	int64_t pair[4], snare[4];", "}\n\nstatic void blockBoundary",
        """	for (int c = 0; c < 2; ++c) {
		for (int n = 0; n < 2; ++n)
			chip->hatTab[2 * n + c] = doubledProduct(waveSampleOf(*drums[0], rhythmPhase(2 * (2 * n + c))), gains[0]);
		chip->cymTab[c] = doubledProduct(waveSampleOf(*drums[1], rhythmPhase(2 * c + 1)), gains[1]);
	}
	for (int k = 0; k < 4; ++k)
		chip->snareTab[k] = doubledProduct(waveSampleOf(*drums[2], rhythmPhase(8 + k)), gains[2]);
""")
    text = _between(text, "	for (int i = 0; i < chip->frames; ++i) {\n		rowRing[i]", "	hiHat.w[OP_INC] = hiHatInc;",
        """	for (int i = 0; i < chip->frames; ++i) {
		const int32_t hp = phaseIndex(hiHat), cp = phaseIndex(cymbal);
		advance(hiHat);
		advance(cymbal);
		const int32_t hat27 = ((hp >> 2) ^ (hp >> 7)) & 1, hat3 = (hp >> 3) & 1, hat8 = (hp >> 8) & 1;
		const int32_t cy3 = (cp >> 3) & 1, cy5 = (cp >> 5) & 1;
		const int32_t old3 = chip->cymbalBits & 1, old5 = (chip->cymbalBits >> 1) & 1;
		const int32_t hatCombined = hat27 | (hat3 ^ old5) | (old3 ^ old5);
		const int32_t cymCombined = hat27 | (hat3 ^ cy5) | (cy3 ^ cy5);
		chip->cymbalBits = cy3 | (cy5 << 1);
		const int32_t noiseHat = (noiseRing[i] >> 3) & 1, noiseSnare = noiseRing[i] & 1;
		const int64_t sum = chip->hatTab[2 * noiseHat + hatCombined] + chip->cymTab[cymCombined]
		                    + chip->snareTab[2 * hat8 + noiseSnare];
		chip->mixTarget[i] = clamp24((int64_t)chip->mixTarget[i] + clamp24(sum >> 24));
	}
""")
    text = text.replace("	int32_t noiseRing[kBlockFrames], rowRing[kBlockFrames];", "	int32_t noiseRing[kBlockFrames];", 1)
    return text


PATCHES = {"hat-previous-cymbal": hat_sees_previous_cymbal}


SOURCES = ("practical-test.cpp", "ablation-test.cpp", "opl-practical.h", "opl-kernel.h", "opl-tables.h",
           "generate-tables.py")

# Scenario families the summary averages over.
FAMILIES = {
    "single voice": ("tone", "pitch", "envelope", "waveforms", "tremolo", "vibrato"),
    "feedback": ("feedback",),
    "high pitch, aliasing": ("high-pitch",),
    "9-voice polyphony": ("polyphony",),
    "rhythm": ("rhythm", "cruise"),
    "game trace": ("atlantis",),
}


def build(name, block, rate, scratch, sdkroot, patch=None):
    work = scratch / name
    work.mkdir(parents=True)
    for source in SOURCES:
        shutil.copy(HERE / source, work / source)
    if patch:
        header = work / "opl-practical.h"
        header.write_text(PATCHES[patch](header.read_text()))
    env = dict(os.environ, FOA_BLOCK_FRAMES=str(block))
    if rate:
        env["FOA_CODEC_RATE"] = rate
    subprocess.run([sys.executable, str(work / "generate-tables.py"), "--practical-header",
                    str(work / "opl-practical-tables.h")], check=True, env=env, stdout=subprocess.DEVNULL)
    binary = work / "opl-ablation-test"
    command = ["c++", "-std=c++11", "-O2", "-DHAVE_CONFIG_H", f"-I{REPOSITORY}", f"-I{work}",
               f"-I{HERE / 'build/headless'}", "-o", str(binary), str(work / "ablation-test.cpp")]
    if sdkroot:
        env["SDKROOT"] = sdkroot
    done = subprocess.run(command, env=env, capture_output=True, text=True)
    if done.returncode:
        sys.exit(f"{name}: build failed\n{done.stderr[-3000:]}")
    return binary


def run(binary, extra, blocks):
    command = [str(binary)] + extra + (["--blocks"] if blocks else [])
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode:
        sys.exit(f"{binary.parent.name}: run failed\n{done.stderr[-2000:]}")
    return json.loads(done.stdout)


def family_means(scenarios):
    by_name = {s["name"]: s for s in scenarios}
    out = {}
    for family, names in FAMILIES.items():
        rows = [by_name[n] for n in names if n in by_name]
        if not rows:
            continue
        out[family] = {key: sum(r[key] for r in rows) / len(rows)
                       for key in ("band_db", "band_lo_db", "band_hi_db", "band_p90_db", "hf_db", "level_db", "env_db", "env_corr", "env2_db", "env2_corr")}
    return out


def table(results, metric, fmt):
    families = [f for f in FAMILIES if any(f in r["families"] for r in results)]
    lines = ["| variant | " + " | ".join(families) + " |", "|---|" + "---:|" * len(families)]
    for r in results:
        cells = [fmt.format(r["families"][f][metric]) if f in r["families"] else "-" for f in families]
        lines.append(f"| {r['name']} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--rhythm-trace", type=Path)
    parser.add_argument("--rhythm-from", type=float, default=0.0)
    parser.add_argument("--variants", help="comma-separated names to run (default all)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--keep", action="store_true", help="keep the scratch builds")
    args = parser.parse_args()

    sdkroot = os.environ.get("SDKROOT")
    if not sdkroot and sys.platform == "darwin":
        candidate = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk")
        sdkroot = str(candidate) if candidate.is_dir() else None
    extra = ["--seconds", str(args.seconds)]
    if args.trace:
        extra += ["--trace", str(args.trace.resolve())]
    if args.rhythm_trace:
        extra += ["--rhythm-trace", str(args.rhythm_trace.resolve()), "--rhythm-from", str(args.rhythm_from)]
    chosen = set(args.variants.split(",")) if args.variants else None
    variants = [v for v in VARIANTS if not chosen or v[0] in chosen]

    scratch = Path(tempfile.mkdtemp(prefix="foa-ablation-"))

    def one(variant):
        name, block, rate, blocks = variant[:4]
        patch = variant[4] if len(variant) > 4 else None
        binary = build(name, block, rate, scratch, sdkroot, patch)
        scenarios = run(binary, extra, blocks)
        print(f"{name}: done", file=sys.stderr)
        return {"name": name, "block_frames": block, "rate": float(rate) if rate else 49169.921875,
                "writes_at_block_boundary": blocks, "patch": patch, "scenarios": scenarios, "families": family_means(scenarios)}

    try:
        with ThreadPoolExecutor(args.jobs) as pool:
            results = list(pool.map(one, variants))
    finally:
        if not args.keep:
            shutil.rmtree(scratch, ignore_errors=True)

    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPOSITORY, capture_output=True, text=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain", "--untracked-files=no", "--", str(HERE)],
                                cwd=REPOSITORY, capture_output=True, text=True).stdout.strip())
    record = {
        "study": "ablation-study.py",
        "scummvm_commit": commit,
        "foa_opl3_worktree_dirty": dirty,
        "source_sha256": {name: source_sha256(HERE / name) for name in SOURCES + ("ablation-study.py",)},
        "trace_sha256": hashlib.sha256(args.trace.read_bytes()).hexdigest() if args.trace else None,
        "rhythm_trace_sha256": hashlib.sha256(args.rhythm_trace.read_bytes()).hexdigest() if args.rhythm_trace else None,
        "rhythm_trace_from_s": args.rhythm_from if args.rhythm_trace else None,
        "seconds": args.seconds,
        "variants": results,
    }
    args.output.write_text(json.dumps(record, indent=1) + "\n")
    for metric, title, fmt in (("band_db", "mean |third-octave band error| (dB)", "{:.2f}"),
                               ("band_lo_db", "...of which the bands below 8 kHz (dB)", "{:.2f}"),
                               ("band_hi_db", "...of which the bands from 8 kHz up (dB)", "{:.2f}"),
                               ("env_db", "mean |20 ms envelope error| (dB)", "{:.2f}"),
                               ("hf_db", "signed error above 8 kHz (dB; + = excess)", "{:+.2f}"),
                               ("env2_db", "mean |2 ms envelope error| (dB): block-scale timing", "{:.2f}"),
                               ("env2_corr", "2 ms envelope correlation", "{:.4f}"),
                               ("env_corr", "20 ms envelope correlation", "{:.4f}")):
        print(f"\n{title}\n{table(results, metric, fmt)}")


if __name__ == "__main__":
    main()
