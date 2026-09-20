// Temporal colour-mixing evaluator for the room-28 SCUMM Bar scene.
//
// Models two STE screen buffers alternating every 50 Hz field. Each source
// colour is shown as a pair of RGB12 colours whose linear-light average is the
// perceived colour. Palettes are optimised over every colour combination, with
// pairs limited to a maximum Oklab lightness difference between the fields
// (the 25 Hz flicker component). Pure alternation with one or two palettes,
// counter-phased checkerboard alternation, a static checkerboard and an
// optional 2x2 pattern of two pairs are compared with the existing no-flicker
// strategies against the original VGA colours. Fidelity and flicker metrics
// only; not an STE timing result, and a 60 Hz monitor cannot show the blend
// faithfully.
//
//   node devtools/atari-ste/tools/monkey-flicker-compare.mjs [--fixture DIR | --capture DIR | --room-colours FILE] [--split LINE]
//     [--union FILE] [--colour-floor 64]
//     [--out DIR] [--data-dir DIR] [--dl 0.05,0.1,0.2,1] [--frames 0] [--restarts 6]
//     [--seed 1] [--flicker-weight 0] [--sheet-dl 0.1] [--chroma-weight 1]
//     [--error-cap DIST --cap-penalty P] [--quad-texture T,...] [--quad-radius 2]
//
// --capture reads 320x200 frames and the live palette captured from the
// running port by scumm-ste-frame-capture.mjs instead of composing the
// 144-line fixture scene; its tables then match the port's real palette.
// --split gives the lines from LINE on (the verb bar) their own palettes and
// pairs, which the port installs there with one Timer B interrupt.
// --room-colours builds a table for a room that was never captured: the
// palette, the background and the colours come from the game files through
// scumm-scene-colours.mjs, and the file is its own --union.
// --union takes the colours a room can show from scumm-scene-colours.mjs: its
// background, objects and costumes become targets of the first region's
// palette fit even when no captured frame showed them, and every target there
// is worth at least --colour-floor pixels per frame, so a costume colour that
// covers few pixels cannot be optimised away. The floor trades the error of
// what the frames happen to show against the error of what the room can show;
// see STE_ATLANTIS_DUAL20.md for the measured curve.
// --chroma-weight scales the Oklab a/b part of the squared error the optimiser
// minimises; --error-cap makes every colour whose optimiser error exceeds DIST
// pay P per squared unit above it, however few pixels it has. Reported metrics
// always use plain Oklab.
// --quad-texture adds, per lightness limit and texture limit, a one-palette
// pattern that shows two pairs per colour: even rows one pair, odd rows the
// other, each counter-phased as a checkerboard. Both pair mixes must lie within
// T in Oklab, which bounds the static row texture. Its palette starts from the
// pair palette and moves each slot within --quad-radius levels per channel.
import { mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadMonkeyContainers, resolveRoom } from './monkey-resource.mjs';
import { parseRoom } from './monkey-room.mjs';
import { choosePalette, nearest } from './palette-select.mjs';
import { convertLine, steWord } from './reference.mjs';
import { renderCostumeFrame } from './monkey-costume.mjs';
import { loadMonkeyBarScene, MONKEY_BAR_CAMERA_X, MONKEY_BAR_SCENE_SPRITES } from './monkey-scene.mjs';
import { writePng } from './png.mjs';

const WIDTH = 320;
const ROOM_WIDTH = 640;
// 144 room lines for the fixture scene, 200 for captured frames; set in main().
let HEIGHT = 144;
let PIXELS = WIDTH * HEIGHT;
// Oklab components are scaled like reference.mjs, so squared errors share its units.
const SCALE = 127;
// A colour further than this (plain Oklab) from its source counts as visibly off.
const VISIBLE = 0.06;
const KIT_DIR = fileURLToPath(new URL('..', import.meta.url));
const USAGE = 'Usage: node devtools/atari-ste/tools/monkey-flicker-compare.mjs [--fixture DIR | --capture DIR] [--split LINE] [--union FILE] [--colour-floor 64] [--out DIR] [--data-dir DIR] [--dl 0.05,0.1,0.2,1] [--frames 0] [--restarts 6] [--seed 1] [--flicker-weight 0] [--sheet-dl 0.1] [--chroma-weight 1] [--error-cap DIST --cap-penalty P] [--quad-texture T,...] [--quad-radius 2]';

function parseArgs(argv) {
	const out = {
		fixture: `${KIT_DIR}monkey-bar/fixture`,
		capture: null,
		roomColours: null,
		union: null,
		colourFloor: 64,
		split: null,
		out: null,
		dataDir: process.env.MONKEY_DATA_DIR,
		dl: [0.05, 0.1, 0.2, 1],
		frames: [0],
		restarts: 6,
		seed: 1,
		flickerWeight: 0,
		sheetDl: 0.1,
		chromaWeight: 1,
		errorCap: null,
		capPenalty: 0,
		quadTexture: [],
		quadRadius: 2,
	};
	const list = value => value.split(',').map(Number);
	for (let i = 2; i < argv.length; i++) {
		const argument = argv[i];
		if (argument === '--fixture') out.fixture = resolve(argv[++i]);
		else if (argument === '--capture') out.capture = resolve(argv[++i]);
		else if (argument === '--room-colours') out.roomColours = resolve(argv[++i]);
		else if (argument === '--union') out.union = resolve(argv[++i]);
		else if (argument === '--colour-floor') out.colourFloor = Number(argv[++i]);
		else if (argument === '--split') out.split = Number(argv[++i]);
		else if (argument === '--out') out.out = resolve(argv[++i]);
		else if (argument === '--data-dir') out.dataDir = resolve(argv[++i]);
		else if (argument === '--dl') out.dl = list(argv[++i]);
		else if (argument === '--frames') out.frames = list(argv[++i]);
		else if (argument === '--restarts') out.restarts = Number(argv[++i]);
		else if (argument === '--seed') out.seed = Number(argv[++i]);
		else if (argument === '--flicker-weight') out.flickerWeight = Number(argv[++i]);
		else if (argument === '--sheet-dl') out.sheetDl = Number(argv[++i]);
		else if (argument === '--chroma-weight') out.chromaWeight = Number(argv[++i]);
		else if (argument === '--error-cap') out.errorCap = Number(argv[++i]);
		else if (argument === '--cap-penalty') out.capPenalty = Number(argv[++i]);
		else if (argument === '--quad-texture') out.quadTexture = list(argv[++i]);
		else if (argument === '--quad-radius') out.quadRadius = Number(argv[++i]);
		else if (argument === '--help') {
			console.log(USAGE);
			process.exit(0);
		} else throw new Error(`Unknown argument ${argument}\n${USAGE}`);
	}
	// A room's colour list is both the scene and the union it is fitted to.
	if (out.roomColours) out.union ??= out.roomColours;
	out.out ??= out.capture ? `${out.capture}/flicker-compare` : `${KIT_DIR}monkey-bar/flicker-compare`;
	out.dl = [...new Set(out.dl)].sort((a, b) => a - b);
	if (out.split !== null && !Number.isInteger(out.split)) throw new Error('--split needs an integer line number');
	if (!Number.isFinite(out.colourFloor) || out.colourFloor < 0) throw new Error('--colour-floor needs a non-negative number of pixels');
	if (!out.dl.length || out.dl.some(v => !Number.isFinite(v) || v < 0)) throw new Error('--dl needs non-negative Oklab lightness limits');
	if (out.frames.some(v => !Number.isInteger(v) || v < 0)) throw new Error('--frames needs non-negative integer frame numbers');
	if (!Number.isInteger(out.restarts) || out.restarts < 0) throw new Error('--restarts needs a non-negative integer');
	if (!Number.isInteger(out.seed)) throw new Error('--seed needs an integer');
	if (!Number.isFinite(out.flickerWeight) || out.flickerWeight < 0) throw new Error('--flicker-weight needs a non-negative number');
	if (!out.dl.includes(out.sheetDl)) throw new Error(`--sheet-dl ${out.sheetDl} is not one of --dl ${out.dl.join(',')}`);
	if (!Number.isFinite(out.chromaWeight) || out.chromaWeight <= 0) throw new Error('--chroma-weight needs a positive number');
	if (out.errorCap !== null && !(out.errorCap > 0)) throw new Error('--error-cap needs a positive Oklab distance');
	if (!Number.isFinite(out.capPenalty) || out.capPenalty < 0) throw new Error('--cap-penalty needs a non-negative number');
	if (out.quadTexture.some(v => !Number.isFinite(v) || v < 0)) throw new Error('--quad-texture needs non-negative Oklab distances');
	if (!Number.isInteger(out.quadRadius) || out.quadRadius < 1) throw new Error('--quad-radius needs a positive integer');
	return out;
}

