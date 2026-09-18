import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { convertLine, distance, quantizeImage } from './reference.mjs';
import { writePng } from './png.mjs';

const WIDTH = 320;
const HEIGHT = 144;
const ROOM_WIDTH = 640;

function parseArgs(argv) {

	const result = {
		fixtureDir: process.env.MONKEY_FIXTURE ?? 'devtools/atari-ste/monkey-bar/fixture',
		outDir: process.env.MONKEY_COMPARE_OUT ?? 'devtools/atari-ste/monkey-bar/comparison',
		camera: Number(process.env.MONKEY_CAMERA ?? 0),
		dither: process.env.MONKEY_DITHER ?? 'none',
	};
	for (let i = 2; i < argv.length; i++) {
		const argument = argv[i];
		if (argument === '--fixture') result.fixtureDir = argv[++i];
		else if (argument === '--out') result.outDir = argv[++i];
		else if (argument === '--camera') result.camera = Number(argv[++i]);
		else if (argument === '--dither') result.dither = argv[++i];
		else if (argument === '--help') {
			console.log('Usage: node devtools/atari-ste/tools/monkey-compare.mjs [--fixture DIR] [--out DIR] [--camera 0..320] [--dither none|checks]');
			process.exit(0);
		} else throw new Error(`Unknown argument ${argument}`);
	}
	if (!Number.isInteger(result.camera) || result.camera < 0 || result.camera > ROOM_WIDTH - WIDTH) throw new Error(`Camera must be an integer in 0..320, got ${result.camera}`);
	if (!['none', 'checks'].includes(result.dither)) throw new Error(`Dither must be none or checks, got ${result.dither}`);
	return result;
}

function rgb12(r, g, b) {

	const q = value => Math.max(0, Math.min(15, Math.round(value / 17)));
	return (q(r) << 8) | (q(g) << 4) | q(b);
}

function rgb12ToRgb(id) {

	return [(id >> 8) * 17, ((id >> 4) & 15) * 17, (id & 15) * 17];
}

function readSource(fixtureDir) {

	const manifest = JSON.parse(readFileSync(`${fixtureDir}/room-manifest.json`, 'utf8'));
	const index = readFileSync(`${fixtureDir}/background-640x144.index.bin`);
	const palette = readFileSync(`${fixtureDir}/room-palette.rgb.bin`);
	if (index.length !== ROOM_WIDTH * HEIGHT) throw new Error(`Unexpected source index length ${index.length}`);
	if (palette.length !== 256 * 3) throw new Error(`Unexpected palette length ${palette.length}`);
	const source = new Uint16Array(index.length);
	const rgba = new Uint8Array(index.length * 4);
	for (let i = 0; i < index.length; i++) {
		const offset = index[i] * 3;
		rgba.set(palette.subarray(offset, offset + 3), i * 4);
		rgba[i * 4 + 3] = 255;
		source[i] = rgb12(palette[offset], palette[offset + 1], palette[offset + 2]);
	}
	return { manifest, index, palette, source, rgba };
}

function cropLine(source, camera, y) {

	return source.subarray(y * ROOM_WIDTH + camera, y * ROOM_WIDTH + camera + WIDTH);
}

function chooseLinePalette(line, count = 16) {

	const histogram = new Map();
	for (const id of line) histogram.set(id, (histogram.get(id) ?? 0) + 1);
	const colors = [...histogram.keys()];
	colors.sort((a, b) => histogram.get(b) - histogram.get(a) || a - b);
	const palette = colors.length ? [colors[0]] : [0];
	while (palette.length < count && palette.length < colors.length) {
		let bestId = colors[0], bestScore = -1;
		for (const candidate of colors) {
			if (palette.includes(candidate)) continue;
			const nearest = Math.min(...palette.map(selected => distance(candidate, selected)));
			const score = nearest * histogram.get(candidate);
			if (score > bestScore || (score === bestScore && candidate < bestId)) {
				bestId = candidate;
				bestScore = score;
			}
		}
		palette.push(bestId);
	}
	while (palette.length < count) palette.push(palette[palette.length - 1]);
	return palette;
}

