// Capture finished SCUMM frames and their live palette from the STE port.
//
// Boots a game in cycle-exact Hatari and, once each requested VBL has passed,
// dumps the engine's completed 320x200 chunky frame together with the palette
// the backend converts it with. By default it boots Monkey Island room 28 with
// the benchmark hooks and breaks at the renderer marker (phase 3, just before
// conversion). The hooks only run in room 28, so in other rooms it breaks at
// the entry of AtariSteSceneRenderer::convert instead and traces
// ScummEngine::startScene for the room and ClassicCostumeLoader::loadCostume
// for the costumes drawn there, which scumm-scene-colours.mjs takes with
// --capture. The directory is the --capture input of
// monkey-flicker-compare.mjs. From the repository root, after building the
// STE executable:
//
//   node devtools/atari-ste/tools/scumm-ste-frame-capture.mjs [--out DIR] [--vbls 12000,12400,...]
//     [--game TARGET] [--boot-param N] [--room N]
//
// --game starts a section of the build's HD/SCUMMVM/SCUMMVM.INI (default monkey)
// and links the game folder its path names from that HD. --boot-param defaults
// to 28 for monkey; --room is the room every capture must show (default 28 for
// Monkey Island room 28). Other targets need --vbls. Fate of Atlantis room 64,
// the Algiers bazaar street, which follows the LucasArts logo:
//
//   node devtools/atari-ste/tools/scumm-ste-frame-capture.mjs --game atlantis --boot-param 9554 --room 64
//     --vbls 44000,44400,44800,45200,45600,46000,46400,46800 --out devtools/atari-ste/monkey-bar/atlantis-room64-capture
import { appendFileSync, copyFileSync, existsSync, mkdirSync, readFileSync, symlinkSync, writeFileSync } from 'node:fs';
import { execFileSync, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writePng } from './png.mjs';

const kit = fileURLToPath(new URL('..', import.meta.url));
const repo = resolve(kit, '../..');
const arg = (name, fallback) => process.argv.includes(name) ? process.argv[process.argv.indexOf(name) + 1] : fallback;
const game = arg('--game', 'monkey');
const bootParam = arg('--boot-param', game === 'monkey' ? '28' : null);
const room = arg('--room', game === 'monkey' && bootParam === '28' ? '28' : null);
for (const [name, value] of [['--boot-param', bootParam], ['--room', room]])
	if (value !== null && !/^-?\d+$/.test(value)) throw Error(`${name} needs an integer`);
// The benchmark hooks in ScummEngine::scummLoop only run in room 28.
const benchmark = room === '28';
const defaultTarget = game === 'monkey' && benchmark;
if (!defaultTarget && !process.argv.includes('--vbls'))
	throw Error('--vbls is required outside Monkey Island room 28');
const out = resolve(arg('--out', `${kit}/monkey-bar/${defaultTarget ? 'room28' : `${game}${room === null ? '' : `-room${room}`}`}-capture`));
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

const iniFile = `${build}/HD/SCUMMVM/SCUMMVM.INI`;
const sectionOf = (lines, section) => {
	const start = lines.findIndex(line => line.trim() === `[${section}]`);
	if (start < 0) throw Error(`No [${section}] section in ${iniFile}`);
	const next = lines.findIndex((line, i) => i > start && line.trim().startsWith('['));
	return [start, next < 0 ? lines.length : next];
};
const keyOf = line => line.split('=')[0].trim();
// Sets keys at the top of a section, replacing earlier values; null removes a key.
const setKeys = (lines, section, entries) => {
	const [start, end] = sectionOf(lines, section);
	const kept = lines.slice(start + 1, end).filter(line => !Object.hasOwn(entries, keyOf(line)));
	const set = Object.entries(entries).filter(([, value]) => value !== null).map(([key, value]) => `${key}=${value}`);
	return [...lines.slice(0, start + 1), ...set, ...kept, ...lines.slice(end)];
};
let ini = readFileSync(iniFile, 'utf8').replace('debuglevel=9', 'debuglevel=0').split(/\r?\n/);
ini = setKeys(ini, 'scummvm', { lastselectedgame: game });
ini = setKeys(ini, game, { boot_param: bootParam, ste_benchmark: benchmark ? 'true' : null, ste_benchmark_walk: benchmark ? 'false' : null });
const [gameStart, gameEnd] = sectionOf(ini, game);
const gamePath = ini.slice(gameStart + 1, gameEnd).find(line => keyOf(line) === 'path')?.replace(/^[^=]*=/, '').trim();
const folder = gamePath?.split(/[\\/]/).filter(Boolean).pop();
if (!folder || !existsSync(`${build}/HD/SCUMMVM/${folder}`))
	throw Error(`The path of [${game}] (${gamePath}) names no game folder in ${build}/HD/SCUMMVM`);