// ---- Colour model: sRGB-encoded levels, mixed in linear light ----

const srgbToLinear = v => {
	v /= 255;
	return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
};
const linearToSrgb8 = v => {
	v = Math.min(1, Math.max(0, v));
	return Math.round(255 * (v <= 0.0031308 ? 12.92 * v : 1.055 * v ** (1 / 2.4) - 0.055));
};
const BYTE_LINEAR = Float64Array.from({ length: 256 }, (_, v) => srgbToLinear(v));

// The rgbToOklab transform from jscolorquantizer, taking linear light.
function oklab(r, g, b, out, o) {
	const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
	const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
	const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
	out[o] = SCALE * (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s);
	out[o + 1] = SCALE * (1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s);
	out[o + 2] = SCALE * (0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
}

// RGB12 ids 0..4095 (0xRGB, one nibble per channel) in linear light and scaled Oklab.
const ID_LIN = new Float64Array(4096 * 3);
const ID_LAB = new Float64Array(4096 * 3);
for (let id = 0; id < 4096; id++) {
	ID_LIN[id * 3] = BYTE_LINEAR[(id >> 8) * 17];
	ID_LIN[id * 3 + 1] = BYTE_LINEAR[((id >> 4) & 15) * 17];
	ID_LIN[id * 3 + 2] = BYTE_LINEAR[(id & 15) * 17];
	oklab(ID_LIN[id * 3], ID_LIN[id * 3 + 1], ID_LIN[id * 3 + 2], ID_LAB, id * 3);
}

// The optimiser's error space: Oklab with a/b scaled by sqrt(--chroma-weight),
// and a per-colour cap penalty. Set once in main().
let CHROMA = 1;
let OPT_LAB = ID_LAB;
let CAP = Infinity;
let CAP_PENALTY = 0;
function oklabOpt(r, g, b, out, o) {
	oklab(r, g, b, out, o);
	out[o + 1] *= CHROMA;
	out[o + 2] *= CHROMA;
}
const capped = e => (e > CAP ? CAP_PENALTY * (e - CAP) : 0);

const rgb24ToRgb12 = c => (Math.round((c >> 16) / 17) << 8) | (Math.round(((c >> 8) & 255) / 17) << 4) | Math.round((c & 255) / 17);
const hex12 = id => id.toString(16).padStart(3, '0');
const darkFirst = (a, b) => ID_LAB[a * 3] < ID_LAB[b * 3] || (ID_LAB[a * 3] === ID_LAB[b * 3] && a <= b) ? [a, b] : [b, a];

// Writes the perceived colour of an RGB12 pair and its flicker penalty at slot n.
// Returns false when the pair's field lightness difference exceeds the limit.
function pairInto(a, b, limits, lab, penalty, n) {
	const dL = Math.abs(ID_LAB[a * 3] - ID_LAB[b * 3]);
	if (dL > limits.tau) return false;
	if (a === b) {
		lab[n * 3] = OPT_LAB[a * 3];
		lab[n * 3 + 1] = OPT_LAB[a * 3 + 1];
		lab[n * 3 + 2] = OPT_LAB[a * 3 + 2];
	} else {
		oklabOpt((ID_LIN[a * 3] + ID_LIN[b * 3]) / 2, (ID_LIN[a * 3 + 1] + ID_LIN[b * 3 + 1]) / 2, (ID_LIN[a * 3 + 2] + ID_LIN[b * 3 + 2]) / 2, lab, n * 3);
	}
	penalty[n] = limits.lambda * dL * dL;
	return true;
}

function mulberry32(seed) {
	let a = seed >>> 0;
	return () => {
		a = (a + 0x6d2b79f5) >>> 0;
		let t = a;
		t = Math.imul(t ^ (t >>> 15), t | 1);
		t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
		return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
	};
}

const gcd = (a, b) => (b ? gcd(b, a % b) : a);

// ---- Scene: 24-bit VGA colours per pixel, from the fixture or from captures ----

// Room-28 viewport plus real COST actors.
function loadScene(options) {
	const index = readFileSync(`${options.fixture}/background-640x144.index.bin`);
	const palette = readFileSync(`${options.fixture}/room-palette.rgb.bin`);
	if (index.length !== ROOM_WIDTH * 144 || palette.length !== 256 * 3) throw new Error('Unexpected room-28 fixture layout');
	const rgbOf = i => (palette[i * 3] << 16) | (palette[i * 3 + 1] << 8) | palette[i * 3 + 2];
	const background = new Uint32Array(WIDTH * 144);
	for (let y = 0; y < 144; y++)
		for (let x = 0; x < WIDTH; x++) background[y * WIDTH + x] = rgbOf(index[y * ROOM_WIDTH + MONKEY_BAR_CAMERA_X + x]);
	const sprites = loadMonkeyBarScene(options.dataDir, MONKEY_BAR_SCENE_SPRITES).sort((a, b) => a.drawOrder - b.drawOrder);
	// Same placement and draw order as monkey-scene.mjs, which shows unmapped
	// costume colours (index 255) as black.
	const poses = sprites.map(sprite => Array.from({ length: sprite.animation.steps }, (_, step) => {
		const pose = renderCostumeFrame(sprite.costume, sprite.animation.animation, step, sprite.worldX, sprite.worldY, Boolean(sprite.mirrored));
		const pixels = [];
		for (const pixel of pose.pixels) {
			const x = pixel.x - MONKEY_BAR_CAMERA_X;
			if (x < 0 || x >= WIDTH || pixel.y < 0 || pixel.y >= 144) continue;
			const global = pose.palette[pixel.color] ?? 255;
			pixels.push(pixel.y * WIDTH + x, global === 255 ? 0 : rgbOf(global));
		}
		return pixels;
	}));
	const cycle = sprites.reduce((n, sprite) => n * sprite.animation.steps / gcd(n, sprite.animation.steps), 1);
	const frame = f => {
		const scene = background.slice();
		sprites.forEach((sprite, i) => {
			const pixels = poses[i][f % sprite.animation.steps];
			for (let j = 0; j < pixels.length; j += 2) scene[pixels[j]] = pixels[j + 1];
		});
		return scene;
	};
	return {
		palette, rgbOf, frame, cycle, height: 144, framesLabel: 'animation frames',
		description: `${WIDTH}x144 room 28 fixture, camera ${MONKEY_BAR_CAMERA_X}, ${sprites.length} COST actors`,
	};
}

// Frames captured from the running port: 320x200 source indices and the live
// palette of each (scumm-ste-frame-capture.mjs).
function loadCapture(dir) {
	const manifest = JSON.parse(readFileSync(`${dir}/capture.json`, 'utf8'));
	const frames = manifest.captures.map(capture => {
		const indices = readFileSync(`${dir}/${capture.frame}`), palette = readFileSync(`${dir}/${capture.palette}`);
		if (indices.length !== WIDTH * 200 || palette.length !== 256 * 3) throw new Error(`Unexpected capture layout in ${capture.frame}`);
		return { indices, palette };
	});
	if (!frames.length) throw new Error(`No captures in ${dir}`);
	const { palette } = frames[0];
	if (frames.some(f => !f.palette.equals(palette))) console.warn('Captured palettes differ; the tables use the first one.');
	const rgbOf = i => (palette[i * 3] << 16) | (palette[i * 3 + 1] << 8) | palette[i * 3 + 2];
	const frame = f => {
		const { indices, palette: p } = frames[f % frames.length];
		return Uint32Array.from(indices, i => (p[i * 3] << 16) | (p[i * 3 + 1] << 8) | p[i * 3 + 2]);
	};
	return {
		palette, rgbOf, frame, cycle: frames.length, height: 200, framesLabel: 'captured frames',
		description: `${WIDTH}x200 frames captured from the port after VBLs ${manifest.captures.map(c => c.afterVbl).join(',')}`,
	};
}

// A room from the game files, without a capture: the palette and the colours
// it can show come from scumm-scene-colours.mjs, the evaluated frame holds the
// room background at camera 0 and one band per verb-bar colour. The fit comes
// from the union, which counts the whole room and its costumes; this frame is
// what the metrics and previews are measured on, so they cover the background
// and the verb-bar colours, not the actors.
function loadRoomColours(file) {
	const union = JSON.parse(readFileSync(file, 'utf8'));
	if (union.schema !== 'scumm-ste-scene-colours/1') throw new Error(`${file} is not a scene-colour union`);
	const palette = Buffer.from(union.palette);
	if (palette.length !== 256 * 3) throw new Error(`${file} has no 256-entry palette`);
	const rgbOf = i => (palette[i * 3] << 16) | (palette[i * 3 + 1] << 8) | palette[i * 3 + 2];
	const names = readdirSync(union.dataDir).filter(name => /\.00[01]$/i.test(name)).sort();
	const containers = loadMonkeyContainers(union.dataDir, names);
	const { container, room } = resolveRoom(containers, union.room);
	const parsed = parseRoom(container.bytes, room, { roomId: union.room });
	const split = union.verbBar.split;
	const scene = new Uint32Array(WIDTH * 200);
	for (let y = 0; y < Math.min(split, parsed.height); y++)
		for (let x = 0; x < Math.min(WIDTH, parsed.width); x++)
			scene[y * WIDTH + x] = rgbOf(parsed.background.pixels[y * parsed.width + x]);
	const bar = union.verbBar.indices;
	for (let y = split; y < 200; y++)
		for (let x = 0; x < WIDTH; x++)
			scene[y * WIDTH + x] = rgbOf(bar.length ? bar[Math.floor(x * bar.length / WIDTH)] : 0);
	return {
		palette, rgbOf, frame: () => scene, cycle: 1, height: 200, framesLabel: 'room image',
		description: `${WIDTH}x200 room ${union.room} of ${union.game} from the game files, `
			+ `${bar.length} verb-bar colours in bands`,
	};
}

// Targets carry optimiser-space Oklab (lab) and linear light (lin) per colour.
function labTargets(colors, weights) {
	const lab = new Float64Array(colors.length * 3), lin = new Float64Array(colors.length * 3);
	colors.forEach((c, t) => {
		lin[t * 3] = BYTE_LINEAR[c >> 16];
		lin[t * 3 + 1] = BYTE_LINEAR[(c >> 8) & 255];
		lin[t * 3 + 2] = BYTE_LINEAR[c & 255];
		oklabOpt(lin[t * 3], lin[t * 3 + 1], lin[t * 3 + 2], lab, t * 3);
	});
	return { colors, lab, lin, weight: Float64Array.from(weights), index: new Map(colors.map((c, t) => [c, t])) };
}

// Pixel-count weights of lines top..bottom-1 over every scene frame, heaviest
// colour first so candidate costs overtake the running best sooner. A union
// adds the colours the room's resources can show but these frames did not, and
// lifts every weight of the region to the floor, so rare costume colours keep a
// say in the palette.
function buildTargets(scene, top, bottom, union = null) {
	const counts = new Map();
	for (let f = 0; f < scene.cycle; f++) {
		const rgb = scene.frame(f);
		for (let i = top * WIDTH; i < bottom * WIDTH; i++) counts.set(rgb[i], (counts.get(rgb[i]) || 0) + 1);
	}
	let added = 0;
	if (union) {
		// The union counts palette indices; their colours come from the live
		// palette the frames were captured with.
		for (let index = 0; index < 256; index++) {
			if (!(union.weights[index] > 0)) continue;
			const colour = scene.rgbOf(index);
			if (!counts.has(colour)) added++;
			counts.set(colour, Math.max(counts.get(colour) ?? 0, union.floor));
		}
		for (const [colour, count] of counts) counts.set(colour, Math.max(count, union.floor));
	}
	const colors = [...counts.keys()].sort((a, b) => counts.get(b) - counts.get(a) || a - b);
	const targets = labTargets(colors, colors.map(c => counts.get(c)));
	targets.total = targets.weight.reduce((sum, w) => sum + w, 0);
	targets.fromUnion = added;
	return targets;
}

// ---- Palette optimisation over every allowed pair ----

// single: 16 solid colours. pair: one 16-colour palette, any two entries.
// dual: palette 0 (slots 0..15) in field 0, palette 1 (slots 16..31) in field 1.
function prepareMode(name) {
	const slots = name === 'dual' ? 32 : 16;
	const mixes = [];
	if (name === 'single') for (let i = 0; i < 16; i++) mixes.push([i, i]);
	else if (name === 'pair') for (let i = 0; i < 16; i++) for (let j = i; j < 16; j++) mixes.push([i, j]);
	else for (let a = 0; a < 16; a++) for (let b = 0; b < 16; b++) mixes.push([a, 16 + b]);
	const perSlot = Array.from({ length: slots }, (_, k) => ({
		others: mixes.filter(([a, b]) => a !== k && b !== k),
		partners: mixes.filter(([a, b]) => a === k || b === k).map(([a, b]) => (a === b ? -1 : a === k ? b : a)),
	}));
	return { name, slots, mixes, perSlot };
}

function paletteCost(mode, entries, limits, targets, assign) {
	const count = mode.mixes.length, lab = new Float64Array(count * 3), penalty = new Float64Array(count), allowed = new Uint8Array(count);
	let allowedMixes = 0;
	mode.mixes.forEach(([a, b], q) => {
		allowed[q] = pairInto(entries[a], entries[b], limits, lab, penalty, q) ? 1 : 0;
		allowedMixes += allowed[q];
	});
	let cost = 0;
	for (let t = 0; t < targets.weight.length; t++) {
		let best = Infinity, bestQ = -1;
		for (let q = 0; q < count; q++) {
			if (!allowed[q]) continue;
			const dl = lab[q * 3] - targets.lab[t * 3], da = lab[q * 3 + 1] - targets.lab[t * 3 + 1], db = lab[q * 3 + 2] - targets.lab[t * 3 + 2];
			const e = dl * dl + da * da + db * db + penalty[q];
			if (e < best) { best = e; bestQ = q; }
		}
		cost += targets.weight[t] * best + capped(best);
		if (assign) assign[t] = bestQ;
	}
	return { cost, allowedMixes };
}

// Replaces slot k with the RGB12 colour that minimises total cost, trying all 4096.
function improveSlot(mode, entries, k, limits, targets) {
	const { others, partners } = mode.perSlot[k];
	const T = targets.weight.length, target = targets.lab, weight = targets.weight;
	const otherLab = new Float64Array(others.length * 3), otherPenalty = new Float64Array(others.length);
	let n = 0;
	for (const [a, b] of others) if (pairInto(entries[a], entries[b], limits, otherLab, otherPenalty, n)) n++;
	const excluded = new Float64Array(T);
	for (let t = 0; t < T; t++) {
		let best = Infinity;
		for (let q = 0; q < n; q++) {
			const dl = otherLab[q * 3] - target[t * 3], da = otherLab[q * 3 + 1] - target[t * 3 + 1], db = otherLab[q * 3 + 2] - target[t * 3 + 2];
			const e = dl * dl + da * da + db * db + otherPenalty[q];
			if (e < best) best = e;
		}
		excluded[t] = best;
	}
	const lab = new Float64Array(partners.length * 3), penalty = new Float64Array(partners.length);
	const costOf = (c, limit) => {
		let m = 0;
		for (const p of partners) if (pairInto(c, p < 0 ? c : entries[p], limits, lab, penalty, m)) m++;
		let cost = 0;
		for (let t = 0; t < T && cost < limit; t++) {
			let best = excluded[t];
			for (let q = 0; q < m; q++) {
				const dl = lab[q * 3] - target[t * 3], da = lab[q * 3 + 1] - target[t * 3 + 1], db = lab[q * 3 + 2] - target[t * 3 + 2];
				const e = dl * dl + da * da + db * db + penalty[q];
				if (e < best) best = e;
			}
			cost += weight[t] * best + capped(best);
		}
		return cost;
	};
	const current = entries[k];
	let bestCost = costOf(current, Infinity), bestId = current;
	for (let c = 0; c < 4096; c++) {
		if (c === current) continue;
		const cost = costOf(c, bestCost);
		if (cost < bestCost * (1 - 1e-12)) { bestCost = cost; bestId = c; }
	}
	entries[k] = bestId;
	return bestId !== current;
}

// Slots listed in fixed keep their colour.
function descend(mode, entries, limits, targets, fixed) {
	let changed = true;
	while (changed) {
		changed = false;
		for (let k = 0; k < mode.slots; k++)
			if (!fixed.includes(k) && improveSlot(mode, entries, k, limits, targets)) changed = true;
	}
	return paletteCost(mode, entries, limits, targets).cost;
}

// Descends from each start, then kicks two random free slots by up to ±3
// levels per channel and keeps the result when it improves.
function optimise(mode, starts, limits, targets, restarts, rng, fixed = []) {
	const free = [...Array(mode.slots).keys()].filter(k => !fixed.includes(k));
	let best = null;
	for (const start of starts) {
		const entries = Int32Array.from(start), cost = descend(mode, entries, limits, targets, fixed);
		if (!best || cost < best.cost) best = { entries, cost };
	}
	for (let r = 0; r < restarts; r++) {
		const entries = best.entries.slice();
		for (let n = 0; n < 2; n++) {
			const k = free[Math.floor(rng() * free.length)], id = entries[k];
			const channel = shift => Math.max(0, Math.min(15, ((id >> shift) & 15) + Math.floor(rng() * 7) - 3));
			entries[k] = (channel(8) << 8) | (channel(4) << 4) | channel(0);
		}
		const cost = descend(mode, entries, limits, targets, fixed);
		if (cost < best.cost) best = { entries, cost };
	}
	return best;
}

// Best allowed pair per target as RGB12 ids: [field-0 colour, field-1 colour]
// for dual, darker colour first otherwise.
function assignPairs(mode, entries, limits, targets) {
	const assign = new Int32Array(targets.weight.length);
	const { allowedMixes } = paletteCost(mode, entries, limits, targets, assign);
	const pairs = new Int32Array(assign.length * 2);
	assign.forEach((q, t) => {
		const [a, b] = mode.mixes[q];
		const ids = mode.name === 'dual' ? [entries[a], entries[b]] : darkFirst(entries[a], entries[b]);
		pairs[t * 2] = ids[0];
		pairs[t * 2 + 1] = ids[1];
	});
	return { pairs, allowedMixes };
}

// Palette-free ceiling: the best allowed pair of any two RGB12 colours within
// one level of the target on each channel.
function ceilingPairs(targets, limits) {
	const pairs = new Int32Array(targets.weight.length * 2), lab = new Float64Array(3), penalty = new Float64Array(1);
	targets.colors.forEach((c, t) => {
		const options = [c >> 16, (c >> 8) & 255, c & 255].map(v => {
			const q = Math.round(v / 17), levels = [...new Set([q - 1, q, q + 1].map(l => Math.max(0, Math.min(15, l))))];
			return levels.flatMap(u => levels.map(w => [u, w]));
		});
		let best = Infinity;
		for (const [r0, r1] of options[0]) for (const [g0, g1] of options[1]) for (const [b0, b1] of options[2]) {
			const a = (r0 << 8) | (g0 << 4) | b0, b = (r1 << 8) | (g1 << 4) | b1;
			if (!pairInto(a, b, limits, lab, penalty, 0)) continue;
			const dl = lab[0] - targets.lab[t * 3], da = lab[1] - targets.lab[t * 3 + 1], db = lab[2] - targets.lab[t * 3 + 2];
			const e = dl * dl + da * da + db * db + penalty[0];
			if (e < best) {
				best = e;
				[pairs[t * 2], pairs[t * 2 + 1]] = darkFirst(a, b);
			}
		}
	});
	return pairs;
}

// ---- 2x2 two-pair pattern ----

// Allowed pairs of a palette, skipping slot skip, with their linear mix,
// optimiser Oklab and flicker penalty.
function palettePairs(entries, limits, skip = -1) {
	const pairs = [], lab = new Float64Array(3), penalty = new Float64Array(1);
	for (let i = 0; i < entries.length; i++) for (let j = i; j < entries.length; j++) {
		if (i === skip || j === skip || !pairInto(entries[i], entries[j], limits, lab, penalty, 0)) continue;
		const a = entries[i], b = entries[j];
		pairs.push({
			i, j, penalty: penalty[0], lab: Float64Array.from(lab),
			lin: [0, 1, 2].map(ch => (ID_LIN[a * 3 + ch] + ID_LIN[b * 3 + ch]) / 2),
		});
	}
	return pairs;
}

// Perceived colours of every pattern combining pair p with each pair in qs
// whose mix lies within the texture limit; appended to combos as flat records.
function addCombos(p, qs, texture2, combos) {
	for (const q of qs) {
		const dl = p.lab[0] - q.lab[0], da = p.lab[1] - q.lab[1], db = p.lab[2] - q.lab[2];
		if (dl * dl + da * da + db * db > texture2) continue;
		const lab = new Float64Array(3);
		oklabOpt((p.lin[0] + q.lin[0]) / 2, (p.lin[1] + q.lin[1]) / 2, (p.lin[2] + q.lin[2]) / 2, lab, 0);
		combos.push({ p, q, lab, penalty: (p.penalty + q.penalty) / 2 });
	}
}

function allCombos(pairs, texture2) {
	const combos = [];
	pairs.forEach((p, n) => addCombos(p, pairs.slice(n), texture2, combos));
	return combos;
}

function quadCost(combos, targets, assign) {
	let cost = 0;
	for (let t = 0; t < targets.weight.length; t++) {
		let best = Infinity, bestCombo = null;
		for (const combo of combos) {
			const dl = combo.lab[0] - targets.lab[t * 3], da = combo.lab[1] - targets.lab[t * 3 + 1], db = combo.lab[2] - targets.lab[t * 3 + 2];
			const e = dl * dl + da * da + db * db + combo.penalty;
			if (e < best) { best = e; bestCombo = combo; }
		}
		cost += targets.weight[t] * best + capped(best);
		if (assign) assign[t] = bestCombo;
	}
	return cost;
}

// Local descent from a pair palette: each free slot tries every RGB12 colour
// within radius levels per channel against the pattern objective.
function optimiseQuad(start, limits, texture2, targets, radius, fixed) {
	const entries = Int32Array.from(start), T = targets.weight.length;
	let cost = quadCost(allCombos(palettePairs(entries, limits), texture2), targets);
	let changed = true;
	while (changed) {
		changed = false;
		for (let k = 0; k < entries.length; k++) {
			if (fixed.includes(k)) continue;
			const others = palettePairs(entries, limits, k), excluded = new Float64Array(T);
			const otherCombos = allCombos(others, texture2);
			for (let t = 0; t < T; t++) {
				let best = Infinity;
				for (const combo of otherCombos) {
					const dl = combo.lab[0] - targets.lab[t * 3], da = combo.lab[1] - targets.lab[t * 3 + 1], db = combo.lab[2] - targets.lab[t * 3 + 2];
					const e = dl * dl + da * da + db * db + combo.penalty;
					if (e < best) best = e;
				}
				excluded[t] = best;
			}
			const current = entries[k];
			let bestCost = cost, bestId = current;
			for (let dr = -radius; dr <= radius; dr++) for (let dg = -radius; dg <= radius; dg++) for (let dbl = -radius; dbl <= radius; dbl++) {
				const r = ((current >> 8) & 15) + dr, g = ((current >> 4) & 15) + dg, b = (current & 15) + dbl;
				if (r < 0 || r > 15 || g < 0 || g > 15 || b < 0 || b > 15) continue;
				const c = (r << 8) | (g << 4) | b;
				if (c === current) continue;
				entries[k] = c;
				// Pairs with the candidate, then every pattern that uses one of them.
				const mine = palettePairs(entries, limits).filter(p => p.i === k || p.j === k), combos = [];
				mine.forEach((p, n) => { addCombos(p, others, texture2, combos); addCombos(p, mine.slice(n), texture2, combos); });
				let candidateCost = 0;
				for (let t = 0; t < T && candidateCost < bestCost; t++) {
					let best = excluded[t];
					for (const combo of combos) {
						const dl = combo.lab[0] - targets.lab[t * 3], da = combo.lab[1] - targets.lab[t * 3 + 1], db = combo.lab[2] - targets.lab[t * 3 + 2];
						const e = dl * dl + da * da + db * db + combo.penalty;
						if (e < best) best = e;
					}
					candidateCost += targets.weight[t] * best + capped(best);
				}
				if (candidateCost < bestCost * (1 - 1e-12)) { bestCost = candidateCost; bestId = c; }
			}
			entries[k] = bestId;
			if (bestId !== current) { cost = bestCost; changed = true; }
		}
	}
	return { entries, cost };
}

// Per target four RGB12 ids: the even-row pair then the odd-row pair.
function assignQuads(entries, limits, texture2, targets) {
	const combos = allCombos(palettePairs(entries, limits), texture2), assign = new Array(targets.weight.length);
	quadCost(combos, targets, assign);
	const quads = new Int32Array(assign.length * 4);
	assign.forEach(({ p, q }, t) => {
		quads.set([...darkFirst(entries[p.i], entries[p.j]), ...darkFirst(entries[q.i], entries[q.j])], t * 4);
	});
	return { quads, combos: combos.length };
}

// ---- Rendering fields and measuring what a viewer sees ----

// solved: one { top, bottom, targets, pairs | quads } per screen region.
// alternate: every pixel shows its first colour in field 0, second in field 1.
// checker: the same, counter-phased on (x + y) parity so areas cancel spatially.
// static: (x + y) parity picks the colour; both fields identical.
// quad: even rows use the first pair and odd rows the second, each as checker.
function renderPairs(rgb, solved, pattern) {
	const field0 = new Uint16Array(PIXELS), field1 = new Uint16Array(PIXELS);
	for (const { top, bottom, targets, pairs, quads } of solved) {
		for (let y = top; y < bottom; y++) for (let x = 0; x < WIDTH; x++) {
			const i = y * WIDTH + x, t = targets.index.get(rgb[i]), odd = (x + y) & 1;
			if (pattern === 'quad') {
				const o = t * 4 + (y & 1) * 2;
				field0[i] = quads[o + odd];
				field1[i] = quads[o + 1 - odd];
				continue;
			}
			const a = pairs[t * 2], b = pairs[t * 2 + 1];
			if (pattern === 'alternate') { field0[i] = a; field1[i] = b; }
			else if (pattern === 'checker') { field0[i] = odd ? b : a; field1[i] = odd ? a : b; }
			else field0[i] = field1[i] = odd ? b : a;
		}
	}
	return [field0, field1];
}

function solidFields(ids) {
	return [ids, ids];
}

function renderBaseline(rgb, name) {
	const ids = Uint16Array.from(rgb, rgb24ToRgb12);
	if (name === 'rgb12-nearest') return ids;
	if (name === 'single16-merge') {
		const palette = choosePalette([...ids], 16);
		return ids.map(id => nearest(id, palette));
	}
	const out = new Uint16Array(PIXELS);
	for (let y = 0; y < HEIGHT; y++) {
		const line = ids.subarray(y * WIDTH, (y + 1) * WIDTH);
		if (name === 'perline16-merge') {
			const palette = choosePalette([...line], 16);
			for (let x = 0; x < WIDTH; x++) out[y * WIDTH + x] = nearest(line[x], palette);
		} else {
			const converted = convertLine(line, { registerCount: 16 });
			for (let x = 0; x < WIDTH; x++) out[y * WIDTH + x] = rgb24ToRgb12((converted.rgb[x * 3] << 16) | (converted.rgb[x * 3 + 1] << 8) | converted.rgb[x * 3 + 2]);
		}
	}
	return out;
}

// The 2x2 box stands in for viewing distance: it averages a checkerboard exactly.
function box(buffer, x, y, channel) {
	const x1 = Math.min(x + 1, WIDTH - 1), y1 = Math.min(y + 1, HEIGHT - 1);
	return (buffer[(y * WIDTH + x) * 3 + channel] + buffer[(y * WIDTH + x1) * 3 + channel]
		+ buffer[(y1 * WIDTH + x) * 3 + channel] + buffer[(y1 * WIDTH + x1) * 3 + channel]) / 4;
}

function stats(values) {
	const sorted = Float64Array.from(values).sort();
	return { mean: sorted.reduce((sum, v) => sum + v, 0) / sorted.length, p95: sorted[Math.floor(0.95 * (sorted.length - 1))] };
}

function measure(rgb, field0, field1) {
	const source = new Float64Array(PIXELS * 3), seen = new Float64Array(PIXELS * 3);
	const lin0 = new Float64Array(PIXELS * 3), lin1 = new Float64Array(PIXELS * 3);
	const average = new Uint8Array(PIXELS * 3), image0 = new Uint8Array(PIXELS * 3), image1 = new Uint8Array(PIXELS * 3);
	for (let i = 0; i < PIXELS; i++) {
		const c = rgb[i];
		source[i * 3] = BYTE_LINEAR[c >> 16];
		source[i * 3 + 1] = BYTE_LINEAR[(c >> 8) & 255];
		source[i * 3 + 2] = BYTE_LINEAR[c & 255];
		for (let ch = 0; ch < 3; ch++) {
			lin0[i * 3 + ch] = ID_LIN[field0[i] * 3 + ch];
			lin1[i * 3 + ch] = ID_LIN[field1[i] * 3 + ch];
			seen[i * 3 + ch] = (lin0[i * 3 + ch] + lin1[i * 3 + ch]) / 2;
			average[i * 3 + ch] = linearToSrgb8(seen[i * 3 + ch]);
			image0[i * 3 + ch] = ((field0[i] >> (8 - 4 * ch)) & 15) * 17;
			image1[i * 3 + ch] = ((field1[i] >> (8 - 4 * ch)) & 15) * 17;
		}
	}
	const a = new Float64Array(3), b = new Float64Array(3);
	const squared = () => (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2;
	const flickerPixel = new Float64Array(PIXELS), flickerArea = new Float64Array(PIXELS);
	let pixelError = 0, blurError = 0, texture = 0;
	for (let y = 0; y < HEIGHT; y++) for (let x = 0; x < WIDTH; x++) {
		const i = y * WIDTH + x;
		oklab(seen[i * 3], seen[i * 3 + 1], seen[i * 3 + 2], a, 0);
		oklab(source[i * 3], source[i * 3 + 1], source[i * 3 + 2], b, 0);
		pixelError += squared();
		oklab(box(seen, x, y, 0), box(seen, x, y, 1), box(seen, x, y, 2), b, 0);
		// Static texture: how far a pixel's time average sits from its 2x2 area.
		texture += Math.sqrt(squared()) / SCALE;
		oklab(box(source, x, y, 0), box(source, x, y, 1), box(source, x, y, 2), a, 0);
		blurError += squared();
		flickerPixel[i] = Math.abs(ID_LAB[field0[i] * 3] - ID_LAB[field1[i] * 3]) / SCALE;
		oklab(box(lin0, x, y, 0), box(lin0, x, y, 1), box(lin0, x, y, 2), a, 0);
		oklab(box(lin1, x, y, 0), box(lin1, x, y, 1), box(lin1, x, y, 2), b, 0);
		flickerArea[i] = Math.abs(a[0] - b[0]) / SCALE;
	}
	return {
		metrics: {
			pixelError: pixelError / PIXELS, blurError: blurError / PIXELS, texture: texture / PIXELS,
			flickerPixel: stats(flickerPixel), flickerArea: stats(flickerArea),
		},
		images: { average, field0: image0, field1: image1, flickers: field0.some((id, i) => id !== field1[i]) },
	};
}

// Distinct perceived outputs and colours visibly off (plain Oklab) per solved
// scene, from each target's assigned RGB12 ids.
function summarise(solved) {
	let distinct = 0, off = 0, offWeight = 0, total = 0;
	const lab = new Float64Array(3), target = new Float64Array(3);
	for (const { targets, pairs, quads } of solved) {
		const ids = quads ?? pairs, per = quads ? 4 : 2, keys = new Set();
		for (let t = 0; t < targets.weight.length; t++) {
			const group = [...ids.subarray(t * per, (t + 1) * per)];
			keys.add(quads ? [group.slice(0, 2).sort().join('+'), group.slice(2).sort().join('+')].sort().join('/') : group.sort().join('+'));
			const mix = [0, 1, 2].map(ch => group.reduce((sum, id) => sum + ID_LIN[id * 3 + ch], 0) / per);
			oklab(mix[0], mix[1], mix[2], lab, 0);
			oklab(targets.lin[t * 3], targets.lin[t * 3 + 1], targets.lin[t * 3 + 2], target, 0);
			if (Math.hypot(lab[0] - target[0], lab[1] - target[1], lab[2] - target[2]) / SCALE > VISIBLE) {
				off++;
				offWeight += targets.weight[t];
			}
		}
		distinct += keys.size;
		total += targets.total;
	}
	return { distinctOutputs: distinct, coloursOff: off, offPixelShare: round(offWeight / total, 4) };
}

// ---- Main ----

const round = (v, digits = 3) => +v.toFixed(digits);

function main() {
	const options = parseArgs(process.argv);
	mkdirSync(options.out, { recursive: true });
	const started = performance.now();
	const elapsed = () => `${((performance.now() - started) / 1000).toFixed(1)}s`;
	const scene = options.roomColours ? loadRoomColours(options.roomColours)
		: options.capture ? loadCapture(options.capture) : loadScene(options);
	HEIGHT = scene.height;
	PIXELS = WIDTH * HEIGHT;
	CHROMA = Math.sqrt(options.chromaWeight);
	if (CHROMA !== 1) {
		OPT_LAB = Float64Array.from(ID_LAB);
		for (let id = 0; id < 4096; id++) {
			OPT_LAB[id * 3 + 1] *= CHROMA;
			OPT_LAB[id * 3 + 2] *= CHROMA;
		}
	}
	if (options.errorCap !== null) {
		CAP = (options.errorCap * SCALE) ** 2;
		CAP_PENALTY = options.capPenalty;
	}
	if (options.split !== null && (options.split <= 0 || options.split >= HEIGHT))
		throw new Error(`--split must lie inside the ${HEIGHT}-line scene`);
	const regions = options.split === null
		? [{ name: 'scene', top: 0, bottom: HEIGHT }]
		: [{ name: 'room', top: 0, bottom: options.split }, { name: 'verb bar', top: options.split, bottom: HEIGHT }];
	// The union describes what the room itself can show, so it belongs to the
	// first region; the verb bar is the engine's and stays as captured.
	let union = null;
	if (options.union !== null) {
		const file = JSON.parse(readFileSync(options.union, 'utf8'));
		if (file.schema !== 'scumm-ste-scene-colours/1') throw new Error(`${options.union} is not a scene-colour union`);
		union = { weights: file.indices.total, floor: options.colourFloor * scene.cycle, source: file };
	}
	for (const region of regions)
		region.targets = buildTargets(scene, region.top, region.bottom, region.top === 0 ? union : null);
	const evalFrames = options.frames.map(frame => ({ frame, rgb: scene.frame(frame) }));
	console.log(`${scene.description}: ${regions.map(r => `${r.name} lines ${r.top}..${r.bottom - 1} ${r.targets.colors.length} VGA colours`).join(', ')} over ${scene.cycle} ${scene.framesLabel}; evaluating frames ${options.frames.join(',')}`);
	if (union)
		console.log(`union ${options.union}: room ${union.source.room} of ${union.source.game}, costumes ${union.source.costumes.map(c => c.id).join(',') || 'none'}, ${regions[0].targets.fromUnion} colours no captured frame showed, floor ${options.colourFloor} pixels per frame`);

	const rows = [], images = new Map();
	const record = (key, strategy, info, fieldsFor) => {
		const measured = evalFrames.map(({ rgb }) => measure(rgb, ...fieldsFor(rgb)));
		const mean = pick => measured.reduce((sum, m) => sum + pick(m.metrics), 0) / measured.length;
		const { images: first } = measured[0];
		images.set(key, first);
		writePng(`${options.out}/${key}.png`, WIDTH, HEIGHT, first.average);
		if (first.flickers) {
			writePng(`${options.out}/${key}-field0.png`, WIDTH, HEIGHT, first.field0);
			writePng(`${options.out}/${key}-field1.png`, WIDTH, HEIGHT, first.field1);
		}
		rows.push({
			key, strategy, ...info,
			pixelError: round(mean(m => m.pixelError)),
			blurError: round(mean(m => m.blurError)),
			texture: round(mean(m => m.texture), 4),
			flickerPixelMean: round(mean(m => m.flickerPixel.mean), 4),
			flickerPixelP95: round(mean(m => m.flickerPixel.p95), 4),
			flickerAreaMean: round(mean(m => m.flickerArea.mean), 4),
			flickerAreaP95: round(mean(m => m.flickerArea.p95), 4),
		});
	};
	const pairsRecord = (key, strategy, info, solved, pattern) =>
		record(key, strategy, { ...info, ...summarise(solved) }, rgb => renderPairs(rgb, solved, pattern));

	const source = new Uint8Array(PIXELS * 3);
	evalFrames[0].rgb.forEach((c, i) => source.set([c >> 16, (c >> 8) & 255, c & 255], i * 3));
	writePng(`${options.out}/vga-source.png`, WIDTH, HEIGHT, source);
	images.set('vga-source', { average: source });

	const baselines = [
		['rgb12-nearest', 'RGB12 nearest, no palette limit'],
		['single16-merge', 'single 16-colour palette per frame (choosePalette)'],
		['perline16-merge', 'per-line 16-colour palette (choosePalette, fit per frame)'],
		['spectrum512', 'full Spectrum 512, 48 words/line (convertLine)'],
	];
	for (const [key, strategy] of baselines) {
		record(key, strategy, { palette: 'baseline' }, rgb => solidFields(renderBaseline(rgb, key)));
		console.log(`  ${key} done (${elapsed()})`);
	}

	const rng = mulberry32(options.seed);
	for (const region of regions) {
		const weightedIds = [];
		region.targets.colors.forEach((c, t) => {
			for (let n = Math.max(1, Math.round(region.targets.weight[t] / scene.cycle)); n > 0; n--) weightedIds.push(rgb24ToRgb12(c));
		});
		region.merged = choosePalette(weightedIds, 16);
		while (region.merged.length < 16) region.merged.push(region.merged[region.merged.length - 1]);
	}

	const modes = { single: prepareMode('single'), pair: prepareMode('pair'), dual: prepareMode('dual') };
	// With a split, slot 0 is black in every palette: the verb bar's first
	// line is black, so a Timer B interrupt that lands a line late stays
	// invisible there.
	const fixedSlots = slots => options.split === null ? [] : slots === 32 ? [0, 16] : [0];
	const solve = (mode, limits, startsFor) => regions.map((region, r) => {
		const fixed = fixedSlots(mode.slots);
		const starts = startsFor(r).map(start => {
			const entries = Int32Array.from(start);
			for (const k of fixed) entries[k] = 0;
			return entries;
		});
		const best = optimise(mode, starts, limits, region.targets, options.restarts, rng, fixed);
		return { ...region, ...best, ...assignPairs(mode, best.entries, limits, region.targets) };
	});
	const paletteInfo = (mode, solved) => ({
		palette: mode.name,
		fitError: round(solved.reduce((sum, s) => sum + s.cost, 0) / solved.reduce((sum, s) => sum + s.targets.total, 0)),
		allowedMixes: solved[0].allowedMixes,
		palettes: solved.map(s => ({
			lines: `${s.top}..${s.bottom - 1}`,
			allowedMixes: s.allowedMixes,
			colours: mode.name === 'dual'
				? [[...s.entries.slice(0, 16)].map(hex12), [...s.entries.slice(16)].map(hex12)]
				: [[...s.entries].map(hex12)],
		})),
	});
	const noLimit = { tau: Infinity, lambda: options.flickerWeight };
	const single = solve(modes.single, noLimit, r => [regions[r].merged]);
	pairsRecord('single16-optimised', `single 16-colour palette, optimised over the ${scene.framesLabel}`,
		paletteInfo(modes.single, single), single, 'alternate');
	console.log(`  single16-optimised done (${elapsed()})`);

	const luts = [];
	const paletteColours = labTargets(Array.from({ length: 256 }, (_, i) => scene.rgbOf(i)), new Array(256).fill(1));
	const writeTable = (name, words, slotsPerRegion) => {
		const out = Buffer.alloc(words.length * 2 + slotsPerRegion.reduce((sum, s) => sum + s.length, 0) + 768);
		words.forEach((id, i) => out.writeUInt16BE(steWord(id), i * 2));
		let offset = words.length * 2;
		for (const slots of slotsPerRegion) {
			out.set(slots, offset);
			offset += slots.length;
		}
		scene.palette.copy(out, offset);
		writeFileSync(`${options.out}/${name}.bin`, out);
		luts.push(`${name}.bin`);
	};
	const writeLut = (name, mode, solved, limits) => {
		const words = [];
		for (let field = 0; field < (mode.name === 'dual' ? 2 : 1); field++)
			for (const s of solved) words.push(...s.entries.slice(field * 16, field * 16 + 16));
		writeTable(name, words, solved.map(s => {
			const { pairs } = assignPairs(mode, s.entries, limits, paletteColours), slots = new Uint8Array(512);
			for (let i = 0; i < 256; i++) {
				slots[i * 2] = s.entries.indexOf(pairs[i * 2]);
				slots[i * 2 + 1] = mode.name === 'dual' ? s.entries.indexOf(pairs[i * 2 + 1], 16) - 16 : s.entries.indexOf(pairs[i * 2 + 1]);
			}
			return slots;
		}));
	};

	let previousPair = null, previousDual = null;
	for (const dl of options.dl) {
		const limits = { tau: dl * SCALE, lambda: options.flickerWeight }, tag = `dl${dl.toFixed(2)}`;
		const pair = solve(modes.pair, limits, r => [regions[r].merged, single[r].entries, ...(previousPair ? [previousPair[r].entries] : [])]);
		const pairInfo = { ...paletteInfo(modes.pair, pair), dl };
		pairsRecord(`pair16-${tag}-alternate`, `one 16-colour palette, pairs alternate per field, ΔL ≤ ${dl}`, pairInfo, pair, 'alternate');
		pairsRecord(`pair16-${tag}-checker`, `one 16-colour palette, checkerboard counter-phased per field, ΔL ≤ ${dl}`, pairInfo, pair, 'checker');
		pairsRecord(`pair16-${tag}-static`, `one 16-colour palette, static checkerboard, ΔL ≤ ${dl}`, pairInfo, pair, 'static');
		writeLut(`lut-pair16-${tag}`, modes.pair, pair, limits);
		console.log(`  pair16 ${tag} done (${elapsed()})`);

		for (const texture of options.quadTexture) {
			const texture2 = (texture * SCALE) ** 2, qtag = `${tag}-t${texture.toFixed(2)}`;
			const quad = regions.map((region, r) => {
				const best = optimiseQuad(pair[r].entries, limits, texture2, region.targets, options.quadRadius, fixedSlots(16));
				return { ...region, ...best, ...assignQuads(best.entries, limits, texture2, region.targets) };
			});
			pairsRecord(`quad16-${qtag}`, `one 16-colour palette, two pairs per colour on alternate rows as checkerboards, ΔL ≤ ${dl}, mixes within ${texture}`,
				{ palette: 'quad', dl, texture, fitError: round(quad.reduce((sum, s) => sum + s.cost, 0) / quad.reduce((sum, s) => sum + s.targets.total, 0)),
					patterns: quad[0].combos, palettes: quad.map(s => ({ lines: `${s.top}..${s.bottom - 1}`, colours: [[...s.entries].map(hex12)] })) },
				quad, 'quad');
			writeTable(`lut-quad16-${qtag}`, quad.flatMap(s => [...s.entries]), quad.map(s => {
				const { quads } = assignQuads(s.entries, limits, texture2, paletteColours), slots = new Uint8Array(1024);
				for (let i = 0; i < 1024; i++) slots[i] = s.entries.indexOf(quads[i]);
				return slots;
			}));
			console.log(`  quad16 ${qtag} done (${elapsed()})`);
		}

		const dual = solve(modes.dual, limits, r => [[...pair[r].entries, ...pair[r].entries], ...(previousDual ? [previousDual[r].entries] : [])]);
		pairsRecord(`dual16-${tag}-alternate`, `two 16-colour palettes swapped per field, ΔL ≤ ${dl}`,
			{ ...paletteInfo(modes.dual, dual), dl }, dual, 'alternate');
		writeLut(`lut-dual16-${tag}`, modes.dual, dual, limits);
		console.log(`  dual16 ${tag} done (${elapsed()})`);

		pairsRecord(`ceiling-${tag}-alternate`, `any two RGB12 colours per source colour (no palette limit), ΔL ≤ ${dl}`,
			{ palette: 'ceiling', dl }, regions.map(region => ({ ...region, pairs: ceilingPairs(region.targets, limits) })), 'alternate');
		previousPair = pair;
		previousDual = dual;
	}

	const sheetTag = `dl${options.sheetDl.toFixed(2)}`;
	const layout = [
		['vga-source', `pair16-${sheetTag}-alternate`],
		['rgb12-nearest', `dual16-${sheetTag}-alternate`],
		['perline16-merge', `pair16-${sheetTag}-static`],
		['spectrum512', `pair16-${sheetTag}-alternate:field0`],
		['single16-optimised', `pair16-${sheetTag}-checker:field0`],
	];
	const gap = 4, sheetW = WIDTH * 2 + gap, sheetH = layout.length * HEIGHT + (layout.length - 1) * gap;
	const sheet = new Uint8Array(sheetW * sheetH * 3).fill(40);
	layout.forEach((row, r) => row.forEach((cell, col) => {
		const [key, view = 'average'] = cell.split(':'), image = images.get(key)[view];
		for (let y = 0; y < HEIGHT; y++)
			for (let x = 0; x < WIDTH; x++)
				sheet.set(image.subarray((y * WIDTH + x) * 3, (y * WIDTH + x) * 3 + 3), ((r * (HEIGHT + gap) + y) * sheetW + col * (WIDTH + gap) + x) * 3);
	}));
	writePng(`${options.out}/contact-sheet.png`, sheetW, sheetH, sheet);

	writeFileSync(`${options.out}/flicker-metrics.json`, JSON.stringify({
		schema: 'spectrum512-monkey-flicker-compare/3',
		scene: scene.description,
		fitFrames: `${scene.cycle} ${scene.framesLabel} (pixel-count weights)`,
		union: union ? {
			file: options.union, game: union.source.game, room: union.source.room,
			costumes: union.source.costumes.map(({ id, source, steps }) => ({ id, source, steps })),
			colourFloorPixelsPerFrame: options.colourFloor,
			coloursNoFrameShowed: regions[0].targets.fromUnion,
			indicesOnlyACostumeUses: union.source.indices.costumeOnly,
		} : null,
		evalFrames: options.frames,
		regions: regions.map(r => ({ name: r.name, lines: `${r.top}..${r.bottom - 1}`, targetColours: r.targets.colors.length })),
		options: {
			dl: options.dl, split: options.split, restarts: options.restarts, seed: options.seed, flickerWeight: options.flickerWeight,
			chromaWeight: options.chromaWeight, errorCap: options.errorCap, capPenalty: options.capPenalty,
			quadTexture: options.quadTexture, quadRadius: options.quadRadius,
		},
		displayModel: 'RGB12 levels shown as level*17 sRGB; the two fields average in linear light; perceived colour converted to Oklab.',
		metrics: {
			pixelError: 'mean squared Oklab distance (components x127, as reference.mjs but unrounded) of the time-averaged pixel vs the original VGA colour',
			blurError: 'the same after a 2x2 linear-light box on both images, standing in for viewing distance; a checkerboard averages exactly',
			texture: 'mean Oklab distance between a pixel\'s time average and its 2x2 box: static pattern visible up close',
			flickerPixel: 'Oklab lightness difference (0..1) between the two fields at each pixel',
			flickerArea: 'Oklab lightness difference between the two fields after the 2x2 box; counter-phased checkerboards cancel here',
			fitError: 'weighted mean optimiser error (with --chroma-weight and --error-cap applied) over all scene frames, per source pixel',
			allowedMixes: 'palette pairs within the lightness limit (single colours included); the first region in the table columns',
			distinctOutputs: 'different perceived results over all regions for the scene colours',
			coloursOff: `scene colours whose perceived result is more than ${VISIBLE} from the source in plain Oklab; offPixelShare weights them by pixels`,
		},
		lutFormat: 'big-endian STE palette words: per field (one for pair and quad, two for dual) a 16-word palette per region (the scene, or the room then the verb bar with --split); then per region 256 x [slot A, slot B] bytes for VGA indices 0..255 (quad: 256 x [even-row A, even-row B, odd-row A, odd-row B]); then the 768-byte VGA palette the pairs were computed for. A is the darker entry except for dual, where A indexes the field-0 palette and B the field-1 palette. With --split slot 0 is black in every palette.',
		luts,
		contactSheet: { file: 'contact-sheet.png', dl: options.sheetDl, layout: layout.map(row => row.join(' | ')) },
		strategies: rows,
		limitations: [
			'The time average assumes a 50 Hz display and full temporal integration; judge flicker on a CRT or true 50 Hz display, not a 60 Hz monitor or Hatari on one.',
			'Lightness limits are not calibrated against real flicker visibility.',
			...(options.capture
				? ['Only the captured frames; palette effects and other rooms are not covered.']
				: ['Only the 144-line room viewport and its actors; no verb bar, inventory, text, cursor or palette effects.',
					'The fixture actors use the room CLUT, not the palette the running game sets.']),
			'The camera does not move; no scroll frames are evaluated.',
			'Baselines are fitted to each evaluated frame, which favours them; the flicker palettes are fixed over all scene frames.',
			'Time-averaged images from alternate and checker are identical per pixel; they differ in flicker structure only.',
			'The quad pattern has no port support yet; its tables are evaluation output only.',
			'Fidelity and flicker only; no STE conversion, redraw or VBL timing.',
		],
	}, null, 2) + '\n');

	console.log(`\n${'strategy'.padEnd(34)}${'pixelErr'.padStart(9)}${'blurErr'.padStart(9)}${'texture'.padStart(8)}${'flkArea'.padStart(9)}${'p95'.padStart(7)}${'mixes'.padStart(7)}${'kept'.padStart(6)}${'off'.padStart(5)}`);
	for (const r of rows)
		console.log(`${r.key.padEnd(34)}${r.pixelError.toFixed(2).padStart(9)}${r.blurError.toFixed(2).padStart(9)}${r.texture.toFixed(3).padStart(8)}${r.flickerAreaMean.toFixed(3).padStart(9)}${r.flickerAreaP95.toFixed(3).padStart(7)}${String(r.allowedMixes ?? r.patterns ?? '').padStart(7)}${String(r.distinctOutputs ?? '').padStart(6)}${String(r.coloursOff ?? '').padStart(5)}`);
	console.log(`\nWrote ${options.out}/flicker-metrics.json, contact-sheet.png and per-strategy PNGs (${elapsed()}).`);
}

main();
