import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { DEFAULT_MONKEY_DATA_DIR, loadMonkeyContainers, resolveRoom, sha256 } from './monkey-resource.mjs';
import { parseRoom, toRgbImage } from './monkey-room.mjs';
import { writePng } from './png.mjs';

function parseArgs(argv) {

	const result = {
		dataDir: DEFAULT_MONKEY_DATA_DIR,
		outDir: process.env.MONKEY_OUT ?? 'devtools/atari-ste/monkey-bar/fixture',
		roomId: Number(process.env.MONKEY_ROOM ?? 28),
	};
	for (let i = 2; i < argv.length; i++) {
		const argument = argv[i];
		if (argument === '--data-dir') result.dataDir = argv[++i];
		else if (argument === '--out') result.outDir = argv[++i];
		else if (argument === '--room') result.roomId = Number(argv[++i]);
		else if (argument === '--help') {
			console.log('Usage: node devtools/atari-ste/tools/monkey-extract.mjs [--data-dir DIR] [--out DIR] [--room ID]');
			process.exit(0);
		} else throw new Error(`Unknown argument ${argument}`);
	}
	if (!Number.isInteger(result.roomId) || result.roomId < 0 || result.roomId > 255) throw new Error(`Invalid room ${result.roomId}`);
	return result;
}

function cropIndexed(source, sourceWidth, sourceHeight, left, width) {

	const result = new Uint8Array(width * sourceHeight);
	for (let y = 0; y < sourceHeight; y++) result.set(source.subarray(y * sourceWidth + left, y * sourceWidth + left + width), y * width);
	return result;
}

function writeIndexedPreview(outDir, name, indexed, width, height, palette) {

	writeFileSync(`${outDir}/${name}.index.bin`, indexed);
	writePng(`${outDir}/${name}.png`, width, height, toRgbImage(indexed, palette));
	return {
		width,
		height,
		indexFile: `${name}.index.bin`,
		indexSha256: sha256(indexed),
		previewFile: `${name}.png`,
	};
}

function main() {

	const options = parseArgs(process.argv);
	const output = resolve(options.outDir);
	mkdirSync(output, { recursive: true });
	const containers = loadMonkeyContainers(options.dataDir);
	const resolved = resolveRoom(containers, options.roomId);
	const room = parseRoom(resolved.container.bytes, resolved.room, { roomId: options.roomId });
	const background = room.background.pixels;
	const palette = room.palette;
	const fullPreview = writeIndexedPreview(output, 'background-640x144', background, room.width, room.height, palette);
	const leftPreview = writeIndexedPreview(output, 'viewport-000-320x144', cropIndexed(background, room.width, room.height, 0, 320), 320, room.height, palette);
	const rightPreview = writeIndexedPreview(output, 'viewport-320-320x144', cropIndexed(background, room.width, room.height, room.width - 320, 320), 320, room.height, palette);
	writeFileSync(`${output}/room-palette.rgb.bin`, palette);
	mkdirSync(`${output}/objects`, { recursive: true });
	const objectBitmaps = room.objectBitmaps.map(({ objectId, tag, bitmap }) => {
		const name = `object-${objectId}-${tag}`;
		writeFileSync(`${output}/objects/${name}.index.bin`, bitmap.pixels);
		writePng(`${output}/objects/${name}.png`, bitmap.width, bitmap.height, toRgbImage(bitmap.pixels, palette));
		return {
			objectId,
			tag,
			width: bitmap.width,
			height: bitmap.height,
			stripCount: bitmap.strips.length,
			codecSummary: bitmap.strips.reduce((summary, strip) => {
				summary[strip.codec] = (summary[strip.codec] ?? 0) + 1;
				return summary;
			}, {}),
			indexFile: `objects/${name}.index.bin`,
			previewFile: `objects/${name}.png`,
			sha256: sha256(bitmap.pixels),
		};
	});

	const manifest = {
		schema: 'spectrum512-monkey-bar-fixture/1',
		createdUtc: new Date().toISOString(),
		game: {
			name: 'The Secret of Monkey Island',
			edition: 'DOS CD',
			resourceXor: '0x69',
			dataDirectory: options.dataDir,
		},
		sourceFiles: containers.map(container => ({
			name: container.name,
			path: container.path,
			encodedBytes: container.encodedBytes,
			sha256: container.sha256,
			containerTag: container.root.tag,
			containerSize: container.root.size,
			loffEntries: container.loff.length,
		})),
		fixture: {
			roomId: options.roomId,
			resourceFile: resolved.container.name,
			resourceFileOffset: resolved.entry.fileOffset,
			resourceTag: resolved.room.tag,
			resourceSize: resolved.room.size,
			lflfChunkOffset: resolved.entry.fileOffset - 8,
			width: room.width,
			height: room.height,
			interfaceRows: 56,
			viewportWidth: 320,
			backgroundIndexIdentity: true,
			paletteEntries: 256,
			objectRecords: room.objectCount,
			decodedObjectImages: room.objectImages.length,
			decodedObjectBitmaps: objectBitmaps.length,
		},
		palette: {
			block: room.paletteBlocks.clut,
			file: 'room-palette.rgb.bin',
			sha256: sha256(palette),
			cycleBlock: room.paletteBlocks.cycl,
			transparentIndex: room.paletteBlocks.trns?.transparentIndex ?? null,
		},
		background: {
			codecSummary: room.image.codecSummary,
			stripCount: room.image.strips.length,
			indexFile: fullPreview.indexFile,
			sha256: fullPreview.indexSha256,
			previewFile: fullPreview.previewFile,
			preparedBytes: background.byteLength + palette.byteLength,
			viewports: [leftPreview, rightPreview],
		},
		roomBlocks: room.blocks,
		objects: room.objects,
		objectBitmaps,
		sequence: room.sequence,
		verification: {
			decoder: 'SCUMM V5 SMAP codecs 18/28/68, source palette indices retained',
			flattenedFrames: false,
			engineCapture: 'not captured',
			nativeRendererMeasurement: 'not measured',
			integratedGameMeasurement: 'not measured',
		},
	};
	writeFileSync(`${output}/room-manifest.json`, JSON.stringify(manifest, null, 2) + '\n');
	writeFileSync(`${output}/room-strips.json`, JSON.stringify(room.image.strips, null, 2) + '\n');
	console.log(JSON.stringify({ output, room: options.roomId, size: `${room.width}x${room.height}`, objectRecords: room.objectCount, codecs: room.image.codecSummary, backgroundSha256: fullPreview.indexSha256 }, null, 2));
}

main();