mkdirSync(gameDir, { recursive: true });
copyFileSync(`${build}/scummvm.prg`, prg);
// 'junction' needs no extra rights on Windows; other systems ignore the type.
if (!existsSync(`${gameDir}/${folder}`)) symlinkSync(`${build}/HD/SCUMMVM/${folder}`, `${gameDir}/${folder}`, 'junction');
writeFileSync(`${gameDir}/SCUMMVM.INI`, ini.join('\n'));

const symbols = execFileSync(nm, [prg], { encoding: 'utf8', maxBuffer: 64 << 20 });
writeFileSync(`${out}/code-symbols.txt`, [...symbols.matchAll(/^([0-9a-f]+) [TtWw] (.+)$/gm)].map(m => `${m[1]} T ${m[2]}`).join('\n') + '\n');
const offsets = Object.fromEntries([...symbols.matchAll(/^([0-9a-f]+) [a-zA-Z] (.+)$/gm)].map(m => [m[2], parseInt(m[1], 16)]));
const addr = name => {
	if (!(name in offsets)) throw Error(`Missing capture symbol: ${name}`);
	return `TEXT+0x${offsets[name].toString(16)}`;
};
// C++ functions are found by their mangled name without the parameter types.
const addrOfPrefix = prefix => {
	const names = Object.keys(offsets).filter(name => name.startsWith(prefix));
	if (names.length !== 1) throw Error(`Expected one capture symbol starting with ${prefix}, found ${names.length}`);
	return addr(names[0]);
};
const state = addr('atari_ste_bench_state'), marker = addr('atari_ste_bench_marker');
const source = addr('atari_ste_last_source'), palette = addr('atari_ste_last_palette');
const convert = addrOfPrefix('_ZN21AtariSteSceneRenderer7convertE');
const startScene = addrOfPrefix('_ZN5Scumm11ScummEngine10startSceneE');
// The V5 costume renderer loads the costume of every actor it draws, so this
// fires for whoever is on screen, not only when a script changes a costume.
const loadCostume = addr('_ZN5Scumm20ClassicCostumeLoader11loadCostumeEi');
const write = (name, lines) => writeFileSync(`${out}/${name}.ini`, lines.join('\n') + '\n');
// Phase 3 of the benchmark marker is the start of a conversion. Elsewhere the
// entry of AtariSteSceneRenderer::convert is the same point: the capture HD has
// no MIX tables, so the port converts for the raster. Either way the engine
// frame is complete.
const atConversion = vbl => benchmark
	? `b pc = '${marker}' && ('${state}+8').l = 3 && VBL > ${vbl} :once :trace`
	: `b pc = '${convert}' && VBL > ${vbl} :once :trace`;
