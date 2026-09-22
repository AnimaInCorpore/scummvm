// Every colour a SCUMM V5 room can show, with a pixel weight per palette index.
//
// The mixing tables are fitted to captured frames, which only hold the actors
// that happened to be on screen in the poses they happened to be in. This tool
// reads the game files instead and counts, per palette index, the pixels of
//
//   - the room background over its full width,
//   - every object image of the room, transparent pixels excluded,
//   - every animation step of every costume the room can present, averaged per
//     step so a long walk cycle does not outweigh a short one.
//
// monkey-flicker-compare.mjs takes the result with --union: colours that can
// appear are then targets of the palette fit even when no captured frame
// showed them. The index is what is counted; the RGB values come from the
// live palette of the capture, because the engine sets part of the palette
// itself.
//
//   node devtools/atari-ste/tools/scumm-scene-colours.mjs --game atlantis --room 64
//     [--data-dir DIR] [--capture DIR] [--costumes 1,2,...] [--split 144]
//     [--verb-bar 0,3,8,...] [--out FILE]
//
// Costumes come from three sources. The costume directory says which ones the
// room file owns, which is where a room keeps its own actors. Actors that walk
// in from elsewhere own their costume in another room, so --capture reads the
// costumes scumm-ste-frame-capture.mjs saw drawn in this room in the running
// game, and --costumes names any further ones by hand. None of the three
// proves that no other actor can ever enter.
import { existsSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadMonkeyContainers, parseChildren, resolveRoom } from './monkey-resource.mjs';
import { costumeColourCounts, costumeDirectory, loadCostume } from './monkey-costume.mjs';
import { parseRoom } from './monkey-room.mjs';

const REPO = fileURLToPath(new URL('../../..', import.meta.url));
const arg = (name, fallback = null) =>
	process.argv.includes(name) ? process.argv[process.argv.indexOf(name) + 1] : fallback;
const integers = (value, name) => {
	const list = value.split(',').map(part => part.trim()).filter(Boolean);
	if (!list.every(part => /^\d+$/.test(part))) throw Error(`${name} needs integers`);
	return list.map(Number);
};

const game = arg('--game', 'monkey');
const room = Number(arg('--room', game === 'monkey' ? '28' : null));
if (!Number.isInteger(room) || room < 1) throw Error('--room needs a room number');
const dataDir = resolve(arg('--data-dir')
	?? [`${REPO}assets/${game}-cd`, `${REPO}../scummvm/assets/${game}-cd`].find(existsSync)
	?? `${REPO}assets/${game}-cd`);
const listed = arg('--costumes') ? integers(arg('--costumes'), '--costumes') : [];
const capture = arg('--capture') ? resolve(arg('--capture')) : null;
const split = Number(arg('--split', '144'));
if (!Number.isInteger(split) || split < 1 || split > 200) throw Error('--split needs a line inside the screen');
const verbBarListed = arg('--verb-bar') ? integers(arg('--verb-bar'), '--verb-bar') : null;
const drawn = capture
	? (JSON.parse(readFileSync(`${capture}/capture.json`, 'utf8')).costumesByRoom ?? [])
		.find(entry => entry.room === room)?.costumes ?? []
	: [];
if (capture && !drawn.length) console.log(`No costumes traced in room ${room} in ${capture}`);

// The verb bar is drawn by the engine, so its colours come from a capture of
// the running game, or from a list measured in another room of the same game.
const verbBarFromCapture = () => {
	const manifest = JSON.parse(readFileSync(`${capture}/capture.json`, 'utf8'));
	const seen = new Set();
	for (const entry of manifest.captures) {
		const frame = readFileSync(`${capture}/${entry.frame}`);
		for (let y = split; y < 200; y++)
			for (let x = 0; x < 320; x++) seen.add(frame[y * 320 + x]);
	}
	return [...seen].sort((a, b) => a - b);
};
const verbBar = verbBarListed ?? (capture ? verbBarFromCapture() : []);
const out = resolve(arg('--out', `${REPO}devtools/atari-ste/monkey-bar/${game}-room${room}-colours.json`));

// A SCUMM V5 game is one index file and one data file next to each other.
const names = readdirSync(dataDir).filter(name => /\.00[01]$/i.test(name)).sort();
if (names.length !== 2) throw Error(`Expected one .000 and one .001 in ${dataDir}, found ${names.length}`);
const containers = loadMonkeyContainers(dataDir, names);
const { container, room: roomBlock } = resolveRoom(containers, room);
const parsed = parseRoom(container.bytes, roomBlock, { roomId: room });
const transparent = parsed.paletteBlocks.trns?.transparentIndex ?? null;

