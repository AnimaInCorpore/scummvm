import { codecSummary, decodeSmap } from './monkey-codec.mjs';
import { findChild, parseChildren, readUInt16LEFromBlock } from './monkey-resource.mjs';

function readUInt16LE(bytes, offset) {

	return bytes[offset] | (bytes[offset + 1] << 8);
}

function readInt16LE(bytes, offset) {

	const value = readUInt16LE(bytes, offset);
	return value & 0x8000 ? value - 0x10000 : value;
}

function requireChild(bytes, block, tag) {

	const child = findChild(bytes, block, tag);
	if (!child) throw new Error(`Missing ${tag} inside ${block.tag} at ${block.offset}`);
	return child;
}

function parseImageHeader(bytes, block) {

	const payload = block.payload;
	if (payload.length < 16) throw new Error(`Truncated IMHD at ${block.offset}`);
	return {
		objectId: readUInt16LE(payload, 0),
		imageCount: readUInt16LE(payload, 2),
		flags: payload[6],
		width: readUInt16LE(payload, 12),
		height: readUInt16LE(payload, 14),
		hotspot: payload.length >= 20 ? { x: readInt16LE(payload, 16), y: readInt16LE(payload, 18) } : null,
	};
}

function parseObjectImage(bytes, block) {

	const header = requireChild(bytes, block, 'IMHD');
	const imageHeader = parseImageHeader(bytes, header);
	const imageBlocks = parseChildren(bytes, block).filter(child => /^IM[0-9A-F]{2}$/.test(child.tag));
	const images = imageBlocks.map(image => {
		const smap = findChild(bytes, image, 'SMAP');
		if (!smap || imageHeader.width < 1 || imageHeader.height < 1 || imageHeader.width % 8 !== 0) {
			return { tag: image.tag, offset: image.offset, size: image.size, smap: null, bitmap: null };
		}
		try {
			return {
				tag: image.tag,
				offset: image.offset,
				size: image.size,
				smap: { offset: smap.offset, size: smap.size },
				bitmap: decodeSmap(bytes, smap, imageHeader.width, imageHeader.height)
			};
		} catch (error) {
			return {
				tag: image.tag,
				offset: image.offset,
				size: image.size,
				smap: { offset: smap.offset, size: smap.size },
				bitmap: null,
				decodeError: error.message
			};
		}
	});
	return {
		blockOffset: block.offset,
		blockSize: block.size,
		...imageHeader,
		images,
		_bitmapData: images.filter(image => image.bitmap).map(image => ({ tag: image.tag, bitmap: image.bitmap })),
	};
}

function parseObjectCode(bytes, block) {

	const header = findChild(bytes, block, 'CDHD');
	if (!header || header.payload.length < 15) return { blockOffset: block.offset, blockSize: block.size };
	const payload = header.payload;
	return {
		blockOffset: block.offset,
		blockSize: block.size,
		objectId: readUInt16LE(payload, 0),
		x: readInt16LE(payload, 2),
		y: readInt16LE(payload, 4),
		width: readUInt16LE(payload, 6),
		height: readUInt16LE(payload, 8),
		flags: payload[10],
		parent: payload[11],
		walkX: readInt16LE(payload, 12),
		walkY: readInt16LE(payload, 14),
		direction: payload[16],
	};
}

function paletteFromClut(clut) {

	if (!clut || clut.payload.length < 256 * 3) throw new Error('Room CLUT does not contain 256 RGB entries');
	return new Uint8Array(clut.payload.subarray(0, 256 * 3));
}

function parseRoomBlocks(bytes, room) {

	const blocks = parseChildren(bytes, room);
	const rmhd = requireChild(bytes, room, 'RMHD');
	const rmim = requireChild(bytes, room, 'RMIM');
	const im00 = requireChild(bytes, rmim, 'IM00');
	const smap = requireChild(bytes, im00, 'SMAP');
	return { blocks, rmhd, rmim, im00, smap };
}

export function parseRoom(bytes, room, options = {}) {

	if (room.tag !== 'ROOM') throw new Error(`Expected ROOM resource, got ${room.tag}`);
	const { blocks, rmhd, rmim, im00, smap } = parseRoomBlocks(bytes, room);
	if (rmhd.payload.length < 6) throw new Error('Truncated RMHD block');
	const width = readUInt16LEFromBlock(rmhd, 0);
	const height = readUInt16LEFromBlock(rmhd, 2);
	const objectCount = readUInt16LEFromBlock(rmhd, 4);
	const clut = requireChild(bytes, room, 'CLUT');
	const cycl = findChild(bytes, room, 'CYCL');
	const trns = findChild(bytes, room, 'TRNS');
	const epal = findChild(bytes, room, 'EPAL');
	const decodedBackground = decodeSmap(bytes, smap, width, height, trns?.payload[0] ?? null);
	const parsedObjectImages = blocks.filter(block => block.tag === 'OBIM').map(block => parseObjectImage(bytes, block));
	const objectBitmaps = parsedObjectImages.flatMap(object => object._bitmapData.map(bitmap => ({ objectId: object.objectId, ...bitmap })));
	const objectImages = parsedObjectImages.map(({ _bitmapData, ...object }) => ({
		...object,
		images: object.images.map(({ bitmap, ...image }) => image)
	}));
	const objectCodes = blocks.filter(block => block.tag === 'OBCD').map(block => parseObjectCode(bytes, block));
	const objectIds = new Set([...objectImages, ...objectCodes].map(object => object.objectId).filter(Number.isInteger));
	const objects = [...objectIds].sort((a, b) => a - b).map(objectId => ({
		objectId,
		image: objectImages.find(image => image.objectId === objectId) ?? null,
		code: objectCodes.find(code => code.objectId === objectId) ?? null,
	}));

	return {
		roomId: options.roomId ?? null,
		block: { offset: room.offset, size: room.size, tag: room.tag },
		width,
		height,
		objectCount,
		palette: paletteFromClut(clut),
		background: decodedBackground,
		blocks: blocks.map(block => ({ tag: block.tag, offset: block.offset, size: block.size })),
		image: {
			rmim: { offset: rmim.offset, size: rmim.size },
			im00: { offset: im00.offset, size: im00.size },
			smap: { offset: smap.offset, size: smap.size },
			codecSummary: codecSummary(decodedBackground.strips),
			strips: decodedBackground.strips,
		},
		paletteBlocks: {
			clut: { offset: clut.offset, size: clut.size, entries: 256 },
			cycl: cycl ? { offset: cycl.offset, size: cycl.size, payload: [...cycl.payload] } : null,
			trns: trns ? { offset: trns.offset, size: trns.size, transparentIndex: trns.payload[0] } : null,
			epal: epal ? { offset: epal.offset, size: epal.size, payloadBytes: epal.payload.length } : null,
		},
		objects,
		objectImages,
		objectBitmaps,
		objectCodes,
		sequence: {
			status: 'not-captured',
			reason: 'The indexed room extractor does not invent actor motion; capture remains an engine integration step.',
		},
	};
}

export function toRgbImage(indexed, palette) {

	const rgb = new Uint8Array(indexed.length * 3);
	for (let i = 0; i < indexed.length; i++) {
		const source = indexed[i] * 3;
		rgb[i * 3] = palette[source];
		rgb[i * 3 + 1] = palette[source + 1];
		rgb[i * 3 + 2] = palette[source + 2];
	}
	return rgb;
}
