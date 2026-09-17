#!/usr/bin/env python3
"""Rebuild an uncompressed monster.sou from a FLAC-compressed monster.sof.

The Falcon build decodes no codec (config.h leaves USE_FLAC, USE_VORBIS and
USE_MAD undefined), because the DSP is fully occupied by the OPL kernel and
the 16 MHz 68030 has no headroom for a decoder beside the game loop. So the
only speech container it can play is the raw one, `.sou` in kVOCMode, and of
the four gate games only Fate of Atlantis ships that; Day of the Tentacle and
the two Ultimate Talkie editions ship `.sof`.

A `.sof` can be turned back into the original `.sou` because it carries the
original offsets itself. Its index is, per sample (engines/scumm/sound.cpp,
`MP3OffsetTable` and `Sound::setupSfxFile`):

    uint32be  org_offset       where the sample sat in the original .sou
    uint32be  new_offset       its place here, from the end of the index
    uint32be  num_tags         mouth-sync bytes
    uint32be  compressed_size  FLAC bytes

`org_offset` is what the game scripts pass to `Sound::startTalkSound`, so
writing each sample back at its own `org_offset` restores the layout the
scripts expect. FLAC is lossless, so the decoded sample count - and hence the
rebuilt VOC block - is exactly the size the original occupied; the tool checks
that against the gap to the next offset rather than trusting it.

What kVOCMode expects at `org_offset` (`startTalkSound` skips 4+4 bytes, reads
`num_tags` bytes of sync, then hands the rest to `makeVOCStream` with
FLAG_UNSIGNED):

    "VCTL" + uint32be(num_tags + 8) + tags + VOC file header + block 1 + 0x00

Only FLAC input is handled. MP3 and Vorbis are lossy, so their decoded length
need not match the original and the records would not be guaranteed to fit.

Usage:
    monster-sou-rebuild.py --input monster.sof --output monster.sou
"""
import argparse
import concurrent.futures
import os
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

# audio/decoders/voc.cpp: checkVOCHeader insists on this exact shape.
VOC_DESC = b"Creative Voice File\x1a"
VOC_HEADER_SIZE = 26          # sizeof(VocFileHeader), and the datablock offset
VOC_VERSION = 0x010A
VOC_ID = (~VOC_VERSION + 0x1234) & 0xFFFF
# getSampleRateFromVOCRate special-cases these two divisors back to exact rates.
EXACT_DIVISORS = {11025: 0xA5, 22050: 0xD2}
# backends/platform/atari/atari-dsp.h kPcmRateHz: what the mixer runs at, so
# anything above it is downsampled on the way out and can alias.
PCM_RATE_HZ = 12292


def voc_divisor(rate):
    """The block-1 frequency divisor that getSampleRateFromVOCRate maps to rate."""
    if rate in EXACT_DIVISORS:
        return EXACT_DIVISORS[rate]
    divisor = 256 - 1000000 // rate
    if not 0 <= divisor <= 255 or 1000000 // (256 - divisor) != rate:
        raise ValueError(f"no VOC divisor for {rate} Hz")
    return divisor


def read_streaminfo(block):
    """rate, channels, bits, total samples from a FLAC stream's STREAMINFO."""
    if block[:4] != b"fLaC":
        raise ValueError("not a FLAC stream")
    if block[4] & 0x7F != 0:
        raise ValueError("first metadata block is not STREAMINFO")
    info = block[8:8 + 34]
    packed = int.from_bytes(info[10:18], "big")
    return (packed >> 44) & 0xFFFFF, ((packed >> 41) & 0x7) + 1, ((packed >> 36) & 0x1F) + 1, packed & 0xFFFFFFFFF


