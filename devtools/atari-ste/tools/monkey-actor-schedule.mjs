// Union-aware per-line palette schedule for room 28, driven by the room's real
// actor set.
//
// The room's real actor costumes (DCOS room==28) are 24, 26 and 37. Costumes 26
// and 37 are floor walkers: the room's BOXD walkboxes put actor feet at screen
// rows 109..140, so each sweeps that band. Costume 24 is the fixed hanging
// object at its room anchor. For every scanline the solver takes the union of
// the background colours on that line and the colours of every costume whose
// reachable vertical band covers it, then picks one frozen 16-colour palette per
// line over that union. Because the schedule never changes at runtime, a moving
// actor can never recolour the scenery behind it, and revealed background always
// returns to its exact colour.
//
//   node devtools/atari-ste/tools/monkey-actor-schedule.mjs [--fixture DIR] [--out DIR]
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { distance, steWord } from './reference.mjs';
import { choosePalette, nearest } from './palette-select.mjs';
import { loadMonkeyContainers, parseBlock, parseChildren } from './monkey-resource.mjs';
import { renderCostumeFrame } from './monkey-costume.mjs';
import {
	loadMonkeyBarScene,
	MONKEY_BAR_CAMERA_X,
	MONKEY_BAR_SCENE_SPRITES,
	renderMonkeyBarSceneFrame,
} from './monkey-scene.mjs';
import { writePng } from './png.mjs';

const WIDTH = 320;
const HEIGHT = 144;
const ROOM_WIDTH = 640;
const FRAME_COUNT = 31;
const BUDGET = 16;

process.chdir(fileURLToPath(new URL('..', import.meta.url)));

function parseArgs(argv) {
	const out = { fixture: 'monkey-bar/fixture', out: 'monkey-bar/actor-schedule' };
	for (let i = 2; i < argv.length; i++) {
		if (argv[i] === '--fixture') out.fixture = argv[++i];
		else if (argv[i] === '--out') out.out = argv[++i];
	}
	return out;
}

const q = v => Math.max(0, Math.min(15, Math.round(v / 17)));
const idToRgb = id => [(id >> 8) * 17, ((id >> 4) & 15) * 17, (id & 15) * 17];
const clutToId = (pal, index) => {
	if (index === 255) return 0;
	const o = index * 3;
	return o + 2 < pal.length ? (q(pal[o]) << 8) | (q(pal[o + 1]) << 4) | q(pal[o + 2]) : 0;
};

function readBackground(fixtureDir) {
	const index = readFileSync(`${fixtureDir}/background-640x144.index.bin`);
	const palette = readFileSync(`${fixtureDir}/room-palette.rgb.bin`);
	const source = new Uint16Array(index.length);
	for (let i = 0; i < index.length; i++) source[i] = clutToId(palette, index[i]);
	return { source, palette };
}

// Read the room's walkbox feet-Y range straight from BOXD (skips the sentinel
// gate box at -32000), so the actor bands come from the real room geometry.
function readFloorRange() {
	const c001 = loadMonkeyContainers().find(c => c.name === 'MONKEY.001');
	const room = parseBlock(c001.bytes, 1342104);
	const boxd = parseChildren(c001.bytes, room).find(k => k.tag === 'BOXD');
	const b = c001.bytes;
	const s16 = p => { const v = b[p] | (b[p + 1] << 8); return v & 0x8000 ? v - 0x10000 : v; };
	let o = boxd.offset + 8;
	const n = b[o] | (b[o + 1] << 8); o += 2;
	let min = Infinity, max = -Infinity;
	for (let i = 0; i < n; i++) {
		for (const dy of [2, 6, 10, 14]) {
			const y = s16(o + dy);
			if (y > -1000 && y < 1000) { min = Math.min(min, y); max = Math.max(max, y); }
		}
		o += 20;
	}
	return { min, max };
}

