import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

process.chdir(fileURLToPath(new URL('..', import.meta.url)));

const vasm = process.env.VASM || resolve('../../../F030Arcade/third_party/vasm/vasmm68k_mot');
const hatari = process.env.HATARI || resolve('../../../F030Arcade/third_party/hatari/build/src/hatari');
const tos = process.env.TOS || resolve('../../../F030Arcade/third_party/tos/tos206de.img');
const output = mkdtempSync(resolve(tmpdir(), 'ste-monkey-native-benchmark-'));

execFileSync(vasm, [
	'monkey-bar/monkey-viewer.s',
	'-m68000',
	'-Ftos',
	'-L', 'monkey-bar/monkey-viewer.lst',
	'-o', 'monkey-bar/MONKEY.PRG',
], { stdio: 'inherit' });

const prg = readFileSync('monkey-bar/MONKEY.PRG');
const textSize = prg.readUInt32BE(2);
const dataSize = prg.readUInt32BE(6);
const bases = [0, textSize, textSize + dataSize];
const symbols = [...readFileSync('monkey-bar/monkey-viewer.lst', 'utf8').matchAll(/^(\w+)\s+(0[012]):([0-9A-F]{8})\s*$/gm)]
	.map(([, name, section, offset]) => `${(bases[Number(section)] + parseInt(offset, 16)).toString(16)} T ${name}`);
writeFileSync(`${output}/symbols`, symbols.join('\n') + '\n');

const writeIni = (name, commands) => writeFileSync(`${output}/${name}.ini`, `${commands}\n`);
writeIni('boot', `b GemdosOpcode = 0x4b && OsCallParam = 0 :trace :once :file ${output}/loaded.ini`);
writeIni('loaded', `b pc = TEXT :trace :once :file ${output}/start.ini`);
writeIni('start', `symbols ${output}/symbols TEXT
b pc = advance_frame :100 :trace :once :file ${output}/profile-start.ini`);
writeIni('profile-start', `echo MONKEY_PROFILE_BEGIN
evaluate CycleCounter
profile on
b pc = advance_frame :100 :trace :once :file ${output}/profile-end.ini`);
writeIni('profile-end', `echo MONKEY_PROFILE_END
evaluate CycleCounter
profile off
profile stats
profile cycles 12
quit`);

const args = [
	'--tos', tos,
	'--machine', 'ste',
	'--memsize', '4',
	'--cpulevel', '0',
	'--cpuclock', '8',
	'--cpu-exact', 'on',
	'--compatible', 'on',
	'--sound', 'off',
	'--fast-forward', 'on',
	'--fast-boot', 'on',
	'--frameskips', '0',
	'--spec512', '1',
	'--borders', 'off',
	'--statusbar', 'off',
	'--zoom', '1',
	'--confirm-quit', 'off',
	'--run-vbls', '5000',
	'--parse', `${output}/boot.ini`,
	'monkey-bar/MONKEY.PRG',
];
const run = spawnSync(hatari, args, {
	encoding: 'utf8',
	timeout: 60000,
	maxBuffer: 8 * 1024 * 1024,
	env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy', SDL_RENDER_DRIVER: 'software' },
});
const log = `${run.stdout || ''}${run.stderr || ''}`;
writeFileSync(`${output}/hatari.log`, log);
if (run.error || run.status !== 0) throw new Error(`Hatari benchmark failed; see ${output}/hatari.log`);
const cycleValues = [...log.matchAll(/evaluate CycleCounter[\s\S]{0,160}?#(\d+)/gi)].map(match => Number(match[1]));
const usedCycles = Number(log.match(/used cycles:\s+(\d+)/i)?.[1] ?? 0);
const profileSeconds = Number(log.match(/=\s+([0-9.]+)s\s*$/im)?.[1] ?? 0);
console.log(`Hatari benchmark log: ${output}/hatari.log`);
if (cycleValues.length >= 2) {
	const delta = (cycleValues[1] - cycleValues[0]) >>> 0;
	console.log(`CycleCounter delta: ${delta} cycles / 1000 updates = ${(delta / 1000).toFixed(0)} cycles/update (${(8000000 / (delta / 1000)).toFixed(2)} theoretical updates/sec).`);
}
if (usedCycles && profileSeconds) console.log(`Hatari profile: ${usedCycles} cycles over ${profileSeconds}s.`);
console.log('The renderer is presented on the 60 Hz VBL path; 10 fps corresponds to one completed update every 6 VBLs.');
