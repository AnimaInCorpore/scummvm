// Builds and runs raster-test/timerb-raster-test.s in Hatari, screenshots the result,
// and verifies every display row shows its scheduled colour.
import { execFileSync, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { readPng } from './png.mjs';

process.chdir(resolve(new URL('..', import.meta.url).pathname));
const out = resolve(process.argv[2] || '/tmp/timerb');
mkdirSync(out, { recursive: true });

// 1. schedule + assemble
execFileSync(process.execPath, ['tools/timerb-schedule.mjs', 'raster-test/timerb-schedule.bin'], { stdio: 'inherit' });
const vasm = process.env.VASM || resolve('../../../F030Arcade/third_party/vasm/vasmm68k_mot');
execFileSync(vasm, ['timerb-raster-test.s', '-m68000', '-Ftos', '-L', 'timerb-raster-test.lst', '-o', 'TIMERB.PRG'],
	{ cwd: 'raster-test', stdio: 'inherit' });

// symbol table from the TOS PRG sections (same recipe as ste/tools/build.mjs in Spectrum512Painter)
const prg = readFileSync('raster-test/TIMERB.PRG');
const textSize = prg.readUInt32BE(2), dataSize = prg.readUInt32BE(6);
const base = [0, textSize, textSize + dataSize];
const syms = [...readFileSync('raster-test/timerb-raster-test.lst', 'utf8')
	.matchAll(/^(\w+)\s+(0[012]):([0-9A-F]{8})\s*$/gm)]
	.map(([, name, sec, off]) => `${(base[Number(sec)] + parseInt(off, 16)).toString(16)} T ${name}`);
writeFileSync(`${out}/symbols`, syms.join('\n') + '\n');

// 2. run in Hatari. Chain: break when the program is Pexec'd, then at its
// entry (pc=TEXT) load symbols so TEXT resolves to the real load base; then
// screenshot and dump the raster state at VBL 600.
const dump = ['raster_frames', 'raster_skipped', 'pending_screen', 'display_palette', 'current_screen', 'line1_address'];
writeFileSync(`${out}/boot.ini`, `b GemdosOpcode = 0x4b && OsCallParam = 0 :once :file ${out}/loaded.ini\n`);
writeFileSync(`${out}/loaded.ini`, `b pc = TEXT :once :file ${out}/start.ini\n`);
writeFileSync(`${out}/start.ini`, `symbols ${out}/symbols TEXT\nb VBL = 600 :once :file ${out}/cap.ini\n`);
writeFileSync(`${out}/cap.ini`, [
	`echo TIMERB_SHOT`,
	`screenshot ${out}/shot.png`,
	`echo HWPAL`,
	`m 0xffff8240`,
	...dump.map(n => `echo ${n}\nevaluate (${n})`),
	`quit`,
].join('\n') + '\n');

const hatari = process.env.HATARI || resolve('../../../F030Arcade/third_party/hatari/build/src/hatari');
const tos = process.env.TOS || resolve('../../../F030Arcade/third_party/tos/tos206de.img');
const r = spawnSync(hatari, ['--tos', tos, '--machine', 'ste', '--memsize', '4', '--cpulevel', '0',
	'--cpuclock', '8', '--cpu-exact', 'on', '--compatible', 'on', '--sound', 'off', '--fast-boot', 'on',
	'--confirm-quit', 'off', '--fast-forward', 'on', '--frameskips', '0', '--spec512', '1', '--borders', 'off',
	'--statusbar', 'off', '--zoom', '1', '--run-vbls', '720', '--conout', '2', '--parse', `${out}/boot.ini`,
	resolve('raster-test/TIMERB.PRG')],
	{ env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy', SDL_RENDER_DRIVER: 'software' }, encoding: 'utf8' });
writeFileSync(`${out}/hatari.log`, (r.stdout || '') + (r.stderr || ''));
const all=(r.stdout+r.stderr).split('\n');
const errs = all.filter(l => /Bus Error|Address Error|HWPAL|HWSTATE|succeeded|= \$|raster_frames|raster_skipped|pending_screen|display_palette|current_screen|line1_address/.test(l));
console.log(errs.join('\n'));

// 3. verify each row against its scheduled colour
if (!existsSync(`${out}/shot.png`)) { console.log('no screenshot produced'); process.exit(1); }
const shot = readPng(`${out}/shot.png`);
const scale = Math.round(shot.width / 320);
console.log(`screenshot ${shot.width}x${shot.height}, scale ${scale}`);
const expect = y => {
	// Lines 0 and 1 share line 0's palette (written at VBL); the raster paints
	// line 2 onward, and its first loop iteration lands on line 3, so line 2
	// also keeps the header palette. Everything from line 3 is per-line exact.
	const src = y < 3 ? 0 : y;
	const r = (src * 5) & 15, g = (src * 11 + 3) & 15, b = (src * 7 + 9) & 15;
	return [r * 17, g * 17, b * 17];
};
let bad = 0, rows = [];
for (let y = 0; y < 200; y++) {
	const [er, eg, eb] = expect(y);
	const o = ((y * scale) * shot.width + 4 * scale) * 4;   // sample a few px in
	const got = [shot.rgba[o], shot.rgba[o + 1], shot.rgba[o + 2]];
	if (got[0] !== er || got[1] !== eg || got[2] !== eb) {
		bad++;
		if (rows.length < 12) rows.push(`row ${y}: expected ${er},${eg},${eb} got ${got}`);
	}
}
console.log(`${bad} of 200 rows mismatched`);
rows.forEach(r => console.log('  ' + r));
process.exit(bad ? 1 : 0);