// Vertical band (screen rows) each real costume can deposit colour on, plus the
// set of RGB12 colours it can show across all of its poses.
function actorColourBands(palette, floor) {
	const sprites = loadMonkeyBarScene(undefined, MONKEY_BAR_SCENE_SPRITES);
	return sprites.map(sprite => {
		const colours = new Set();
		let celTop = Infinity, celBottom = -Infinity;
		for (let step = 0; step < sprite.animation.steps; step++) {
			const pose = renderCostumeFrame(sprite.costume, sprite.animation.animation, step, 0, 0, Boolean(sprite.mirrored));
			for (const px of pose.pixels) {
				colours.add(clutToId(palette, pose.palette[px.color] ?? 255));
				celTop = Math.min(celTop, px.y);
				celBottom = Math.max(celBottom, px.y);
			}
		}
		// Floor walkers (anchor within the walkbox range) sweep the whole floor;
		// the hanging object keeps its fixed anchor.
		const walker = sprite.worldY >= floor.min - 4 && sprite.worldY <= floor.max + 4;
		const feetLo = walker ? floor.min : sprite.worldY;
		const feetHi = walker ? floor.max : sprite.worldY;
		const bandTop = Math.max(0, feetLo + celTop);
		const bandBottom = Math.min(HEIGHT - 1, feetHi + celBottom);
		return { name: sprite.name, costumeId: sprite.costumeId, walker, colours: [...colours], bandTop, bandBottom };
	});
}

function buildFrozenSchedule(source, bands) {
	const schedule = [];
	const demandSizes = [];
	for (let y = 0; y < HEIGHT; y++) {
		// Background demand: every colour on the full 640-wide row, weighted by
		// how often it occurs, so common scenery dominates the merge.
		const demand = [];
		const row = source.subarray(y * ROOM_WIDTH, (y + 1) * ROOM_WIDTH);
		for (const id of row) demand.push(id);
		const distinctBg = new Set(row).size;
		const actorWeight = Math.max(1, Math.round(ROOM_WIDTH / Math.max(1, distinctBg)));
		// Actor demand: each reachable costume colour weighted like a typical
		// scenery colour, so it is not merged away and the actor keeps its hue.
		const actorColours = new Set();
		for (const band of bands)
			if (y >= band.bandTop && y <= band.bandBottom)
				for (const c of band.colours) actorColours.add(c);
		for (const c of actorColours) for (let w = 0; w < actorWeight; w++) demand.push(c);

		demandSizes.push(new Set(demand).size);
		schedule.push(choosePalette(demand, BUDGET));
	}
	return { schedule, demandSizes };
}

function quantizeToSchedule(scene, schedule) {
	let error = 0;
	const rgb = new Uint8Array(WIDTH * HEIGHT * 3);
	for (let y = 0; y < HEIGHT; y++) {
		const palette = schedule[y];
		for (let x = 0; x < WIDTH; x++) {
			const src = scene[y * WIDTH + x];
			const id = nearest(src, palette);
			error += distance(src, id);
			rgb.set(idToRgb(id), (y * WIDTH + x) * 3);
		}
	}
	return { error, rgb };
}

