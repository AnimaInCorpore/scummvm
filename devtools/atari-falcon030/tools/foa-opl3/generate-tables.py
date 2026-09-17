#!/usr/bin/env python3
"""Generate the OPL log-sine and exponential ROMs for the host and DSP kernels,
and the block-rate tables of the practical (approximate) kernel.

Both ROMs are closed-form, so they are computed rather than copied. The
kernel test checks them against the Nuked ROMs shipped in this tree.

A DSP56001 has 256 words of internal X RAM and 256 of internal Y RAM. These
two 256-entry tables are exactly that size, so the whole waveform ROM lives
on chip: the eight 1,024-entry unpacked waveform tables a desktop build uses
are reconstructed from the quarter sine by index and sign arithmetic instead.

The practical kernel renders at the Falcon codec's 32,779.9479 Hz in blocks
of 32 frames, and advances its envelopes and LFO once per block. Its tables
are derived here from the exact envelope machine rather than typed in:
each rate's attack curve and decay slope are measured by running that
machine, then retimed to the codec rate and folded into one step per block.
"""
import argparse
import math
from pathlib import Path

LOGSIN = [int(round(-math.log2(math.sin((i + 0.5) * math.pi / 512)) * 256)) for i in range(256)]
EXP = [2 * (int(round((2 ** ((255 - i) / 256.0) - 1) * 1024)) + 1024) for i in range(256)]

NATIVE_RATE = 49716.0
CODEC_RATE = 25175000.0 / 4.0 / 192.0
RATIO = NATIVE_RATE / CODEC_RATE
BLOCK_FRAMES = 32
NATIVE_PER_BLOCK = BLOCK_FRAMES * RATIO
ENV_FRACTION_BITS = 12           # envelope word: attenuation units * 2^12
ENV_OFF = 0x1F8                  # the chip snaps a released envelope here
EG_INC_STEP = ((0, 0, 0, 0), (1, 0, 0, 0), (1, 0, 1, 0), (1, 1, 1, 0))


class ExactEnvelope:
    """The chip-wide envelope clock and one slot, as in opl-kernel.h."""

    def __init__(self):
        self.timer = 0
        self.rem = 0
        self.state = 0
        self.add = 0
        self.timer_lo = 0

    def shift_for(self, rate_hi, rate_lo, reg_rate):
        env_shift = rate_hi + self.add
        shift = 0
        if reg_rate != 0:
            if rate_hi < 12:
                if self.state:
                    if env_shift == 12:
                        shift = 1
                    elif env_shift == 13:
                        shift = (rate_lo >> 1) & 1
                    elif env_shift == 14:
                        shift = rate_lo & 1
            else:
                shift = (rate_hi & 3) + EG_INC_STEP[rate_lo][self.timer_lo]
                if shift & 4:
                    shift = 3
                if not shift:
                    shift = self.state
        return shift

    def clock(self):
        if self.state:
            low = self.timer & 0x1FFF
            if not low:
                self.add = 0
            else:
                shift = 0
                while not ((low >> shift) & 1):
                    shift += 1
                self.add = shift + 1
            self.timer_lo = self.timer & 3
        if self.rem or self.state:
            if self.timer == 0xFFFFFFFFF:
                self.timer = 0
                self.rem = 1
            else:
                self.timer += 1
                self.rem = 0
        self.state ^= 1


def attack_samples(rate):
    """Native samples the exact attack needs from 511 to 0 at this rate."""
    rate_hi, rate_lo = min(rate >> 2, 15), rate & 3
    if rate_hi == 15:
        return 0
    chip, env, samples = ExactEnvelope(), 511, 0
    while env:
        shift = chip.shift_for(rate_hi, rate_lo, 1)
        if shift > 0:
            env = (env + (((~env) & 0xFFFF) >> (4 - shift))) & 0x1FF
        chip.clock()
        samples += 1
        if samples > 50_000_000:
            raise SystemExit(f"attack rate {rate} never completes")
    return samples