def read_index(path):
    """[(org_offset, new_offset, num_tags, compressed_size)], file order."""
    with path.open("rb") as handle:
        size = struct.unpack(">I", handle.read(4))[0]
        if size % 16:
            raise ValueError(f"index size {size} is not a multiple of 16")
        raw = handle.read(size)
    if len(raw) != size:
        raise ValueError("truncated index")
    entries = []
    for i in range(size // 16):
        org, new, tags, comp = struct.unpack(">4I", raw[i * 16:i * 16 + 16])
        # The engine adds the index size and the 4-byte size field itself.
        entries.append((org, new + size + 4, tags, comp))
    return entries


def lowpass(ffmpeg, pcm, rate, cutoff):
    """Band-limit 8-bit unsigned PCM in place, at its own rate.

    The Falcon mixer runs at kPcmRateHz (12,292 Hz mono) and ScummVM resamples
    to it in audio/rate.cpp's interpolateConvert: linear interpolation between
    adjacent input samples, with no anti-alias filter. A 22,050 Hz sample
    therefore folds everything above 6,146 Hz back into the speech band, which
    is audible on bright voices as a smeared, blurred consonant.

    Filtering does not resample, so the sample count - and with it the record
    size and every offset after it - is unchanged; the caller checks that.
    Three biquad sections give about 36 dB per octave, so the content near the
    source's own Nyquist, which folds down the furthest into the speech band,
    lands well attenuated.
    """
    chain = ",".join([f"lowpass=f={cutoff}:poles=2"] * 3)
    done = subprocess.run([ffmpeg, "-v", "error", "-f", "u8", "-ar", str(rate), "-ac", "1",
                           "-i", "-", "-af", chain, "-f", "u8", "-"],
                          input=pcm, capture_output=True)
    if done.returncode != 0:
        raise RuntimeError(f"ffmpeg failed: {done.stderr.decode(errors='replace').strip()}")
    if len(done.stdout) != len(pcm):
        raise RuntimeError(f"lowpass changed the length, {len(pcm)} to {len(done.stdout)}; "
                           "the record would no longer fit its offset")
    return done.stdout


def decode(flac, data):
    """Raw 8-bit unsigned PCM from one FLAC stream, through the flac CLI."""
    done = subprocess.run([flac, "-d", "-c", "--totally-silent", "--force-raw-format",
                           "--endian=little", "--sign=unsigned", "-"],
                          input=data, capture_output=True)
    if done.returncode != 0:
        raise RuntimeError(f"flac failed: {done.stderr.decode(errors='replace').strip()}")
    return done.stdout


def build_record(tags, pcm, rate):
    """The bytes that belong at org_offset: VCTL block, then a one-block VOC."""
    voc = (VOC_DESC + struct.pack("<HHH", VOC_HEADER_SIZE, VOC_VERSION, VOC_ID)
           + b"\x01" + (len(pcm) + 2).to_bytes(3, "little")
           + bytes((voc_divisor(rate), 0))   # divisor, codec 0 = 8-bit unsigned PCM
           + pcm + b"\x00")                  # terminator block
    return b"VCTL" + struct.pack(">I", len(tags) + 8) + tags + voc


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, required=True, help="the .sof to rebuild from")
    parser.add_argument("--output", type=Path, required=True, help="the .sou to write")
    parser.add_argument("--flac", default="flac", help="the flac CLI to decode with")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4),
                        help="parallel flac processes")
    parser.add_argument("--limit", type=int, help="only the first N samples (a smoke test; "
                                                  "the result is not playable)")
    parser.add_argument("--lowpass", type=int, nargs="?", const=5800, metavar="HZ",
                        help="band-limit samples that the Falcon mixer will have to downsample "
                             f"(those above {PCM_RATE_HZ} Hz), so they do not alias in ScummVM's "
                             "unfiltered rate conversion; default 5800 Hz")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="the ffmpeg CLI to filter with")
    parser.add_argument("--verify", type=int, nargs="?", const=200, metavar="N",
                        help="after writing, read N random samples back out of the .sou and "
                             "check them against the source (0 for all of them)")
    args = parser.parse_args()

    if args.output.exists():
        parser.error(f"{args.output} exists; remove it or pick another name")
    try:
        subprocess.run([args.flac, "--version"], capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        parser.error(f"cannot run {args.flac!r}; install flac or pass --flac")

    if args.lowpass:
        try:
            subprocess.run([args.ffmpeg, "-version"], capture_output=True, check=True)
        except (OSError, subprocess.CalledProcessError):
            parser.error(f"cannot run {args.ffmpeg!r}; install ffmpeg or drop --lowpass")

    entries = read_index(args.input)
    print(f"{args.input.name}: {len(entries)} samples, "
          f"last at offset {max(e[0] for e in entries):,}")
    # The gap to the next sample is the room a record has. The index is in
    # offset order in every file seen so far, but sort rather than assume it.
    order = sorted(range(len(entries)), key=lambda i: entries[i][0])
    limit = args.limit if args.limit else len(entries)
    source = args.input.read_bytes()

    started = time.monotonic()
    written = total_pcm = 0
    rates = {}
    with args.output.open("wb") as out, \
         concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        def work(position):
            index = order[position]
            org, new, num_tags, comp = entries[index]
            tags = source[new:new + num_tags]
            data = source[new + num_tags:new + num_tags + comp]
            rate, channels, bits, _ = read_streaminfo(data)
            if channels != 1 or bits != 8:
                raise ValueError(f"sample at {org}: {channels} channels, {bits} bits; "
                                 "a VOC block 1 holds 8-bit mono only")
            pcm = decode(args.flac, data)
            if args.lowpass and rate > PCM_RATE_HZ:
                pcm = lowpass(args.ffmpeg, pcm, rate, args.lowpass)
            record = build_record(tags, pcm, rate)
            room = (entries[order[position + 1]][0] - org) if position + 1 < len(order) else None
            if room is not None and len(record) > room:
                raise ValueError(f"sample at {org}: rebuilt to {len(record)} bytes "
                                 f"but only {room} are free before the next one")
            return org, record, rate

        for done, (org, record, rate) in enumerate(pool.map(work, range(limit)), 1):
            out.seek(org)
            out.write(record)
            written += 1
            total_pcm += len(record)
            rates[rate] = rates.get(rate, 0) + 1
            if done % 250 == 0 or done == limit:
                elapsed = time.monotonic() - started
                print(f"  {done}/{limit} samples, {elapsed:.0f} s, "
                      f"{done / max(elapsed, 0.001):.0f}/s", flush=True)

    size = args.output.stat().st_size
    print(f"wrote {args.output} - {written} samples, {size:,} bytes "
          f"({size / 1e6:.0f} MB), rates {rates}")
    if args.limit:
        print("NOTE: --limit was given, so this file is a smoke test, not playable speech.")

    if args.verify is not None:
        picks = list(range(limit))
        if args.verify:
            random.seed(7)   # a fixed seed so a rerun checks the same samples
            picks = sorted(random.sample(picks, min(args.verify, len(picks))))
        bad = verify(args, entries, order, source, picks)
        print(f"verified {len(picks)} samples against {args.input.name}: "
              f"{bad} mismatched")
        if bad:
            return 1
    return 0


def verify(args, entries, order, source, picks):
    """Read records back out of the .sou and compare them with the source."""
    bad = 0
    with args.output.open("rb") as out:
        for position in picks:
            org, new, num_tags, comp = entries[order[position]]
            tags = source[new:new + num_tags]
            data = source[new + num_tags:new + num_tags + comp]
            rate, _, _, _ = read_streaminfo(data)
            pcm = decode(args.flac, data)
            if args.lowpass and rate > PCM_RATE_HZ:
                pcm = lowpass(args.ffmpeg, pcm, rate, args.lowpass)
            expected = build_record(tags, pcm, rate)
            out.seek(org)
            if out.read(len(expected)) != expected:
                print(f"  sample at {org} does not match")
                bad += 1
    return bad


if __name__ == "__main__":
    sys.exit(main())