// :trace keeps the debugger from stopping for console input at these breakpoints.
write('boot', [`b GemdosOpcode = 0x4b && OsCallParam = 0 :once :trace :file ${out}/loaded.ini`]);
write('loaded', [`b pc = TEXT :once :trace :file ${out}/start.ini`]);
write('start', [
	`symbols ${out}/code-symbols.txt TEXT`,
	...(benchmark ? [] : [
		`b pc = '${startScene}' :trace :file ${out}/room.ini`,
		`b pc = '${loadCostume}' :trace :file ${out}/costume.ini`,
	]),
	`${atConversion(vbls[0])} :file ${out}/capture-0.ini`,
	`b VBL = ${vbls.at(-1) + 5000} :once :trace :file ${out}/timeout.ini`,
]);
// At the entry of startScene(int room, Actor *, int), a7 points at the return
// address, then this, then the room. ClassicCostumeLoader::loadCostume(int)
// has the same layout, so its costume is (a7+8) as well: together they say
// which costumes were drawn while each room was on screen.
if (!benchmark) {
	write('room', ['echo STE_ROOM', 'evaluate (a7+8)', 'evaluate VBL']);
	write('costume', ['echo STE_COSTUME', 'evaluate (a7+8)', 'evaluate VBL']);
}
vbls.forEach((vbl, k) => write(`capture-${k}`, [
	'echo STE_CAPTURE',
	'evaluate VBL',
	...(benchmark ? [`savebin ${out}/state-${k}.bin '${state}' #76`] : []),
	`savebin ${out}/frame-${k}.bin '(${source})' #64000`,
	`savebin ${out}/palette-${k}.xrgb.bin '(${palette})' #1024`,
	...(k + 1 < vbls.length
		? [`${atConversion(vbls[k + 1])} :file ${out}/capture-${k + 1}.ini`]
		: ['echo STE_CAPTURE_DONE', 'quit']),
]));
write('timeout', ['echo STE_CAPTURE_TIMEOUT', `screenshot ${out}/timeout.png`, 'quit']);

const args = ['--configfile', `${out}/hatari.cfg`, '--tos', tos, '--harddrive', hd,
	'--machine', 'ste', '--monitor', 'rgb', '--memsize', '4', '--cpulevel', '0', '--cpuclock', '8',
	'--cpu-exact', 'on', '--compatible', 'on', '--sound', 'off', '--fast-boot', 'on',
	'--confirm-quit', 'off', '--fast-forward', 'on', '--frameskips', '0', '--spec512', '1',
	'--borders', 'off', '--statusbar', 'off', '--zoom', '1',
	'--conout', '2', '--run-vbls', String(vbls.at(-1) + 5100), '--parse', `${out}/boot.ini`, '--auto', 'C:\\SCUMMVM\\SCUMMVM.PRG'];