def decay_samples(rate):
    """Native samples the exact release needs from 0 to the silence snap."""
    rate_hi, rate_lo = min(rate >> 2, 15), rate & 3
    chip, env, samples = ExactEnvelope(), 0, 0
    while (env & ENV_OFF) != ENV_OFF:
        shift = chip.shift_for(rate_hi, rate_lo, 1)
        if shift > 0:
            env = (env + (1 << (shift - 1))) & 0x1FF
        chip.clock()
        samples += 1
        if samples > 50_000_000:
            raise SystemExit(f"decay rate {rate} never completes")
    return samples


def practical_tables():
    attack, decay = [], []
    for rate in range(64):
        if rate < 4:
            # A zero register nibble stops the envelope; rates 1-3 cannot
            # occur otherwise, so entry 0 (and its neighbours) mean "hold".
            attack.append(0x7FFFFF)
            decay.append(0)
            continue
        samples = attack_samples(rate)
        if samples == 0:
            attack.append(0)                       # instant attack
        else:
            per_sample = math.log(512.0) / samples  # decay of ln(env + 1)
            factor = math.exp(-per_sample * NATIVE_PER_BLOCK)
            attack.append(min(0x7FFFFF, int(round(factor * (1 << 23)))))
        samples = decay_samples(rate)
        units_per_block = ENV_OFF / samples * NATIVE_PER_BLOCK
        decay.append(min(0x7FFFFF, int(round(units_per_block * (1 << ENV_FRACTION_BITS)))))
    # Vibrato multipliers of the pre-doubled f-number step, by LFO position
    # and depth shift (index 0 is the deep setting, 1 the shallow one).
    shape = (0.0, 0.25, 0.5, 0.25, 0.0, -0.25, -0.5, -0.25)
    vibrato = [[int(round(value / (1 << shift) * (1 << 23))) & 0xFFFFFF for value in shape]
               for shift in (0, 1)]
    # Envelope gain 2^(-envOut/32) as a 24-bit fraction; the chip is silent
    # from 0x1f8 up, where its exponent shifts every mantissa bit away.
    gain = [0 if e >= ENV_OFF else min(0x7FFFFF, int(round(2.0 ** (-e / 32.0) * (1 << 23))))
            for e in range(512)]
    return {
        "rate_q16": int(round(RATIO * 65536)),
        # Phase increment word per unit of the chip's native increment: the
        # DSP adds x1 * 1023 * 2 to a 48-bit index.fraction phase per frame.
        "inc_q16": int(round(RATIO * (1 << 24) / (512.0 * 2046.0) * 65536)),
        "gain": gain,
        "tremolo_step": int(round(NATIVE_PER_BLOCK / 64.0 * 4096)),
        "vibrato_step": int(round(NATIVE_PER_BLOCK / 1024.0 * 4096)),
        "attack": attack,
        "decay": decay,
        "vibrato": vibrato,
    }


def c_table(name, values, per_line=8, ctype="uint16_t", width=4):
    body = ""
    for start in range(0, len(values), per_line):
        body += "\t" + ", ".join((f"-0x{-v:0{width}x}" if v < 0 else f"0x{v:0{width}x}")
                                  for v in values[start:start + per_line]) + ",\n"
    return f"static const {ctype} {name}[{len(values)}] = {{\n{body}}};\n"


def signed24(values):
    return [v - (1 << 24) if v >= (1 << 23) else v for v in values]


