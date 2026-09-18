// Fidelity comparison of STE palette strategies on the real room-28 scene.
//
// Composes the room-28 background with the decoded COST actors (same path the
// native generator uses) and renders the completed 320x144 viewport under
// candidate palette strategies, reporting mean Oklab error per pixel and a
// viewable contact sheet. This measures achievable fidelity per strategy; it is
// not an STE timing result.
//
//   node devtools/atari-ste/tools/monkey-strategy-compare.mjs [--fixture DIR] [--out DIR] [--frame N]
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { convertLine, distance } from './reference.mjs';
import { choosePalette, nearest } from './palette-select.mjs';
import { spectrum32EarlySlot, spectrum32LateSlot } from './monkey-spectrum-model.mjs';
import {
	loadMonkeyBarScene,
	MONKEY_BAR_CAMERA_X,
	MONKEY_BAR_SCENE_SPRITES,
	renderMonkeyBarSceneFrame,
} from './monkey-scene.mjs';
import { writePng } from './png.mjs';

const WIDTH = 320;
const HEIGHT = 144;

process.chdir(fileURLToPath(new URL('..', import.meta.url)));

function parseArgs(argv) {
	const out = { fixture: 'monkey-bar/fixture', out: 'monkey-bar/strategy-compare', frame: 0 };
	for (let i = 2; i < argv.length; i++) {
		if (argv[i] === '--fixture') out.fixture = argv[++i];
		else if (argv[i] === '--out') out.out = argv[++i];
		else if (argv[i] === '--frame') out.frame = Number(argv[++i]);
	}
	return out;
}

function readBackground(fixtureDir) {
	const index = readFileSync(`${fixtureDir}/background-640x144.index.bin`);
	const palette = readFileSync(`${fixtureDir}/room-palette.rgb.bin`);
	const source = new Uint16Array(index.length);
	const q = v => Math.max(0, Math.min(15, Math.round(v / 17)));
	for (let i = 0; i < index.length; i++) {
		const o = index[i] * 3;
		source[i] = (q(palette[o]) << 8) | (q(palette[o + 1]) << 4) | q(palette[o + 2]);
	}
	return { source, palette };
}

const idToRgb = id => [(id >> 8) * 17, ((id >> 4) & 15) * 17, (id & 15) * 17];

// Strategy renderers: each returns { rgb: Uint8Array(WIDTH*HEIGHT*3), error }.
function renderNearestPerRegion(scene, paletteForRow) {
	const rgb = new Uint8Array(WIDTH * HEIGHT * 3);
	let error = 0;
	for (let y = 0; y < HEIGHT; y++) {
		const palette = paletteForRow(y);
		for (let x = 0; x < WIDTH; x++) {
			const src = scene[y * WIDTH + x];
			const id = nearest(src, palette);
			error += distance(src, id);
			rgb.set(idToRgb(id), (y * WIDTH + x) * 3);
		}
	}
	return { rgb, error };
}

function strategySingle(scene) {
	const palette = choosePalette([...scene], 16);
	return renderNearestPerRegion(scene, () => palette);
}

function strategyPerLine(scene) {
	const cache = [];
	return renderNearestPerRegion(scene, y => (cache[y] ??= choosePalette(
		Array.from(scene.subarray(y * WIDTH, (y + 1) * WIDTH)), 16)));
}

function strategyReserved(scene, backgroundScene, reserved = 10) {
	// Reserved: `reserved` colours chosen once over all background pixels and
	// held stable on every line; the remaining 16-reserved chosen per line from
	// that line's own pixels (EIGHT.PRG's stable-background / per-line-sprite
	// split).
	const stable = choosePalette([...backgroundScene], reserved);
	const perLine = 16 - reserved;
	const cache = [];
	return renderNearestPerRegion(scene, y => (cache[y] ??= [
		...stable,
		...choosePalette(Array.from(scene.subarray(y * WIDTH, (y + 1) * WIDTH)), perLine),
	]));
}

function strategySpectrum(scene, slotAt) {
	const rgb = new Uint8Array(WIDTH * HEIGHT * 3);
	let error = 0;
	for (let y = 0; y < HEIGHT; y++) {
		const line = scene.subarray(y * WIDTH, (y + 1) * WIDTH);
		const converted = convertLine(line, { registerCount: 16, slotAt });
		for (let x = 0; x < WIDTH; x++) {
			const o = x * 3;
			const id = (Math.round(converted.rgb[o] / 17) << 8) | (Math.round(converted.rgb[o + 1] / 17) << 4) | Math.round(converted.rgb[o + 2] / 17);
			error += distance(line[x], id);
			rgb.set([converted.rgb[o], converted.rgb[o + 1], converted.rgb[o + 2]], (y * WIDTH + x) * 3);
		}
	}
	return { rgb, error };
}