const target = `[${game}]${bootParam === null ? '' : ` boot_param ${bootParam}`}${room === null ? '' : ` room ${room}`}`;
console.log(`${target} frame capture after VBLs ${vbls.join(',')} at the ${benchmark ? 'benchmark marker' : 'converter entry'}; ${out}`);
// An empty configuration file, because Hatari cannot use /dev/null on Windows.
writeFileSync(`${out}/hatari.cfg`, '');
writeFileSync(`${out}/hatari.log`, '');
const child = spawn(hatari, args, { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy', SDL_RENDER_DRIVER: 'software' } });
for (const stream of [child.stdout, child.stderr]) stream.on('data', data => appendFileSync(`${out}/hatari.log`, data));
// The costumes drawn while each room was on screen, in id order.
const costumesByRoom = entries => {
	const byRoom = new Map();
	for (const { costume, room: at } of entries) {
		if (at === null) continue;
		if (!byRoom.has(at)) byRoom.set(at, new Set());
		byRoom.get(at).add(costume);
	}
	return [...byRoom].map(([at, ids]) => ({ room: at, costumes: [...ids].sort((a, b) => a - b) }));
};

child.on('close', (code, signal) => {
	const log = readFileSync(`${out}/hatari.log`, 'utf8');
	if (code || signal || !log.includes('STE_CAPTURE_DONE>')) {
		console.log(`Hatari exit ${code ?? signal}; capture incomplete, see ${out}/hatari.log`);
		process.exitCode = 1;
		return;
	}
	// echo adds no newline, so a marker shares its line with the next command;
	// evaluate prints its result as "= ... #N (dec) ...".
	const rooms = [], costumes = [], atCapture = [];
	let label = null, values = [];
	for (const line of log.split(/\r?\n/)) {
		if (line.startsWith('STE_ROOM>') || line.startsWith('STE_COSTUME>') || line.startsWith('STE_CAPTURE>')) {
			label = line.slice(0, line.indexOf('>'));
			values = [];
		}
		const value = label && line.match(/^= .*?#(-?\d+) \(dec\)/);
		if (!value) continue;
		values.push(Number(value[1]));
		if (label === 'STE_CAPTURE') {
			atCapture.push({ vbl: values[0], room: rooms.at(-1)?.room ?? null });
			label = null;
		} else if (values.length === 2) {
			if (label === 'STE_ROOM')
				rooms.push({ room: values[0], vbl: values[1] });
			else
				costumes.push({ costume: values[0], vbl: values[1], room: rooms.at(-1)?.room ?? null });
			label = null;
		}
	}
	// _RGB entries carry a reserved byte before red, green and blue.
	const captures = vbls.map((vbl, k) => {
		const frame = readFileSync(`${out}/frame-${k}.bin`);
		const xrgb = readFileSync(`${out}/palette-${k}.xrgb.bin`), rgb = Buffer.alloc(256 * 3);
		for (let i = 0; i < 256; i++) xrgb.copy(rgb, i * 3, i * 4 + 1, i * 4 + 4);
		writeFileSync(`${out}/palette-${k}.rgb.bin`, rgb);
		const preview = new Uint8Array(frame.length * 3);
		for (let i = 0; i < frame.length; i++) rgb.copy(preview, i * 3, frame[i] * 3, frame[i] * 3 + 3);
		writePng(`${out}/frame-${k}.png`, 320, 200, preview);
		const benchmarkState = benchmark ? readFileSync(`${out}/state-${k}.bin`) : null;
		return {
			afterVbl: vbl, vbl: atCapture[k]?.vbl ?? null,
			room: benchmarkState ? benchmarkState.readUInt32BE(16) : atCapture[k]?.room ?? null,
			roomFrame: benchmarkState ? benchmarkState.readUInt32BE(12) : null,
			frame: `frame-${k}.bin`, palette: `palette-${k}.rgb.bin`, preview: `frame-${k}.png`,
		};
	});
	const palettes = captures.map(c => readFileSync(`${out}/${c.palette}`));
	const manifest = {
		schema: 'spectrum512-scumm-ste-frame-capture/1',
		game, bootParam: bootParam === null ? null : Number(bootParam),
		prgSha256: createHash('sha256').update(readFileSync(prg)).digest('hex'),
		source: `atari_ste_last_source (320x200 chunky) and atari_ste_last_palette (_RGB[256]) ${benchmark ? 'at benchmark phase 3' : 'at the entry of AtariSteSceneRenderer::convert'}`,
		palettesIdentical: palettes.every(p => p.equals(palettes[0])),
		...(benchmark ? {} : { rooms, costumesByRoom: costumesByRoom(costumes) }),
		captures,
	};
	writeFileSync(`${out}/capture.json`, JSON.stringify(manifest, null, 2) + '\n');
	const traced = manifest.costumesByRoom?.find(entry => entry.room === Number(room));
	if (traced) console.log(`Costumes drawn in room ${room}: ${traced.costumes.join(',')}`);
	console.log(JSON.stringify({ palettesIdentical: manifest.palettesIdentical, captures: captures.map(c => [c.afterVbl, c.vbl, c.room, c.roomFrame]) }));
	if (!benchmark) console.log(`Rooms entered: ${rooms.map(r => `${r.room} at VBL ${r.vbl}`).join(', ') || 'none traced'}`);
	if (room !== null && captures.some(c => c.room !== Number(room))) {
		console.log(`Not every capture is in room ${room}`);
		process.exitCode = 1;
	}
});