const zeros = () => new Float64Array(256);
const background = zeros(), objects = zeros(), costumeWeights = zeros();

for (const index of parsed.background.pixels) background[index]++;

for (const bitmap of parsed.objectBitmaps) {
	for (const index of bitmap.bitmap.pixels) {
		// An object image is drawn over the background, so its transparent
		// pixels never reach the screen.
		if (index !== transparent) objects[index]++;
	}
}

const owned = costumeDirectory(containers).filter(resource => resource.room === room).map(resource => resource.id);
const wanted = [...new Set([...owned, ...drawn, ...listed])].sort((a, b) => a - b);
const sourceOf = id => owned.includes(id) ? 'room' : drawn.includes(id) ? 'capture' : 'listed';
const costumes = [];

const skipped = [];

for (const id of wanted) {
	let costume;
	try {
		({ costume } = loadCostume(containers, id));
	} catch (error) {
		// A costume the room mentions but this decoder cannot read leaves its
		// colours out of the union; the table then falls back for them.
		skipped.push({ id, source: sourceOf(id), reason: error.message });
		continue;
	}
	const { counts, steps, cels, rejected } = costumeColourCounts(costume);
	const perIndex = new Map();

	for (const [color, pixels] of counts) {
		const index = costume.palette[color] ?? 255;
		perIndex.set(index, (perIndex.get(index) ?? 0) + pixels);
	}

	// Average over the steps: what one actor of this costume covers on screen.
	const colours = [];
	for (const [index, pixels] of [...perIndex].sort((a, b) => b[1] - a[1])) {
		const weight = steps ? pixels / steps : 0;
		costumeWeights[index] += weight;
		colours.push({ index, pixelsPerStep: Number(weight.toFixed(2)) });
	}
	costumes.push({
		id,
		source: sourceOf(id),
		format: `0x${costume.format.toString(16)}`,
		paletteEntries: costume.numColors,
		steps, cels, rejectedCels: rejected,
		colours,
	});
}

const total = zeros();
for (let index = 0; index < 256; index++) total[index] = background[index] + objects[index] + costumeWeights[index];
const present = [...total.keys()].filter(index => total[index] > 0);
const costumeOnly = present.filter(index => costumeWeights[index] > 0 && background[index] + objects[index] === 0);

const round = weights => [...weights].map(weight => Number(weight.toFixed(2)));
writeFileSync(out, JSON.stringify({
	schema: 'scumm-ste-scene-colours/1',
	game, room, dataDir,
	containers: containers.map(({ name, sha256 }) => ({ name, sha256 })),
	width: parsed.width, height: parsed.height, transparentIndex: transparent,
	// The room's own palette. The engine overwrites entries in some games, so
	// a table built from this one falls back to nearest pairs for those.
	palette: [...parsed.palette],
	verbBar: { split, source: verbBarListed ? 'listed' : capture ? 'capture' : 'none', indices: verbBar },
	weights: 'pixels per palette index: the background once, every object image once, every costume averaged over its animation steps',
	costumes,
	skippedCostumes: skipped,
	indices: {
		present: present.length,
		costumeOnly,
		background: round(background),
		objects: round(objects),
		costumes: round(costumeWeights),
		total: round(total),
	},
}, null, 2) + '\n');

console.log(`room ${room} of ${game}: ${parsed.width}x${parsed.height}, ${present.length} palette indices can appear`);
console.log(`costumes ${wanted.join(',') || 'none'} (${owned.length} owned by the room, ${drawn.length} drawn in the capture, ${listed.length} listed)`);
console.log(`${costumeOnly.length} indices only a costume uses: ${costumeOnly.join(',') || 'none'}`);
console.log(`verb bar from line ${split}: ${verbBar.length} indices (${verbBarListed ? 'listed' : capture ? 'measured in the capture' : 'none given'})`);
if (skipped.length) console.log(`skipped costumes: ${skipped.map(entry => `${entry.id} (${entry.reason})`).join(', ')}`);
const rejectedCels = costumes.reduce((sum, costume) => sum + costume.rejectedCels, 0);
if (rejectedCels) console.log(`${rejectedCels} cels larger than the screen were left out`);
console.log(out);