function main() {
	const options = parseArgs(process.argv);
	const fixtureDir = resolve(options.fixture);
	const outDir = resolve(options.out);
	mkdirSync(outDir, { recursive: true });

	const { source, palette } = readBackground(fixtureDir);
	const floor = readFloorRange();
	const bands = actorColourBands(palette, floor);

	console.log(`Room-28 walkbox feet rows: ${floor.min}..${floor.max}`);
	for (const band of bands)
		console.log(`  COST ${band.costumeId} (${band.walker ? 'floor walker' : 'fixed'}): ${band.colours.length} colours, band rows ${band.bandTop}..${band.bandBottom}`);

	const { schedule, demandSizes } = buildFrozenSchedule(source, bands);

	// Compose the 31 real animation frames and quantise each to the ONE frozen
	// schedule, so this measures fidelity held across motion, not per frame.
	const frames = Array.from({ length: FRAME_COUNT }, (_, f) => renderMonkeyBarSceneFrame(source, f, bands.length ? loadMonkeyBarScene(undefined, MONKEY_BAR_SCENE_SPRITES) : [], palette).scene);
	const pixels = WIDTH * HEIGHT;
	let total = 0, worst = 0;
	const rendered = [];
	for (const scene of frames) {
		const { error, rgb } = quantizeToSchedule(scene, schedule);
		total += error;
		worst = Math.max(worst, error);
		rendered.push(rgb);
	}

	// Static-background stability: a pixel that is background in every frame is
	// quantised through the same frozen palette every time, so its displayed
	// colour cannot change. Verify by construction.
	let checked = 0, flips = 0;
	for (let i = 0; i < pixels; i++) {
		let first = -1, isStatic = true;
		const base = frames[0][i];
		for (const scene of frames) if (scene[i] !== base) { isStatic = false; break; }
		if (!isStatic) continue;
		checked++;
		for (const rgb of rendered) {
			const c = (rgb[i * 3] << 16) | (rgb[i * 3 + 1] << 8) | rgb[i * 3 + 2];
			if (first < 0) first = c; else if (c !== first) { flips++; break; }
		}
	}

	const overBudget = demandSizes.filter(d => d > BUDGET).length;
	const actorRows = demandSizes.map((_, y) => bands.some(b => y >= b.bandTop && y <= b.bandBottom)).filter(Boolean).length;

	// Emit the frozen schedule as STE palette words, one 16-word row per scanline.
	const scheduleBin = Buffer.alloc(HEIGHT * 16 * 2);
	schedule.forEach((palette, y) => palette.forEach((id, s) => scheduleBin.writeUInt16BE(steWord(id), (y * 16 + s) * 2)));
	writeFileSync(`${outDir}/monkey-line16-schedule.bin`, scheduleBin);

	// Contact sheet: source frame 0, frozen frame 0, frozen frame 15 (mid-motion).
	const sheetFrames = [
		['source frame 0', (() => { const r = new Uint8Array(pixels * 3); for (let i = 0; i < pixels; i++) r.set(idToRgb(frames[0][i]), i * 3); return r; })()],
		['frozen schedule, frame 0', rendered[0]],
		['frozen schedule, frame 15', rendered[15]],
	];
	const gap = 4;
	const sheetH = sheetFrames.length * HEIGHT + (sheetFrames.length - 1) * gap;
	const sheet = new Uint8Array(WIDTH * sheetH * 3);
	sheet.fill(40);
	sheetFrames.forEach(([, rgb], i) => {
		const top = i * (HEIGHT + gap);
		for (let y = 0; y < HEIGHT; y++) sheet.set(rgb.subarray(y * WIDTH * 3, (y + 1) * WIDTH * 3), ((top + y) * WIDTH) * 3);
	});
	writePng(`${outDir}/frozen-schedule-motion.png`, WIDTH, sheetH, sheet);

	const summary = {
		schema: 'spectrum512-monkey-actor-schedule/1',
		scene: `${WIDTH}x${HEIGHT}`,
		budgetPerLine: BUDGET,
		writesPerLine: 1,
		floorFeetRows: floor,
		actors: bands.map(b => ({ costumeId: b.costumeId, walker: b.walker, colours: b.colours.length, bandRows: [b.bandTop, b.bandBottom] })),
		perLineColourDemand: {
			min: Math.min(...demandSizes), max: Math.max(...demandSizes),
			overBudgetLines: overBudget, actorAffectedLines: actorRows,
			over16Lines: demandSizes.filter(d => d > 16).length,
			over32Lines: demandSizes.filter(d => d > 32).length,
			over48Lines: demandSizes.filter(d => d > 48).length,
			byRow: demandSizes,
		},
		coverage: 'Room-28 background plus selected animations of COST 24, 26, 37; not proof of all actors, animation states, script palette changes or UI colours. Demand counts alone do not establish Spectrum representability at each x.',
		fidelity: { meanErrorPerPixel: +(total / (FRAME_COUNT * pixels)).toFixed(2), worstFrameErrorPerPixel: +(worst / pixels).toFixed(2) },
		motionStability: { staticBackgroundPixels: checked, recolouredAcrossMotion: flips },
	};
	writeFileSync(`${outDir}/actor-schedule-summary.json`, JSON.stringify(summary, null, 2) + '\n');

	console.log(`\nper-line colour demand: min ${summary.perLineColourDemand.min}, max ${summary.perLineColourDemand.max}, over budget (${BUDGET}) on ${overBudget}/${HEIGHT} lines`);
	console.log(`over 32 on ${summary.perLineColourDemand.over32Lines}/${HEIGHT} lines; over 48 on ${summary.perLineColourDemand.over48Lines}/${HEIGHT} lines`);
	console.log(`fidelity over ${FRAME_COUNT} motion frames: mean ${summary.fidelity.meanErrorPerPixel} err/px, worst frame ${summary.fidelity.worstFrameErrorPerPixel}`);
	console.log(`motion stability: ${flips} of ${checked} always-background pixels ever recoloured`);
	console.log(`\nWrote ${outDir}/monkey-line16-schedule.bin, frozen-schedule-motion.png, actor-schedule-summary.json`);
}

main();
