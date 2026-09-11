import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { execFileSync, spawnSync } from 'node:child_process';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readPng, writePng } from './png.mjs';

process.chdir(fileURLToPath(new URL('..', import.meta.url)));
const output = resolve(process.argv[2] || 'monkey-bar/native-run');
mkdirSync(output, { recursive: true });
const write = (name, commands) => writeFileSync(`${output}/${name}.ini`, `${commands}\n`);
write('boot', `b GemdosOpcode = 0x4b && OsCallParam = 0 :trace :once :file ${output}/loaded.ini`);
write('loaded', `b pc = TEXT :trace :once :file ${output}/start.ini`);
write('start', `b VBL = 'VBL+40' :trace :once :file ${output}/capture-0.ini`);
for (let frame = 0; frame < 4; frame++) {
	const next = frame + 1;
	write(`capture-${frame}`, `screenshot ${output}/monkey-hatari-${frame}.png\n${next < 4 ? `b VBL = 'VBL+40' :trace :once :file ${output}/capture-${next}.ini` : 'quit'}`);
}
const hatari = process.env.HATARI || resolve('../../../F030Arcade/third_party/hatari/build/src/hatari');
const tos = process.env.TOS || resolve('../../../F030Arcade/third_party/tos/tos206de.img');
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
	'--max-width', '320',
	'--max-height', '200',
	'--crt', 'off',
	'--confirm-quit', 'off',
	'--run-vbls', '20000',
	'--parse', `${output}/boot.ini`,
	'monkey-bar/MONKEY.PRG',
];
const run = spawnSync(hatari, args, {
		cwd: resolve('.'),
		encoding: 'utf8',
		timeout: 60000,
		maxBuffer: 4 * 1024 * 1024,
		env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy', SDL_RENDER_DRIVER: 'software' },
	});
const log = (run.stdout || '') + (run.stderr || '');
writeFileSync(`${output}/hatari.log`, log);
if (run.error) throw run.error;
if (run.status !== 0) throw new Error(`Hatari exited with ${run.status}; see ${output}/hatari.log`);
const captures = Array.from({ length: 4 }, (_, frame) => readPng(`${output}/monkey-hatari-${frame}.png`));
if (captures.some(image => image.width !== captures[0].width || image.height !== captures[0].height)) throw new Error('Hatari captures have inconsistent dimensions');
const contact = new Uint8Array(captures[0].width * 2 * captures[0].height * 2 * 3);
for (let frame = 0; frame < captures.length; frame++) {
	const image = captures[frame];
	const panelX = (frame & 1) * image.width;
	const panelY = (frame >> 1) * image.height;
	for (let y = 0; y < image.height; y++) for (let x = 0; x < image.width; x++) {
		const source = (y * image.width + x) * 4;
		const target = ((panelY + y) * image.width * 2 + panelX + x) * 3;
		contact[target] = image.rgba[source];
		contact[target + 1] = image.rgba[source + 1];
		contact[target + 2] = image.rgba[source + 2];
	}
}
writePng(`${output}/monkey-hatari-contact.png`, captures[0].width * 2, captures[0].height * 2, contact);
console.log(`Hatari animation captures: ${output}/monkey-hatari-contact.png`);