function sourceRgb(scene) {
	const rgb = new Uint8Array(WIDTH * HEIGHT * 3);
	for (let i = 0; i < scene.length; i++) rgb.set(idToRgb(scene[i]), i * 3);
	return rgb;
}

function main() {
	const options = parseArgs(process.argv);
	const fixture = resolve(options.fixture);
	const outDir = resolve(options.out);
	mkdirSync(outDir, { recursive: true });

	const { source, palette } = readBackground(fixture);
	const sprites = loadMonkeyBarScene(undefined, MONKEY_BAR_SCENE_SPRITES);
	const composed = renderMonkeyBarSceneFrame(source, options.frame, sprites, palette).scene;
	// Bare background viewport (no actors) drives the stable reserved colours.
	const background = new Uint16Array(WIDTH * HEIGHT);
	for (let y = 0; y < HEIGHT; y++)
		background.set(source.subarray(y * 640 + MONKEY_BAR_CAMERA_X, y * 640 + MONKEY_BAR_CAMERA_X + WIDTH), y * WIDTH);

	const candidates = [
		['source (RGB12 ceiling)', { rgb: sourceRgb(composed), error: 0 }, null, null],
		['1 single 16-colour palette / frame', strategySingle(composed), 0, 16],
		['2 per-line 16-colour (fit to this frame)', strategyPerLine(composed), 1, 16],
		['3 reserved 10 background + 6 per-line', strategyReserved(composed, background, 10), 1, 16],
		['4 Spectrum 32 (early visible reload)', strategySpectrum(composed, spectrum32EarlySlot), 2, 32],
		['5 Spectrum 32 (late visible reload)', strategySpectrum(composed, spectrum32LateSlot), 2, 32],
		['6 full Spectrum 512 (48 words / line)', strategySpectrum(composed), 3, 48],
	];

	const pixels = WIDTH * HEIGHT;
	const rows = [];
	for (const [name, result, writes, words] of candidates) {
		writePng(`${outDir}/${name.replace(/[^a-z0-9]+/gi, '-').toLowerCase()}.png`, WIDTH, HEIGHT, result.rgb);
		rows.push({ strategy: name, meanErrorPerPixel: +(result.error / pixels).toFixed(3), paletteWritesPerLine: writes, paletteWordsPerLine: words });
	}

	// Contact sheet: source + candidates stacked with 4px gaps.
	const gap = 4;
	const sheetH = candidates.length * HEIGHT + (candidates.length - 1) * gap;
	const sheet = new Uint8Array(WIDTH * sheetH * 3);
	sheet.fill(40);
	candidates.forEach(([, result], i) => {
		const top = i * (HEIGHT + gap);
		for (let y = 0; y < HEIGHT; y++)
			sheet.set(result.rgb.subarray(y * WIDTH * 3, (y + 1) * WIDTH * 3), ((top + y) * WIDTH) * 3);
	});
	writePng(`${outDir}/contact-sheet.png`, WIDTH, sheetH, sheet);

	writeFileSync(`${outDir}/strategy-metrics.json`, JSON.stringify({
		schema: 'spectrum512-monkey-strategy-compare/2',
		scene: `${WIDTH}x${HEIGHT}`,
		frame: options.frame,
		metric: 'mean fixed-point Oklab squared error per pixel vs RGB12 source',
		note: 'Single-frame fits, not frozen motion schedules. Write counts are 16-register groups including the next-line border reload. The 32-word models retain either the early or late visible reload at existing Spectrum positions. CPU savings require a new raster that returns time to the engine; fewer writes inside the existing masked loop do not do so. Lines 0/1 and the known top-line timing offset are not modelled here.',
		strategies: rows,
	}, null, 2) + '\n');

	console.log(`Scene ${WIDTH}x${HEIGHT}, frame ${options.frame}, ${sprites.length} real COST actors composited\n`);
	console.log('strategy                                    mean Oklab err/px   palette writes/line');
	for (const r of rows)
		console.log(`  ${r.strategy.padEnd(42)}${String(r.meanErrorPerPixel).padStart(8)}${String(r.paletteWritesPerLine).padStart(15)}`);
	console.log(`\nWrote ${outDir}/contact-sheet.png and per-strategy PNGs.`);
}

main();