function nearestPaletteIndex(id, palette) {

	let best = 0;
	let bestDistance = Infinity;
	for (let i = 0; i < palette.length; i++) {
		const current = distance(id, palette[i]);
		if (current < bestDistance || (current === bestDistance && palette[i] < palette[best])) {
			best = i;
			bestDistance = current;
		}
	}
	return best;
}

function packPlanar(indices) {

	const planar = Buffer.alloc(160);
	for (let x = 0; x < WIDTH; x++) {
		const index = indices[x];
		for (let plane = 0; plane < 4; plane++) if (index & (1 << plane)) {
			const offset = (x >> 4) * 8 + plane * 2;
			planar.writeUInt16BE(planar.readUInt16BE(offset) | (0x8000 >> (x & 15)), offset);
		}
	}
	return planar;
}

function convertSharedLine(line, stableLine = line) {

	const palette = chooseLinePalette(stableLine);
	const indices = Uint8Array.from(line, id => nearestPaletteIndex(id, palette));
	const colors = Uint16Array.from(indices, index => palette[index]);
	return { palette, indices, colors, planar: packPlanar(indices) };
}

function convertJointLine(line) {

	const converted = convertLine(line, { registerCount: 16 });
	const colors = new Uint16Array(WIDTH);
	for (let x = 0; x < WIDTH; x++) {
		const offset = x * 3;
		colors[x] = rgb12(converted.rgb[offset], converted.rgb[offset + 1], converted.rgb[offset + 2]);
	}
	return {
		palette: converted.slots,
		indices: null,
		colors,
		planar: converted.planar,
	};
}

function compareCandidate(source, camera, converter, conversionSource = source) {

	const colors = new Uint16Array(WIDTH * HEIGHT);
	const paletteRows = [];
	const planar = Buffer.alloc(160 * HEIGHT);
	let error = 0;
	let changedPixels = 0;
	for (let y = 0; y < HEIGHT; y++) {
		const line = cropLine(conversionSource, camera, y);
		const stableLine = conversionSource.subarray(y * ROOM_WIDTH, y * ROOM_WIDTH + ROOM_WIDTH);
		const converted = converter(line, stableLine);
		colors.set(converted.colors, y * WIDTH);
		paletteRows.push(converted.palette);
		converted.planar.copy(planar, y * 160);
		for (let x = 0; x < WIDTH; x++) {
			const sourceId = line[x];
			const resultId = converted.colors[x];
			error += distance(sourceId, resultId);
			if (sourceId !== resultId) changedPixels++;
		}
	}
	return { colors, paletteRows, planar, error, changedPixels };
}

function imageFromColors(colors) {

	const rgb = new Uint8Array(colors.length * 3);
	for (let i = 0; i < colors.length; i++) rgb.set(rgb12ToRgb(colors[i]), i * 3);
	return rgb;
}

function sourceImage(sourceIndex, palette, camera) {

	const rgb = new Uint8Array(WIDTH * HEIGHT * 3);
	for (let y = 0; y < HEIGHT; y++) for (let x = 0; x < WIDTH; x++) {
		const originalIndex = sourceIndex[y * ROOM_WIDTH + camera + x];
		const sourceOffset = originalIndex * 3;
		rgb.set(palette.subarray(sourceOffset, sourceOffset + 3), (y * WIDTH + x) * 3);
	}
	return rgb;
}

function writePaletteRows(path, rows) {

	const rowLength = rows[0].length;
	const out = Buffer.alloc(rows.length * rowLength * 2);
	for (let y = 0; y < rows.length; y++) for (let i = 0; i < rowLength; i++) out.writeUInt16BE(rows[y][i], (y * rowLength + i) * 2);
	writeFileSync(path, out);
	return out.length;
}

function makeComparisonImage(sourceRgb, sharedRgb, jointRgb) {

	const rgb = new Uint8Array(WIDTH * 3 * HEIGHT * 3);
	for (let y = 0; y < HEIGHT; y++) {
		for (let x = 0; x < WIDTH; x++) {
			for (let panel = 0; panel < 3; panel++) {
				const input = [sourceRgb, sharedRgb, jointRgb][panel];
				const inputOffset = (y * WIDTH + x) * 3;
				const outputOffset = (y * WIDTH * 3 + panel * WIDTH + x) * 3;
				rgb.set(input.subarray(inputOffset, inputOffset + 3), outputOffset);
			}
		}
	}
	return rgb;
}

