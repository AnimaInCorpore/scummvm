import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { convertLine, distance, steWord } from './reference.mjs';
import { getSpectrum512ColorSlotIndex as slotAt } from './spectrum512-slots.mjs';
import { loadMonkeyBarScene, MONKEY_BAR_CAMERA_X, MONKEY_BAR_SCENE_SPRITES, renderMonkeyBarSceneFrame } from './monkey-scene.mjs';

const WIDTH = 320;
const HEIGHT = 200;
const SCENE_HEIGHT = 144;
const PALETTE_ROWS = 199;
const PALETTE_SLOTS = 48;
const FRAME_COUNT = 31;
const FRAME_BYTES = 32000 + PALETTE_ROWS * PALETTE_SLOTS * 2 + 192;

function parseArgs(argv) {
	const result = {
		fixture: 'devtools/atari-ste/monkey-bar/fixture',
		out: 'devtools/atari-ste/monkey-bar/native-assets',
		costume: process.env.MONKEY_COSTUME ? Number(process.env.MONKEY_COSTUME) : null,
	};
	for (let i = 2; i < argv.length; i++) {
		if (argv[i] === '--fixture') result.fixture = argv[++i];
		else if (argv[i] === '--out') result.out = argv[++i];
		else if (argv[i] === '--costume') result.costume = Number(argv[++i]);
		else if (argv[i] === '--help') {
			console.log('Usage: node devtools/atari-ste/tools/generate-monkey-native.mjs [--fixture DIR] [--out DIR]');
			process.exit(0);
		} else throw new Error(`Unknown argument ${argv[i]}`);
	}
	return result;
}

function setPixel(screen, x, y, index) {
	if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
	const offset = y * 160 + (x >> 4) * 8;
	const mask = 0x8000 >> (x & 15);
	for (let plane = 0; plane < 4; plane++) {
		const wordOffset = offset + plane * 2;
		let word = screen.readUInt16BE(wordOffset);
		word = index & (1 << plane) ? word | mask : word & ~mask;
		screen.writeUInt16BE(word, wordOffset);
	}
}

function fillRect(screen, x, y, width, height, index) {
	for (let row = y; row < y + height; row++) for (let column = x; column < x + width; column++) setPixel(screen, column, row, index);
}

function addGemPanel(screen) {
	fillRect(screen, 0, SCENE_HEIGHT, WIDTH, HEIGHT - SCENE_HEIGHT, 1);
	fillRect(screen, 0, SCENE_HEIGHT, WIDTH, 2, 15);
	fillRect(screen, 0, HEIGHT - 2, WIDTH, 2, 0);
	fillRect(screen, 7, SCENE_HEIGHT + 10, 306, 28, 2);
	fillRect(screen, 9, SCENE_HEIGHT + 12, 302, 24, 1);
	fillRect(screen, 9, SCENE_HEIGHT + 12, 302, 2, 15);
	fillRect(screen, 9, SCENE_HEIGHT + 32, 302, 2, 0);
	fillRect(screen, 17, SCENE_HEIGHT + 19, 72, 8, 3);
	fillRect(screen, 101, SCENE_HEIGHT + 19, 72, 8, 3);
	fillRect(screen, 185, SCENE_HEIGHT + 19, 56, 8, 3);
	fillRect(screen, 253, SCENE_HEIGHT + 19, 48, 8, 3);
	fillRect(screen, 17, SCENE_HEIGHT + 47, 286, 1, 0);
	fillRect(screen, 17, SCENE_HEIGHT + 54, 90, 4, 4);
	fillRect(screen, 119, SCENE_HEIGHT + 54, 82, 4, 4);
	fillRect(screen, 213, SCENE_HEIGHT + 54, 90, 4, 4);
}

function interfacePalette() {
	return [0x000, 0x111, 0x222, 0x333, 0x444, 0x555, 0x666, 0x777, 0x888, 0x999, 0xaaa, 0xbbb, 0xccc, 0xddd, 0xeee, 0xfff];
}

