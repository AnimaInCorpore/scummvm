#!/usr/bin/env python3
"""Build and verify resident-table rendering with live pitch/level controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import numpy as np
from bank import RATE, resample, wav
from bench import HERE, run


def cpu_time(path):
    text = path.read_text()
    hz = int(re.search(r"Cycles/second:\s*(\d+)", text)[1])
    rows = re.findall(r"^([0-9a-f]+)\s.*?\([0-9]+, ([0-9]+),", text, re.M)
    cycles = sum(int(c) for pc, c in rows)
    if hz != 16042494 or not cycles:
        raise ValueError("CPU profile calibration mismatch")
    return hz, cycles


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--f030mt32", type=Path, default=HERE.parents[4]/"F030MT32")
    parser.add_argument("--hatari", type=Path, default=HERE.parents[4]/"F030Arcade/third_party/hatari/build/src/hatari")
    parser.add_argument("--dosbox", default=shutil.which("dosbox-staging"))
    parser.add_argument("--bank", type=Path, default=HERE/"build/resident-bank")
    parser.add_argument("--voices", type=int, choices=range(1, 33), default=32)
    parser.add_argument("--blocks", type=int, choices=range(1, 2049), default=256)
    parser.add_argument("--frames", type=int, choices=(64, 128, 512), default=64)
    parser.add_argument("--ssi", action="store_true", help="play through SSI/DAC and verify a DMA recording")
    parser.add_argument("--prefill-frames", type=int, choices=(512, 1024), default=512, help="SSI startup reserve; 1024 uses a larger ring")
    parser.add_argument("--attacks", type=int, choices=range(33), default=0, help="simultaneous 150 ms recorded attacks before resident sustain")
    parser.add_argument("--captures", type=Path, default=HERE/'build/captures')
    parser.add_argument("--attack-stress", action="store_true", help="stream attack voices for the whole timed interval to measure their cost")
    args = parser.parse_args()
    root = args.f030mt32.resolve()
    sys.path.insert(0, str(root/"tools"))
    from generate_dsp_stage2 import make_boot_image
    from profile_dsp import parse_profile, parse_listing, require_symbol
    bank = json.loads((args.bank/"bank.json").read_text())
    entries, size = bank["entries"], bank["table_frames"]
    case = HERE/f"build/resident-{args.voices}-{args.frames}{'-ssi' if args.ssi else ''}{'-a'+str(args.attacks) if args.attacks else ''}{'-stress' if args.attack_stress else ''}{'-p1024' if args.prefill_frames==1024 else ''}"
    case.mkdir(parents=True, exist_ok=True)
    samples = [np.fromfile(args.bank/(e['id']+'.s16le'), dtype='<i2').astype(np.int64) for e in entries]
    per_bank = (len(samples)+1)//2
    # Lower SRAM half: Y tables. Upper half: X tables and X output buffer.
    # Tables must not alias the SSI output ring in the same physical SRAM.
    base = 0x400
    assert base+per_bank*size <= 0x3000
    assert args.voices*3 <= 0x70
    startup = [per_bank*size]
    startup += [int(x)&0xffffff for s in samples[:per_bank] for x in s]
    startup += [(len(samples)-per_bank)*size]
    startup += [int(x)&0xffffff for s in samples[per_bank:] for x in s]
    phases = []
    for i in range(args.voices):
        table = i % len(samples)
        phase = (i*2137+65520) & 65535
        startup += [int(table>=per_bank), base+(table%per_bank)*size, phase]
        phases.append(phase)
    if args.attacks > args.voices:
        raise SystemExit('Attack voices exceed the voice count')
    attack_blocks = args.blocks if args.attack_stress else min(args.blocks, int(np.ceil(.15*RATE/args.frames)))
    attack_length = attack_blocks*args.frames
    attacks = {}
    if args.attacks:
        for e in entries:
            x = resample(np.fromfile(args.captures/(e['id']+'.s16le'), dtype='<i2').astype(float))
            if attack_length > len(x):
                raise SystemExit('Capture is too short for this attack stress test')
            x = x[:attack_length]
            scale = max(float(abs(x).max())/32752, 1/32752)
            q = np.clip(np.rint(x/scale/16), -2048, 2047).astype(np.int64)
            attacks[e['id']] = (q*16, scale)
    controls = []
    payload_counts = []
    total = args.blocks*args.frames
    expected = np.zeros((total, 2), dtype=np.int64)
    for block in range(args.blocks):
        packet = []
        for voice in range(args.voices):
            table = voice%len(samples)
            # Real control changes. Pitch is bent by up to one semitone; gains
            # rise and fall independently. All voices have nonzero gain.
            bend = np.sin(2*np.pi*block/101+voice) * 1/12
            step = round(entries[table]['step'] * 2**bend)
            level = .55+.4*np.sin(block/23+voice)**2
            left = round((voice%7+1)*(1<<16)*level)
            right = round((8-voice%7)*(1<<16)*level)
            phase = (phases[voice]+np.arange(args.frames)*step) & 65535
            source = samples[table][phase*size//65536]
            streaming = voice < args.attacks and block < attack_blocks
            if streaming:
                data, scale = attacks[entries[table]['id']]
                source = data[block*args.frames:(block+1)*args.frames]
                left, right = round(left*scale), round(right*scale)
            packet += [step | (0x800000 if streaming else 0), left, right]
            if streaming:
                q = source//16 & 4095
                packet += ((q[::2]<<12)|q[1::2]).tolist()
            expected[block*args.frames:(block+1)*args.frames, 0] += source*left//(1<<23)
            expected[block*args.frames:(block+1)*args.frames, 1] += source*right//(1<<23)
            phases[voice] = (phases[voice]+args.frames*step) & 65535
        payload_counts.append(len(packet))
        controls += [len(packet)] + packet
    np.asarray(startup, dtype='>u4').tofile(case/'START.RAW')
    np.asarray(controls, dtype='>u4').tofile(case/'CTRL.RAW')
    for name in ('ASM56000.EXE', 'CLDLOD.EXE', 'DOS4GW.EXE', 'ioequ.inc'):
        shutil.copyfile(root/'third_party/f030dsp3d/tools/asm56k'/name, case/name)
    shutil.copyfile(HERE/'resident.asm', case/'MIX.ASM')
    ring_words = args.prefill_frames*4
    if args.ssi and args.blocks*args.frames < args.prefill_frames:
        raise SystemExit('SSI test is shorter than the prefill')
    assert 0x3000+ring_words <= 0x4000
    (case/'config.inc').write_text(f'VOICES equ {args.voices}\nFRAMES equ {args.frames}\nTABLE_SIZE equ {size}\nTABLE_BASE equ {base}\nSSI equ {int(args.ssi)}\nBLOCKS equ {args.blocks}\nRING_WORDS equ {ring_words}\nPREFILL_WORDS equ {args.prefill_frames*2}\n')
    (case/'BUILD.BAT').write_text('@ECHO OFF\nASM56000.EXE -q -a -bMIX.CLD -z -lMIX.LST MIX.ASM\nIF ERRORLEVEL 1 EXIT 1\nCLDLOD.EXE MIX.CLD > MIX.LOD\nEXIT\n')
    for name in ('MIX.CLD', 'MIX.LST', 'MIX.LOD'):
        (case/name).unlink(missing_ok=True)
    run([args.dosbox, '--noprimaryconf', '--set', 'output=texture', case/'BUILD.BAT'], HERE, case/'assembler.log')
    listing = (case/'MIX.LST').read_text()
    if not all(re.search(rf'^0 +{s}$', listing, re.M) for s in ('Errors', 'Warnings')):
        raise SystemExit(f'Assembly failed: {case / "MIX.LST"}')
    boot = make_boot_image(case/'MIX.LOD', limit=512, purpose='resident mixer')
    (case/'image.i').write_text(f'MIX_BOOT_WORDS equ {len(boot)}\n        data\nmix_boot:\n'+''.join(
        f'        dc.b ${(w>>16)&255:02x},${(w>>8)&255:02x},${w&255:02x}\n' for w in boot)+'        even\n')
    (case/'config.i').write_text(f'BLOCKS equ {args.blocks}\nFRAMES equ {args.frames}\nCONTROL_WORDS equ {args.voices*3}\nSTARTUP_WORDS equ {len(startup)}\nOUTPUT_BYTES equ {(total+(4096 if args.ssi else 0))*4}\n')
    host_source = HERE/('resident_ssi_host.s' if args.ssi else 'resident_host.s')
    run([root/'build/tools/vasm/vasmm68k_mot', host_source, '-quiet', '-Felf', '-m68030',
         f'-I{root / "src/m68k"}', f'-I{case}', '-o', case/'host.o'], HERE, case/'vasm.log')
    run([root/'build/tools/vlink/vlink', case/'host.o', '-b', 'ataritos', '-s', '-e', 'start', '-o', case/'MIX.TOS'], HERE, case/'vlink.log')
    (case/'start.ini').write_text(f'b d7 = $135780 :once :trace :file {case / "load.ini"}\n')
    (case/'load.ini').write_text(f'profile on\nb d7 = $246880 :once :trace :file {case / "loaded.ini"}\n')
    (case/'loaded.ini').write_text(f'profile save {case / "load-cpu.txt"}\nprofile off\nb d7 = $13579b :once :trace :file {case / "begin.ini"}\n')
    (case/'begin.ini').write_text(f'r\nprofile on\ndp on\nb d7 = $2468ac :once :trace :file {case / "end.ini"}\n')
    (case/'end.ini').write_text(f'profile save {case / "cpu.txt"}\nprofile off\ndp save {case / "dsp.txt"}\ndp off\n')
    for name in ('OUTPUT.RAW', 'STATS.RAW', 'cpu.txt', 'dsp.txt', 'load-cpu.txt'):
        (case/name).unlink(missing_ok=True)
    for name in ('result.json', 'failure.json'):
        (case/name).unlink(missing_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    run([args.hatari, '--machine', 'falcon', '--dsp', 'emu', '--memsize', '14', '--cpuclock', '16', '--cpu-exact', 'true',
         '--tos', root/'third_party/f030dsp3d/tools/tos402.rom', '--patch-tos', 'true', '--fast-boot', 'true', '--fast-forward', 'true',
         '--sound', 'off', '--confirm-quit', 'false', '--run-vbls', '1100', '--trace', 'gemdos', '--trace-file', case/'trace.txt',
         '--parse', case/'start.ini', 'MIX.TOS'], case, case/'hatari.log', env)
    trace = (case/'trace.txt').read_text()
    if 'Pterm0' not in trace or 'Pterm(1)' in trace:
        raise SystemExit(f'Host failed: {case}')
    hz, cycles = cpu_time(case/'cpu.txt')
    hashes = {name:hashlib.sha256((case/name).read_bytes()).hexdigest()
              for name in ('MIX.ASM', 'MIX.TOS', 'START.RAW', 'CTRL.RAW', 'cpu.txt', 'dsp.txt')}
    hashes.update({p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in (Path(__file__), host_source)})
    actual = np.fromfile(case/'OUTPUT.RAW', dtype='>i2').astype(np.int64).reshape(-1, 2)
    if expected.min() < -32768 or expected.max() > 32767:
        raise SystemExit('Fixture clipped')
    offset = 0
    ssi_stats = None
    if args.ssi:
        ssi_stats = np.fromfile(case/'STATS.RAW', dtype='>u4').tolist()
        if ssi_stats != [0, 0, total*2 & 65535, 0x454e44]:
            (case/'failure.json').write_text(json.dumps(dict(reason='SSI underrun/error/counter failure',
                voices=args.voices, frames=total, frames_per_block=args.frames, blocks=args.blocks,
                attack_voices=args.attacks, attack_stress=args.attack_stress, ssi_status=ssi_stats,
                prefill_frames=args.prefill_frames, elapsed_ms=cycles*1000/hz,
                audio_duration_ms=total*1000/RATE, sha256=hashes), indent=2)+'\n')
            raise SystemExit(f'SSI underrun/error/counter failure: {ssi_stats}')
        # Search only the bounded startup prefix; compare every following word,
        # so alignment cannot conceal dropped, repeated or swapped samples.
        candidates = np.where(np.all(actual[:4097] == expected[0], axis=1))[0]
        matches = [int(i) for i in candidates if i+total<=len(actual) and np.array_equal(actual[i:i+total], expected)]
        if len(matches)==1:
            offset = matches[0]
            actual = actual[offset:offset+total]
    if actual.shape != expected.shape or not np.array_equal(expected, actual):
        np.save(case/'expected.npy', expected)
        np.save(case/'actual.npy', actual)
        raise SystemExit(f'Output mismatch: {case}')
    load_hz, load_cycles = cpu_time(case/'load-cpu.txt')
    dsp_hz, dsp_cycles, rows = parse_profile(case/'dsp.txt')
    symbols = parse_listing(case/'MIX.LST')
    loop_pcs = [require_symbol(symbols, 'P', s) for s in ('loop_x', 'loop_y')]
    stream_pc = require_symbol(symbols, 'P', 'stream_read')
    stream_words = sum(n for pc, n, c, p in rows if pc==stream_pc)
    rendered = sum(n for pc, n, c, p in rows if pc in loop_pcs) + 2*stream_words
    if rendered != total*args.voices:
        raise SystemExit(f'Voice sample count mismatch: {rendered}')
    if stream_words != args.attacks*attack_length//2:
        raise SystemExit(f'Attack sample count mismatch: {stream_words}')
    wav(case/'mix.wav', actual)
    report = dict(voices=args.voices, frames_per_block=args.frames, blocks=args.blocks, frames=total,
                  ssi=args.ssi, dma_alignment_frames=offset, ssi_status=ssi_stats,
                  timing_mode='clock-paced playback' if args.ssi else 'unpaced render',
                  elapsed_ms=cycles*1000/hz, audio_duration_ms=total*1000/RATE,
                  prefill_frames=args.prefill_frames if args.ssi else 0,
                  prefill_audio_ms=args.prefill_frames*1000/RATE if args.ssi else 0,
                  attack_voices=args.attacks, attack_frames=attack_length if args.attacks else 0,
                  attack_stress=args.attack_stress, verified_attack_words=stream_words,
                  table_words=bank['table_words'], table_bytes=bank['table_words']*3,
                  reserved_output_words=ring_words, internal_y_state_words=128,
                  startup_words=len(startup), startup_ms=load_cycles*1000/load_hz,
                  control_words_per_block=args.voices*3, cpu_hz=hz, cpu_cycles=cycles,
                  max_input_words_per_block=max(payload_counts),
                  block_ms=cycles*1000/hz/args.blocks if not args.ssi else None,
                  equivalent_512_frame_ms=cycles*1000/hz/total*512 if not args.ssi else None,
                  deadline_ms=args.frames*1000/RATE, fraction_of_deadline=cycles/hz/(total/RATE) if not args.ssi else None,
                  dsp_hz=dsp_hz, dsp_instruction_cycles=dsp_cycles/2,
                  exact_stereo_words=actual.size, verified_voice_samples=rendered,
                  omissions=([] if args.attacks else ['recorded attacks']) +
                            ['attack-to-sustain crossfade', 'attack pitch interpolation', 'release timbre', 'reverb', 'ScummVM', 'physical hardware'] + ([] if args.ssi else ['SSI/DAC']),
                  sha256=hashes)
    (case/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