function candidateManifest(name, result, paletteBytes) {

	return {
		name,
		metric: 'fixed-point Oklab squared distance from the source image quantized to RGB12',
		totalError: result.error,
		meanErrorPerPixel: result.error / (WIDTH * HEIGHT),
		changedPixels: result.changedPixels,
		screenBytes: result.planar.length,
		paletteBytes,
		preparedBytes: result.planar.length + paletteBytes,
	};
}

function main() {

	const options = parseArgs(process.argv);
	const fixtureDir = resolve(options.fixtureDir);
	const output = resolve(options.outDir);
	mkdirSync(output, { recursive: true });
	const loaded = readSource(fixtureDir);
	const sourceImageRgb = sourceImage(loaded.index, loaded.palette, options.camera);
	const conversionSource = options.dither === 'checks'
		? quantizeImage({ width: ROOM_WIDTH, height: HEIGHT, rgba: loaded.rgba })
		: loaded.source;
	const shared = compareCandidate(loaded.source, options.camera, convertSharedLine, conversionSource);
	const joint = compareCandidate(loaded.source, options.camera, convertJointLine, conversionSource);
	const sharedPaletteBytes = writePaletteRows(`${output}/shared-line-palettes.rgb12.bin`, shared.paletteRows);
	const jointPaletteBytes = writePaletteRows(`${output}/joint-spectrum-palettes.rgb12.bin`, joint.paletteRows);
	writeFileSync(`${output}/shared-line-screen.planar.bin`, shared.planar);
	writeFileSync(`${output}/joint-spectrum-screen.planar.bin`, joint.planar);
	const conversionBytes = Buffer.alloc(conversionSource.length * 2);
	for (let i = 0; i < conversionSource.length; i++) conversionBytes.writeUInt16BE(conversionSource[i], i * 2);
	writeFileSync(`${output}/conversion-input.rgb12.bin`, conversionBytes);
	writePng(`${output}/source.png`, WIDTH, HEIGHT, sourceImageRgb);
	writePng(`${output}/shared-line.png`, WIDTH, HEIGHT, imageFromColors(shared.colors));
	writePng(`${output}/joint-spectrum.png`, WIDTH, HEIGHT, imageFromColors(joint.colors));
	writePng(`${output}/comparison-source-shared-joint.png`, WIDTH * 3, HEIGHT, makeComparisonImage(sourceImageRgb, imageFromColors(shared.colors), imageFromColors(joint.colors)));

	const result = {
		schema: 'spectrum512-monkey-bar-comparison/1',
		fixture: {
			manifest: `${fixtureDir}/room-manifest.json`,
			roomId: loaded.manifest.fixture.roomId,
			cameraOffset: options.camera,
			viewport: `${WIDTH}x${HEIGHT}`,
		},
		input: {
			sourcePaletteEntries: 256,
			sourcePixels: WIDTH * HEIGHT,
			sourceRgb12Quantization: 'nearest per channel, 4 bits per component',
			dither: options.dither,
			conversionInputFile: 'conversion-input.rgb12.bin',
		},
		candidates: [
			candidateManifest('shared-line-palette-room-stable', shared, sharedPaletteBytes),
			candidateManifest('joint-spectrum-16-register', joint, jointPaletteBytes),
		],
		files: {
			comparison: 'comparison-source-shared-joint.png',
			sharedPreview: 'shared-line.png',
			jointPreview: 'joint-spectrum.png',
		},
		limitations: [
			'Host-side static background only; no actor costumes, masks, subtitles, camera capture, or engine/audio timing.',
			'Palette state is represented as RGB12 identifiers; native STE word serialization remains a later renderer step.',
		],
	};
	writeFileSync(`${output}/comparison-manifest.json`, JSON.stringify(result, null, 2) + '\n');
	console.log(JSON.stringify(result, null, 2));
}

main();
