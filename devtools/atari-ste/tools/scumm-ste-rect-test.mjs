// Unmodified scene renderer + small surface adapters. Host: ASan/UBSan and
// scalar c2p. --hatari: actual 68000 c2p and NF_CYCLES, without engine/raster.
// node devtools/atari-ste/tools/scumm-ste-rect-test.mjs [--hatari] [--out DIR]
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdirSync, copyFileSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const kit = fileURLToPath(new URL('..', import.meta.url));
const repo = resolve(process.env.SCUMM_STE || `${kit}/../..`);
const outArg = process.argv.indexOf('--out');
const out = resolve(outArg < 0 ? `${kit}/monkey-bar/rect-test` : process.argv[outArg + 1]);
const atari = process.argv.includes('--hatari');
const source = `${repo}/backends/graphics/atari`;
mkdirSync(out, { recursive: true });
for (const name of ['atari-ste-scene.cpp', 'atari-ste-scene.h', 'atari-ste-raster.h'])
	copyFileSync(`${source}/${name}`, `${out}/${name}`);
for (const name of ['shim.h', 'test.cpp'])
	copyFileSync(`${kit}/tests/scene-renderer/${name}`, `${out}/${name}`);
for (const name of ['mint/ostruct.h', 'common/scummsys.h', 'common/path.h', 'common/debug.h', 'common/fs.h',
	'common/ptr.h', 'common/stream.h', 'common/textconsole.h', 'graphics/surface.h', 'atari-screen.h', 'atari-surface.h']) {
	mkdirSync(dirname(`${out}/${name}`), { recursive: true });
	writeFileSync(`${out}/${name}`, '#include "shim.h"\n');
}
const toolchain = process.env.MINT_BIN || `${repo}/../cross-mint/bin`;
const compiler = atari ? `${toolchain}/m68k-atari-mintelf-g++` : (process.env.CXX || 'c++');
const binary = `${out}/${atari ? 'RECT.TOS' : 'rect-test'}`;
const args = ['-std=c++11', '-Os', '-g', '-I', out, '-I', repo, `${out}/atari-ste-scene.cpp`, `${out}/test.cpp`, '-o', binary];
if (atari) args.push('-m68000', '-fomit-frame-pointer', '-fno-exceptions', '-Wl,--stack,256k', `${source}/atari-c2p-asm.S`);
else args.push('-fsanitize=address,undefined', '-fno-omit-frame-pointer');
execFileSync(compiler, args, { stdio: 'inherit' });
if (!atari) {
	execFileSync(binary, [], { stdio: 'inherit' });
} else {
	const hatari = resolve(process.env.HATARI || `${repo}/../F030Arcade/third_party/hatari/build/src/hatari`);
	const tos = resolve(process.env.TOS || `${repo}/../F030Arcade/third_party/tos/tos206de.img`);
	const result = spawnSync(hatari, ['--configfile', '/dev/null', '--tos', tos, '--machine', 'ste',
		'--monitor', 'rgb', '--memsize', '4', '--cpulevel', '0', '--frameskips', '0',
		'--cpuclock', '8', '--cpu-exact', 'on', '--compatible', 'on', '--sound', 'off', '--natfeats', 'on',
		'--fast-boot', 'on', '--confirm-quit', 'off', '--fast-forward', 'on', '--conout', '2',
		'--run-vbls', '30000', binary], {
		env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy', SDL_RENDER_DRIVER: 'software' },
		encoding: 'utf8', timeout: 55000, maxBuffer: 16 * 1024 * 1024,
	});
	const log = (result.stdout || '') + (result.stderr || '');
	writeFileSync(`${out}/hatari.log`, log);
	console.log(log.split('\n').filter(l => /SCENE_RECT|Assertion|Error|cycles/.test(l)).join('\n'));
	if (result.error || result.status || !log.includes('SCENE_RECT PASS'))
		throw Error(`Hatari test failed; see ${out}/hatari.log (${result.error || result.signal || result.status})`);
}