function readSource(fixtureDir) {
	const index = readFileSync(`${fixtureDir}/background-640x144.index.bin`);
	const palette = readFileSync(`${fixtureDir}/room-palette.rgb.bin`);
	const source = new Uint16Array(index.length);
	const quantize = value => Math.max(0, Math.min(15, Math.round(value / 17)));
	for (let i = 0; i < index.length; i++) {
		const offset = index[i] * 3;
		source[i] = (quantize(palette[offset]) << 8) | (quantize(palette[offset + 1]) << 4) | quantize(palette[offset + 2]);
	}
	return { source, palette };
}

function convertFixedLine(line, slots) {
	const planar = Buffer.alloc(160);
	const rgb = new Uint8Array(960);
	let error = 0;
	let changedPixels = 0;
	for (let x = 0; x < WIDTH; x++) {
		let best = Infinity;
		let bestRegister = 0;
		for (let register = 0; register < 16; register++) {
			const slot = slotAt(x, register);
			const candidate = slots[slot];
			const candidateError = distance(line[x], candidate);
			if (candidateError < best) {
				best = candidateError;
				bestRegister = register;
			}
		}
		const id = slots[slotAt(x, bestRegister)];
		rgb.set([(id >> 8) * 17, ((id >> 4) & 15) * 17, (id & 15) * 17], x * 3);
		for (let plane = 0; plane < 4; plane++) if (bestRegister & (1 << plane)) {
			const offset = (x >> 4) * 8 + plane * 2;
			planar.writeUInt16BE(planar.readUInt16BE(offset) | (0x8000 >> (x & 15)), offset);
		}
		error += best;
		if (line[x] !== id) changedPixels++;
	}
	return { planar, rgb, error, changedPixels };
}

function fixedLineError(line, slots) {
	let error = 0;
	for (let x = 0; x < WIDTH; x++) {
		let best = Infinity;
		for (let register = 0; register < 16; register++) best = Math.min(best, distance(line[x], slots[slotAt(x, register)]));
		error += best;
	}
	return error;
}

function chooseStablePaletteRows(sceneFrames) {
	const candidateRows = sceneFrames.map(scene => Array.from({ length: SCENE_HEIGHT }, (_, y) => (
		convertLine(scene.subarray(y * WIDTH, (y + 1) * WIDTH), { registerCount: 16 }).slots
	)));
	return Array.from({ length: SCENE_HEIGHT }, (_, y) => {
		let bestSlots = candidateRows[0][y];
		let bestError = Infinity;
		for (const frameRows of candidateRows) {
			const slots = frameRows[y];
			const error = sceneFrames.reduce((sum, scene) => sum + fixedLineError(scene.subarray(y * WIDTH, (y + 1) * WIDTH), slots), 0);
			if (error < bestError) {
				bestError = error;
				bestSlots = slots;
			}
		}
		return bestSlots;
	});
}

function convertScene(scene, stablePaletteRows) {
	const screen = Buffer.alloc(WIDTH * HEIGHT / 2);
	const paletteRows = stablePaletteRows ?? [];
	let error = 0;
	let changedPixels = 0;
	for (let y = 0; y < SCENE_HEIGHT; y++) {
		const sourceLine = scene.subarray(y * WIDTH, (y + 1) * WIDTH);
		const converted = stablePaletteRows
			? convertFixedLine(sourceLine, stablePaletteRows[y])
			: convertLine(sourceLine, { registerCount: 16 });
		converted.planar.copy(screen, y * 160);
		if (!stablePaletteRows) paletteRows.push(converted.slots);
		for (let x = 0; x < WIDTH; x++) {
			const offset = x * 3;
			const convertedId = (Math.round(converted.rgb[offset] / 17) << 8) | (Math.round(converted.rgb[offset + 1] / 17) << 4) | Math.round(converted.rgb[offset + 2] / 17);
			error += distance(sourceLine[x], convertedId);
			if (sourceLine[x] !== convertedId) changedPixels++;
		}
	}
	addGemPanel(screen);
	return { screen, palette: buildPaletteStream(paletteRows), error, changedPixels };
}

