// Measure real room 28 through boot_param=28, with optional scripted walking.
// Debugger snapshots and instruction profiles leave the beam-timed ISR intact.
// node devtools/atari-ste/tools/scumm-ste-room-profile.mjs [--out DIR] [--first N] [--last N] [--idle]
import { mkdirSync, readFileSync, writeFileSync, copyFileSync, existsSync, symlinkSync, appendFileSync } from 'node:fs';
import { execFileSync, spawn } from 'node:child_process';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

const kit = fileURLToPath(new URL('..', import.meta.url));
const repo = resolve(kit, '../..');
const arg = (name, fallback) => process.argv.includes(name) ? process.argv[process.argv.indexOf(name) + 1] : fallback;
const out = resolve(arg('--out', `${kit}/monkey-bar/room-profile/walk`));
const first = Number(arg('--first', 8)), last = Number(arg('--last', 24));
if (!Number.isInteger(first) || !Number.isInteger(last) || first < 1 || last <= first)
	throw Error('Frame bounds must be integers with 1 <= first < last');
if (existsSync(`${out}/run.json`))
	throw Error(`Existing measurement at ${out}; choose a new --out directory`);
const build = resolve(process.env.STE_BUILD || `${repo}/build-ste-scumm-static4`);
const hatari = resolve(process.env.HATARI || `${repo}/../F030Arcade/third_party/hatari/build/src/hatari`);
const tos = resolve(process.env.TOS || `${repo}/../F030Arcade/third_party/tos/tos206de.img`);
const nm = resolve(process.env.NM || `${repo}/../cross-mint/bin/m68k-atari-mintelf-nm`);
const hd = `${out}/HD`, gameDir = `${hd}/SCUMMVM`, prg = `${gameDir}/SCUMMVM.PRG`;
mkdirSync(gameDir, { recursive: true });
copyFileSync(`${build}/scummvm.prg`, prg);
// 'junction' needs no extra rights on Windows; other systems ignore the type.
for (const name of ['MONKEY', 'ATLANTIS'])
	if (!existsSync(`${gameDir}/${name}`)) symlinkSync(`${build}/HD/SCUMMVM/${name}`, `${gameDir}/${name}`, 'junction');
let ini = readFileSync(`${build}/HD/SCUMMVM/SCUMMVM.INI`, 'utf8').replace('debuglevel=9', 'debuglevel=0');
ini = ini.replace('[monkey]', `[monkey]\nboot_param=28\nste_benchmark=true\nste_benchmark_walk=${!process.argv.includes('--idle')}\nste_raster_60hz=false\nste_raster_lines=144`);
writeFileSync(`${gameDir}/SCUMMVM.INI`, ini);
const symbols = execFileSync(nm, [prg], { encoding: 'utf8' });
writeFileSync(`${out}/symbols.txt`, symbols);
writeFileSync(`${out}/code-symbols.txt`, [...symbols.matchAll(/^([0-9a-f]+) [TtWw] (.+)$/gm)].map(m => `${m[1]} T ${m[2]}`).join('\n') + '\n');
const offsets = Object.fromEntries([...symbols.matchAll(/^([0-9a-f]+) [a-zA-Z] (.+)$/gm)].map(m => [m[2], parseInt(m[1], 16)]));
const addr = name => {
	if (!(name in offsets)) throw Error(`Missing benchmark symbol: ${name}`);
	return `TEXT+0x${offsets[name].toString(16)}`;
};
const state = addr('atari_ste_bench_state'), marker = addr('atari_ste_bench_marker');
const field = i => `(${state}+${i * 4})`;
const memory = i => `('${state}+${i * 4}').l`;
const write = (name, lines) => writeFileSync(`${out}/${name}.ini`, lines.join('\n') + '\n');
const snapshot = [
	'echo STE_BENCH_EVENT', 'evaluate CycleCounter', 'evaluate TEXT',
	`savebin ${out}/state-'${field(1)}'.bin '${state}' #76`,
];
// :trace keeps the debugger from stopping for console input at these breakpoints.
write('boot', [`b GemdosOpcode = 0x4b && OsCallParam = 0 :once :trace :file ${out}/loaded.ini`]);
write('loaded', [`b pc = TEXT :once :trace :file ${out}/start.ini`]);
write('start', [
	`symbols ${out}/code-symbols.txt TEXT`,
	`b pc = '${marker}' && ${memory(3)} = ${first} && ${memory(2)} = 1 :once :trace :file ${out}/begin.ini`,
	`b VBL = 100000 :once :trace :file ${out}/timeout.ini`,
]);
write('begin', [
	...snapshot,
	`b pc = '${marker}' && ${memory(3)} < ${last} :trace :file ${out}/event.ini`,
	`b pc = '${marker}' && ${memory(3)} = ${last} && ${memory(2)} = 1 :once :trace :file ${out}/done.ini`,
	'profile on',
]);
write('event', [...snapshot, `profile save ${out}/segment-'${field(1)}'.txt`, 'profile on']);
write('done', [
	...snapshot, `profile save ${out}/segment-'${field(1)}'.txt`, 'profile off',
	`savebin ${out}/screen.bin '(${addr('atari_ste_raster_last_screen')})' #32000`,
	`savebin ${out}/schedule.bin '(${addr('atari_ste_raster_last_palette')})' #13696`,
	`screenshot ${out}/room.png`, 'echo STE_BENCH_DONE', 'quit',
]);
write('timeout', ['echo STE_BENCH_TIMEOUT', `screenshot ${out}/timeout.png`, 'quit']);
writeFileSync(`${out}/run.json`, JSON.stringify({ first, last, walk: !process.argv.includes('--idle'),
	prgSha256: createHash('sha256').update(readFileSync(prg)).digest('hex'),
	audio: 'compiled AtariSilentMixer', profileFormat: 'Hatari per-instruction cycles; split at engine/renderer markers' }, null, 2));
const args = ['--configfile', `${out}/hatari.cfg`, '--tos', tos, '--harddrive', hd,
	'--machine', 'ste', '--monitor', 'rgb', '--memsize', '4', '--cpulevel', '0', '--cpuclock', '8',
	'--cpu-exact', 'on', '--compatible', 'on', '--sound', 'off', '--fast-boot', 'on',
	'--confirm-quit', 'off', '--fast-forward', 'on', '--frameskips', '0', '--spec512', '1',
	'--borders', 'off', '--statusbar', 'off', '--drive-led', 'off', '--zoom', '1',
	'--conout', '2', '--run-vbls', '100100', '--parse', `${out}/boot.ini`, '--auto', 'C:\\SCUMMVM\\SCUMMVM.PRG'];
console.log(`Room 28, frames ${first}..${last - 1}, ${process.argv.includes('--idle') ? 'idle' : 'walking'}; ${out}`);
// An empty configuration file, because Hatari cannot use /dev/null on Windows.
writeFileSync(`${out}/hatari.cfg`, '');
writeFileSync(`${out}/hatari.log`, '');
const child = spawn(hatari, args, { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy', SDL_RENDER_DRIVER: 'software' } });
for (const stream of [child.stdout, child.stderr]) stream.on('data', data => {
	appendFileSync(`${out}/hatari.log`, data);
});
child.on('close', (code, signal) => {
	const done = readFileSync(`${out}/hatari.log`, 'utf8').includes('STE_BENCH_DONE>');
	console.log(`Hatari exit ${code ?? signal}; snapshots complete=${done}`);
	if (code || signal || !done) process.exitCode = 1;
});
