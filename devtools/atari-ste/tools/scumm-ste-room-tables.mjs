// One colour-mixing table per room, for the STE port to install when a room
// loads (AtariGraphicsManager::steSetRoom).
//
// Per room it lists the colours the room can show (scumm-scene-colours.mjs)
// and fits the two palettes to them (monkey-flicker-compare.mjs
// --room-colours), then writes the table as R<room>.BIN. Everything comes from
// the game files, so rooms that were never captured get a table too; where the
// engine overwrites palette entries, the port falls back to nearest pairs for
// those entries alone.
//
//   node devtools/atari-ste/tools/scumm-ste-room-tables.mjs --game atlantis --rooms 64,68
//     [--all] [--data-dir DIR] [--verb-bar 0,3,8,...] [--capture DIR] [--dl 0.2]
//     [--colour-floor 64] [--out DIR] [--install DIR]
//
// --verb-bar names the palette entries the engine's verb bar uses, measured
// once with --capture in any room of the game; --capture passes a capture
// directory to the colour list instead, which also names the costumes drawn
// there. --install copies the tables next to the default table on the staged
// drive.
import { execFileSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadMonkeyContainers } from './monkey-resource.mjs';

const REPO = fileURLToPath(new URL('../../..', import.meta.url));
const TOOLS = fileURLToPath(new URL('.', import.meta.url));
const arg = (name, fallback = null) =>
	process.argv.includes(name) ? process.argv[process.argv.indexOf(name) + 1] : fallback;
const game = arg('--game', 'monkey');
const dataDir = resolve(arg('--data-dir')
	?? [`${REPO}assets/${game}-cd`, `${REPO}../scummvm/assets/${game}-cd`].find(existsSync)
	?? `${REPO}assets/${game}-cd`);
const dl = arg('--dl', '0.2');
const floor = arg('--colour-floor', '64');
const verbBar = arg('--verb-bar');
const capture = arg('--capture');
const out = resolve(arg('--out', `${REPO}devtools/atari-ste/monkey-bar/${game}-room-tables`));
const install = arg('--install') ? resolve(arg('--install')) : null;

// --all takes the rooms from the data file's room table.
const allRooms = () => {
	const names = readdirSync(dataDir).filter(name => /\.00[01]$/i.test(name)).sort();
	const containers = loadMonkeyContainers(dataDir, names);
	return containers.find(container => container.loff.length)
		.loff.map(entry => entry.resourceId).sort((a, b) => a - b);
};
const rooms = process.argv.includes('--all')
	? allRooms()
	: (arg('--rooms') ?? '').split(',').filter(Boolean).map(Number);
if (!rooms.length) throw Error('--rooms or --all is required');

mkdirSync(out, { recursive: true });
if (install) mkdirSync(install, { recursive: true });
const run = args => execFileSync(process.execPath, args, { encoding: 'utf8', maxBuffer: 64 << 20 });
const built = [], failed = [];

for (const room of rooms) {
	const roomOut = `${out}/room${String(room).padStart(3, '0')}`;
	const colours = `${roomOut}/colours.json`;
	mkdirSync(roomOut, { recursive: true });
	try {
		run([`${TOOLS}scumm-scene-colours.mjs`, '--game', game, '--room', String(room), '--data-dir', dataDir,
			...(capture ? ['--capture', capture] : []), ...(verbBar ? ['--verb-bar', verbBar] : []),
			'--out', colours]);
		run([`${TOOLS}monkey-flicker-compare.mjs`, '--room-colours', colours, '--split', '144',
			'--dl', dl, '--sheet-dl', dl, '--colour-floor', floor, '--out', roomOut]);
		const table = `${roomOut}/lut-dual16-dl${Number(dl).toFixed(2)}.bin`;
		const name = `R${String(room).padStart(3, '0')}.BIN`;
		copyFileSync(table, `${out}/${name}`);
		if (install) copyFileSync(table, `${install}/${name}`);
		const union = JSON.parse(readFileSync(colours, 'utf8'));
		built.push({ room, table: name, colours: union.indices.present,
			costumeOnly: union.indices.costumeOnly.length,
			costumes: union.costumes.map(costume => costume.id), skipped: union.skippedCostumes.length });
		console.log(`room ${room}: ${union.indices.present} colours, ${union.costumes.length} costumes -> ${name}`);
	} catch (error) {
		failed.push({ room, reason: (error.stderr || error.message).toString().trim().split('\n').at(-1) });
		console.log(`room ${room}: FAILED ${failed.at(-1).reason}`);
	}
}

writeFileSync(`${out}/room-tables.json`, JSON.stringify({
	schema: 'scumm-ste-room-tables/1',
	game, dataDir, dl: Number(dl), colourFloor: Number(floor),
	verbBar: verbBar ? verbBar.split(',').map(Number) : null,
	capture: capture ?? null, built, failed,
}, null, 2) + '\n');
console.log(`${built.length} tables in ${out}${install ? `, installed in ${install}` : ''}; ${failed.length} failed`);
