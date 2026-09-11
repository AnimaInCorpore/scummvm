// Capture finished SCUMM frames and their live palette from the STE port.
//
// Boots room 28 in cycle-exact Hatari with the benchmark hooks, breaks at the
// renderer marker (phase 3, just before conversion) once each requested VBL
// has passed, and dumps the engine's completed 320x200 chunky frame together
// with the palette the backend converts it with. The directory is the
// --capture input of monkey-flicker-compare.mjs. From the repository
// root, after building the STE executable:
//
//   node devtools/atari-ste/tools/scumm-ste-frame-capture.mjs [--out DIR] [--vbls 12000,12400,...]
import { appendFileSync, copyFileSync, existsSync, mkdirSync, readFileSync, symlinkSync, writeFileSync } from 'node:fs';
import { execFileSync, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from './png.mjs';

const kit = fileURLToPath(new URL('..', import.meta.url));
const repo = resolve(kit, '../..');
const arg = (name, fallback) => process.argv.includes(name) ? process.argv[process.argv.indexOf(name) + 1] : fallback;
const out = resolve(arg('--out', `${kit}/monkey-bar/room28-capture`));
const vbls = arg('--vbls', '12000,12400,12800,13200,13600,14000,14400,14800').split(',').map(Number);
if (!vbls.length || vbls.some((v, i) => !Number.isInteger(v) || v < 1 || (i && v <= vbls[i - 1])))
	throw Error('--vbls needs ascending positive integers');
if (existsSync(`${out}/capture.json`))
	throw Error(`Existing capture at ${out}; choose a new --out directory`);
const build = resolve(process.env.STE_BUILD || `${repo}/build-ste-scumm-static4`);
const hatari = resolve(process.env.HATARI || `${repo}/../F030Arcade/third_party/hatari/build/src/hatari`);
const tos = resolve(process.env.TOS || `${repo}/../F030Arcade/third_party/tos/tos206de.img`);
const nm = resolve(process.env.NM || `${repo}/../cross-mint/bin/m68k-atari-mintelf-nm`);
const hd = `${out}/HD`, gameDir = `${hd}/SCUMMVM`, prg = `${gameDir}/SCUMMVM.PRG`;
mkdirSync(gameDir, { recursive: true });
copyFileSync(`${build}/scummvm.prg`, prg);
for (const name of ['MONKEY', 'ATLANTIS'])
	if (!existsSync(`${gameDir}/${name}`)) symlinkSync(`${build}/HD/SCUMMVM/${name}`, `${gameDir}/${name}`);
let ini = readFileSync(`${build}/HD/SCUMMVM/SCUMMVM.INI`, 'utf8').replace('debuglevel=9', 'debuglevel=0');
ini = ini.replace('[monkey]', '[monkey]\nboot_param=28\nste_benchmark=true\nste_benchmark_walk=false');
writeFileSync(`${gameDir}/SCUMMVM.INI`, ini);

const symbols = execFileSync(nm, [prg], { encoding: 'utf8' });
writeFileSync(`${out}/code-symbols.txt`, [...symbols.matchAll(/^([0-9a-f]+) [TtWw] (.+)$/gm)].map(m => `${m[1]} T ${m[2]}`).join('\n') + '\n');
const offsets = Object.fromEntries([...symbols.matchAll(/^([0-9a-f]+) [a-zA-Z] (.+)$/gm)].map(m => [m[2], parseInt(m[1], 16)]));
const addr = name => {
	if (!(name in offsets)) throw Error(`Missing capture symbol: ${name}`);
	return `TEXT+0x${offsets[name].toString(16)}`;
};
const state = addr('atari_ste_bench_state'), marker = addr('atari_ste_bench_marker');
const source = addr('atari_ste_last_source'), palette = addr('atari_ste_last_palette');
const write = (name, lines) => writeFileSync(`${out}/${name}.ini`, lines.join('\n') + '\n');
// Phase 3 is the start of a conversion, so the engine frame is complete.
const atConversion = vbl => `b pc = '${marker}' && ('${state}+8').l = 3 && VBL > ${vbl} :once :trace`;
write('boot', [`b GemdosOpcode = 0x4b && OsCallParam = 0 :once :file ${out}/loaded.ini`]);
write('loaded', [`b pc = TEXT :once :file ${out}/start.ini`]);
write('start', [
	`symbols ${out}/code-symbols.txt TEXT`,
	`${atConversion(vbls[0])} :file ${out}/capture-0.ini`,
	`b VBL = ${vbls.at(-1) + 5000} :once :file ${out}/timeout.ini`,
]);
vbls.forEach((vbl, k) => write(`capture-${k}`, [
	'echo STE_CAPTURE',
	`savebin ${out}/state-${k}.bin '${state}' #76`,
	`savebin ${out}/frame-${k}.bin '(${source})' #64000`,
	`savebin ${out}/palette-${k}.xrgb.bin '(${palette})' #1024`,
	...(k + 1 < vbls.length
		? [`${atConversion(vbls[k + 1])} :file ${out}/capture-${k + 1}.ini`]
		: ['echo STE_CAPTURE_DONE', 'quit']),
]));
write('timeout', ['echo STE_CAPTURE_TIMEOUT', `screenshot ${out}/timeout.png`, 'quit']);

const args = ['--configfile', '/dev/null', '--tos', tos, '--harddrive', hd,
	'--machine', 'ste', '--monitor', 'rgb', '--memsize', '4', '--cpulevel', '0', '--cpuclock', '8',
	'--cpu-exact', 'on', '--compatible', 'on', '--sound', 'off', '--fast-boot', 'on',
	'--confirm-quit', 'off', '--fast-forward', 'on', '--frameskips', '0', '--spec512', '1',
	'--borders', 'off', '--statusbar', 'off', '--zoom', '1',
	'--conout', '2', '--run-vbls', String(vbls.at(-1) + 5100), '--parse', `${out}/boot.ini`, '--auto', 'C:\\SCUMMVM\\SCUMMVM.PRG'];
console.log(`Room 28 frame capture after VBLs ${vbls.join(',')}; ${out}`);
writeFileSync(`${out}/hatari.log`, '');
const child = spawn(hatari, args, { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy', SDL_RENDER_DRIVER: 'software' } });
for (const stream of [child.stdout, child.stderr]) stream.on('data', data => appendFileSync(`${out}/hatari.log`, data));
child.on('close', (code, signal) => {
	if (code || signal || !readFileSync(`${out}/hatari.log`, 'utf8').includes('STE_CAPTURE_DONE>')) {
		console.log(`Hatari exit ${code ?? signal}; capture incomplete, see ${out}/hatari.log`);
		process.exitCode = 1;
		return;
	}
	// _RGB entries carry a reserved byte before red, green and blue.
	const captures = vbls.map((vbl, k) => {
		const benchmark = readFileSync(`${out}/state-${k}.bin`), frame = readFileSync(`${out}/frame-${k}.bin`);
		const xrgb = readFileSync(`${out}/palette-${k}.xrgb.bin`), rgb = Buffer.alloc(256 * 3);
		for (let i = 0; i < 256; i++) xrgb.copy(rgb, i * 3, i * 4 + 1, i * 4 + 4);
		writeFileSync(`${out}/palette-${k}.rgb.bin`, rgb);
		const preview = new Uint8Array(frame.length * 3);
		for (let i = 0; i < frame.length; i++) rgb.copy(preview, i * 3, frame[i] * 3, frame[i] * 3 + 3);
		writePng(`${out}/frame-${k}.png`, 320, 200, preview);
		return { afterVbl: vbl, room: benchmark.readUInt32BE(16), roomFrame: benchmark.readUInt32BE(12),
			frame: `frame-${k}.bin`, palette: `palette-${k}.rgb.bin`, preview: `frame-${k}.png` };
	});
	const palettes = captures.map(c => readFileSync(`${out}/${c.palette}`));
	const manifest = {
		schema: 'spectrum512-scumm-ste-frame-capture/1',
		prgSha256: createHash('sha256').update(readFileSync(prg)).digest('hex'),
		source: 'atari_ste_last_source (320x200 chunky) and atari_ste_last_palette (_RGB[256]) at benchmark phase 3',
		palettesIdentical: palettes.every(p => p.equals(palettes[0])),
		captures,
	};
	writeFileSync(`${out}/capture.json`, JSON.stringify(manifest, null, 2) + '\n');
	console.log(JSON.stringify({ palettesIdentical: manifest.palettesIdentical, captures: captures.map(c => [c.afterVbl, c.room, c.roomFrame]) }));
	if (captures.some(c => c.room !== 28)) {
		console.log('Not every capture is in room 28');
		process.exitCode = 1;
	}
});