function buildPaletteStream(scenePaletteRows) {
	const rows = [];
	const ui = interfacePalette();
	for (let streamRow = 0; streamRow < PALETTE_ROWS; streamRow++) {
		// The timed STE raster installs the palette for display scanline y from
		// stream row y-1. Row zero is the synchronization line.
		const screenY = streamRow + 1;
		const row = screenY < SCENE_HEIGHT
			? scenePaletteRows[screenY]
			: ui.concat(ui, ui);
		rows.push(Array.from(row, value => steWord(value)));
	}
	const output = Buffer.alloc(rows.length * PALETTE_SLOTS * 2);
	rows.flat().forEach((value, index) => output.writeUInt16BE(value, index * 2));
	return output;
}

function main() {
	const options = parseArgs(process.argv);
	const fixture = resolve(options.fixture);
	const output = resolve(options.out);
	mkdirSync(output, { recursive: true });
	const { source, palette: roomPalette } = readSource(fixture);
	const definitions = options.costume === null
		? MONKEY_BAR_SCENE_SPRITES
		: [{ ...MONKEY_BAR_SCENE_SPRITES[0], name: `override-costume-${options.costume}`, costumeId: options.costume }];
	const sprites = loadMonkeyBarScene(undefined, definitions);
	console.log(`Loaded ${sprites.length} real room-28 COST sprites for camera ${MONKEY_BAR_CAMERA_X}`);
	for (const sprite of sprites) console.log(`  ${sprite.name}: COST ${sprite.costumeId}, animation ${sprite.animation.animation}, ${sprite.animation.steps} pose steps at (${sprite.worldX}, ${sprite.worldY})`);
	const sceneFrames = Array.from({ length: FRAME_COUNT }, (_, frame) => renderMonkeyBarSceneFrame(source, frame, sprites, roomPalette).scene);
	const stablePaletteRows = chooseStablePaletteRows(sceneFrames);
	const frames = [];
	const metrics = [];
	for (let frame = 0; frame < FRAME_COUNT; frame++) {
		const converted = convertScene(sceneFrames[frame], stablePaletteRows);
		const frameData = Buffer.concat([converted.screen, converted.palette, Buffer.alloc(192)]);
		if (frameData.length !== FRAME_BYTES) throw new Error(`Unexpected frame size ${frameData.length}`);
		frames.push(frameData);
		if (frame === 0) {
			writeFileSync(`${output}/monkey-screen.bin`, converted.screen);
			writeFileSync(`${output}/monkey-palettes.bin`, converted.palette);
		}
		metrics.push({ frame, totalError: converted.error, changedPixels: converted.changedPixels });
	}
	writeFileSync(`${output}/monkey-frames.bin`, Buffer.concat(frames));
	writeFileSync(`${output}/animation-metrics.json`, JSON.stringify({
		schema: 'spectrum512-monkey-bar-animation-metrics/1',
		metric: 'fixed-point Oklab squared distance against the composed RGB12 source frame',
		frames: metrics,
		meanErrorPerPixel: metrics.reduce((sum, frame) => sum + frame.totalError, 0) / (FRAME_COUNT * WIDTH * SCENE_HEIGHT),
	}, null, 2) + '\n');
	writeFileSync(`${output}/native-manifest.json`, JSON.stringify({
		schema: 'spectrum512-monkey-bar-native-preview/1',
		candidate: 'joint-spectrum-16-register with animated native overlay',
		scene: `${WIDTH}x${SCENE_HEIGHT}`,
		screenBytes: 32000,
		paletteRows: PALETTE_ROWS,
		paletteWordsPerRow: PALETTE_SLOTS,
		animationFrames: FRAME_COUNT,
		cameraOffset: MONKEY_BAR_CAMERA_X,
		animation: 'room-28 COST sprite batch, composited in world order before one stable per-scanline Spectrum palette schedule is reused for every animation frame',
		paletteSchedule: 'one host-selected 48-word row schedule shared by all 31 animation frames',
		sprites: sprites.map(sprite => ({
			name: sprite.name,
			costumeId: sprite.costumeId,
			animation: sprite.animation.animation,
			steps: sprite.animation.steps,
			worldX: sprite.worldX,
			worldY: sprite.worldY,
		})),
		interface: 'GEM-style static status panel in rows 144..199',
	}, null, 2) + '\n');
	console.log(`Generated native preview assets in ${output}`);
}

main();