def dsp_table(label, values, per_line=8):
    body = ""
    for start in range(0, len(values), per_line):
        body += "\tdc\t" + ",".join(f"${v:06x}" for v in values[start:start + per_line]) + "\n"
    return f"{label}\n{body}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, help="C++ header to write")
    parser.add_argument("--dsp", type=Path, help="asm56000 include file to write")
    parser.add_argument("--practical-header", type=Path, help="C++ header of the block-rate tables")
    parser.add_argument("--practical-dsp", type=Path, help="asm56000 include of the block-rate tables")
    args = parser.parse_args()
    if args.header:
        args.header.write_text(
            "/* Generated by generate-tables.py. Do not edit. */\n"
            "#ifndef FOA_OPL_TABLES_H\n#define FOA_OPL_TABLES_H\n#include <stdint.h>\n\n"
            "/* Quarter log-sine, 12 bit; -log2(sin((i + 0.5) * pi / 512)) * 256. */\n"
            + c_table("kOplLogSin", LOGSIN) +
            "\n/* Exponential ROM, 12 bit; 2 * ((2^((255 - i) / 256) - 1) * 1024 + 1024). */\n"
            + c_table("kOplExp", EXP) + "\n#endif\n")
    if args.dsp:
        args.dsp.write_text(
            "; Generated by generate-tables.py. Do not edit.\n"
            "; Quarter log-sine (X) and exponential ROM (Y), 256 words each.\n"
            + dsp_table("oplLogSin", LOGSIN) + "\n" + dsp_table("oplExp", EXP))
    if args.practical_header or args.practical_dsp:
        tables = practical_tables()
        if args.practical_header:
            args.practical_header.write_text(
                "/* Generated by generate-tables.py. Do not edit. */\n"
                "#ifndef FOA_OPL_PRACTICAL_TABLES_H\n#define FOA_OPL_PRACTICAL_TABLES_H\n"
                "#include <stdint.h>\n\n"
                f"#define OPL_PRACTICAL_CODEC_RATE {CODEC_RATE!r}\n"
                f"#define OPL_PRACTICAL_BLOCK_FRAMES {BLOCK_FRAMES}\n"
                f"#define OPL_PRACTICAL_RATE_Q16 {tables['rate_q16']}u\n"
                f"#define OPL_PRACTICAL_INC_Q16 {tables['inc_q16']}u\n"
                f"#define OPL_PRACTICAL_TREMOLO_STEP {tables['tremolo_step']}\n"
                f"#define OPL_PRACTICAL_VIBRATO_STEP {tables['vibrato_step']}\n\n"
                "/* Attack retention factor per block, 24-bit fraction, by 6-bit rate. */\n"
                + c_table("kOplAttackBlock", tables["attack"], ctype="uint32_t", width=6) +
                "\n/* Decay and release step per block, attenuation units * 2^12, by rate. */\n"
                + c_table("kOplDecayBlock", tables["decay"], ctype="uint32_t", width=6) +
                "\n/* Vibrato multiplier of the doubled f-number step: [depth shift][position]. */\n"
                + c_table("kOplVibratoDeep", signed24(tables["vibrato"][0]), ctype="int32_t", width=6)
                + c_table("kOplVibratoShallow", signed24(tables["vibrato"][1]), ctype="int32_t", width=6)
                + "\n/* Envelope gain 2^(-envOut/32), 24-bit fraction, zero from 0x1f8. */\n"
                + c_table("kOplGain", tables["gain"], ctype="uint32_t", width=6)
                + "\n#endif\n")
        if args.practical_dsp:
            # Only the LFO steps are assembled in; the tables reach the DSP in
            # the host's upload image, built by rt-fixture.cpp from the header.
            args.practical_dsp.write_text(
                "; Generated by generate-tables.py. Do not edit.\n"
                f"OPL_TREMOLO_STEP equ {tables['tremolo_step']}\n"
                f"OPL_VIBRATO_STEP equ {tables['vibrato_step']}\n")
    if not any((args.header, args.dsp, args.practical_header, args.practical_dsp)):
        tables = practical_tables()
        print(f"logsin {min(LOGSIN)}..{max(LOGSIN)}  exp {min(EXP)}..{max(EXP)}")
        print(f"ratio {RATIO:.6f} native per block {NATIVE_PER_BLOCK:.4f}")
        for rate in (4, 16, 32, 40, 48, 52, 56, 59, 60):
            print(f"rate {rate:2d}: attack {attack_samples(rate):8d} samples, factor"
                  f" {tables['attack'][rate] / (1 << 23):.6f}; decay {decay_samples(rate):8d} samples,"
                  f" {tables['decay'][rate] / 4096:.4f} units/block")


if __name__ == "__main__":
    main()
