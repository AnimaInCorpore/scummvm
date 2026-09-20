import { loadMonkeyContainers, parseBlock } from './monkey-resource.mjs';

function readU16(bytes, offset) {
	return bytes[offset] | (bytes[offset + 1] << 8);
}

function readS16(bytes, offset) {
	const value = readU16(bytes, offset);
	return value & 0x8000 ? value - 0x10000 : value;
}

function findDirectoryBlock(bytes, tag) {
	for (let offset = 0; offset + 8 <= bytes.length;) {
		const currentTag = String.fromCharCode(bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]);
		const size = (bytes[offset + 4] * 0x1000000) + (bytes[offset + 5] << 16) + (bytes[offset + 6] << 8) + bytes[offset + 7];
		if (currentTag === tag) return { offset, size, payloadOffset: offset + 8 };
		if (size < 8 || offset + size > bytes.length) break;
		offset += size;
	}
	throw new Error(`Index block ${tag} was not found`);
}

// The index container holds DCOS, the data container the LOFF room table;
// which file that is depends on the game, so both are found by content.
function indexContainer(containers) {
	return containers.find(container => !container.loff.length) ?? containers[0];
}

function dataContainer(containers) {
	return containers.find(container => container.loff.length) ?? containers.at(-1);
}

export function costumeDirectory(containers) {
	return readCostumeDirectory(indexContainer(containers));
}

function readCostumeDirectory(container) {
	const block = findDirectoryBlock(container.bytes, 'DCOS');
	const count = readU16(container.bytes, block.payloadOffset);
	const roomsOffset = block.payloadOffset + 2;
	const offsetsOffset = roomsOffset + count;
	const resources = [];
	for (let id = 0; id < count; id++) {
		resources.push({
			id,
			room: container.bytes[roomsOffset + id],
			offset: container.bytes[offsetsOffset + id * 4]
				| (container.bytes[offsetsOffset + id * 4 + 1] << 8)
				| (container.bytes[offsetsOffset + id * 4 + 2] << 16)
				| (container.bytes[offsetsOffset + id * 4 + 3] * 0x1000000),
		});
	}
	return resources;
}

function readResource(containers, resource) {
	if (!resource || resource.room === 0 || resource.offset === 0) throw new Error('Costume resource is not present');
	const container = dataContainer(containers);
	const room = container.loff.find(entry => entry.resourceId === resource.room);
	if (!room) throw new Error(`Room file ${resource.room} for costume ${resource.id} was not found`);
	const block = parseBlock(container.bytes, room.fileOffset + resource.offset);
	if (block.tag !== 'COST') throw new Error(`Costume ${resource.id} points to ${block.tag}, not COST`);
	return { container, block };
}

function parseCostume(containers, resource) {
	const { block } = readResource(containers, resource);
	const bytes = block._bytes ?? dataContainer(containers).bytes;
	const base = block.offset + 2;
	const format = bytes[base + 7] & 0x7f;
	const numColors = format === 0x57 ? 0 : format === 0x59 ? 32 : format === 0x58 ? 16 : 0;
	if (!numColors) throw new Error(`Costume ${resource.id} has unsupported format 0x${format.toString(16)}`);
	const palette = Array.from(bytes.subarray(base + 8, base + 8 + numColors));
	const frameTable = base + 8 + numColors;
	const frameOffsets = frameTable + 2;
	const dataOffsets = format === 0x57 ? frameTable + 18 : frameTable + 34;
	const animationCommands = base + readU16(bytes, frameTable);
	return {
		id: resource.id,
		format,
		numColors,
		numAnimations: bytes[base + 6],
		palette,
		bytes,
		base,
		frameOffsets,
		dataOffsets,
		animationCommands,
	};
}

function readAnimation(costume, animation) {
	if (animation > costume.numAnimations) return [];
	const { bytes, base, dataOffsets, animationCommands } = costume;
	const offset = readU16(bytes, dataOffsets + animation * 2);
	if (offset === 0) return [];
	let cursor = base + offset;
	let mask = readU16(bytes, cursor);
	cursor += 2;
	const limbs = [];
	for (let limb = 0; limb < 16 && mask; limb++, mask = (mask << 1) & 0xffff) {
		if (!(mask & 0x8000)) continue;
		const position = readU16(bytes, cursor);
		cursor += 2;
		if (position === 0xffff) {
			limbs.push({ limb, stopped: true });
			continue;
		}
		const extra = bytes[cursor++];
		const command = bytes[animationCommands + position];
		if (command === 0x7a || command === 0x79) {
			limbs.push({ limb, stopped: true });
			continue;
		}
		limbs.push({
			limb,
			start: position,
			end: position + (extra & 0x7f),
			oneshoot: Boolean(extra & 0x80),
		});
	}
	return limbs;
}

// Where a limb's cel of one animation position starts, or null for no cel.
// Cels are shared between animations and steps, so this is also the key that
// lets costumeColourCounts() decode each of them once.
function celSource(costume, limb, position) {
	const { bytes, base, frameOffsets, animationCommands } = costume;
	const framePointer = base + readU16(bytes, frameOffsets + limb * 2);
	const command = bytes[animationCommands + position] & 0x7f;
	if (command === 0x7b) return null;
	return base + readU16(bytes, framePointer + command * 2);
}

function readCel(costume, limb, position) {
	const { bytes } = costume;
	const source = celSource(costume, limb, position);
	if (source === null) return null;
	const width = readU16(bytes, source);
	const height = readU16(bytes, source + 2);
	// A cel cannot be larger than the screen; anything else is a bad pointer.
	if (width > 320 || height > 200 || !width || !height)
		return null;
	const cel = {
		width,
		height,
		relX: readS16(bytes, source + 4),
		relY: readS16(bytes, source + 6),
		moveX: readS16(bytes, source + 8),
		moveY: readS16(bytes, source + 10),
		pixels: new Uint8Array(width * height),
	};
	let cursor = source + 12;
	const colorShift = costume.numColors === 32 ? 3 : 4;
	const colorMask = costume.numColors === 32 ? 7 : 15;
	for (let x = 0; x < width; x++) {
		let y = 0;
		while (y < height) {
			// Data that ends inside the cel means the stream is not this cel's.
			if (cursor >= bytes.length) return cel;
			const run = bytes[cursor++];
			const color = run >> colorShift;
			let length = run & colorMask;
			// A separate length byte of zero is 256: the original decrements an
			// 8-bit counter before testing it. Without this the run draws
			// nothing and the strip never advances.
			if (!length) length = bytes[cursor++] || 256;
			for (let i = 0; i < length && y < height; i++) cel.pixels[y++ * width + x] = color;
		}
	}
	return cel;
}

export function loadCostume(containers, costumeId) {
	const resource = costumeDirectory(containers).find(candidate => candidate.id === costumeId);
	if (!resource) throw new Error(`Costume ${costumeId} is outside the directory`);
	return { resource, costume: parseCostume(containers, resource) };
}

export function loadMonkeyCostume(costumeId, dataDir) {
	return loadCostume(loadMonkeyContainers(dataDir), costumeId);
}

// Pixels per costume colour over every animation step, and the number of steps
// that draw anything. Every cel is decoded once, however many steps show it:
// compositing each step instead costs minutes for a costume with sixty
// animations. Colour 0 is transparent, as in renderCostumeFrame().
export function costumeColourCounts(costume) {
	const histograms = new Map();
	const counts = new Map();
	let steps = 0, rejected = 0;

	for (const { animation, steps: stepCount } of describeCostume(costume)) {
		const limbs = readAnimation(costume, animation);
		for (let step = 0; step < stepCount; step++) {
			let drawn = false;
			for (const state of limbs) {
				if (state.stopped) continue;
				const length = state.end - state.start + 1;
				const position = state.start + (step % length);
				const source = celSource(costume, state.limb, position);
				if (source === null) continue;
				let histogram = histograms.get(source);
				if (!histogram) {
					const cel = readCel(costume, state.limb, position);
					histogram = new Map();
					if (cel) {
						for (const color of cel.pixels)
							if (color) histogram.set(color, (histogram.get(color) ?? 0) + 1);
					} else {
						// A cel larger than the screen is a bad pointer, not art.
						rejected++;
					}
					histograms.set(source, histogram);
				}
				for (const [color, pixels] of histogram) counts.set(color, (counts.get(color) ?? 0) + pixels);
				drawn = true;
			}
			if (drawn) steps++;
		}
	}
	return { counts, steps, cels: histograms.size, rejected };
}

export function describeCostume(costume) {
	const animations = [];
	for (let animation = 0; animation <= costume.numAnimations; animation++) {
		const limbs = readAnimation(costume, animation);
		if (!limbs.length) continue;
		const steps = Math.max(1, ...limbs.filter(limb => !limb.stopped).map(limb => limb.end - limb.start + 1));
		animations.push({ animation, limbs: limbs.length, steps });
	}
	return animations;
}

export function renderCostumeFrame(costume, animation, step = 0, actorX = 0, actorY = 0, mirrored = false) {
	const limbs = readAnimation(costume, animation);
	const pixels = [];
	let xMove = 0;
	let yMove = 0;
	for (const state of limbs) {
		if (!state.stopped) {
			const length = state.end - state.start + 1;
			const position = state.oneshot
				? state.start + Math.min(step, length - 1)
				: state.start + (step % length);
			const cel = readCel(costume, state.limb, position);
			if (cel) {
				for (let y = 0; y < cel.height; y++) for (let x = 0; x < cel.width; x++) {
					const color = cel.pixels[y * cel.width + x];
					if (!color) continue;
					const celX = xMove + cel.relX;
					const celY = yMove + cel.relY;
					const drawX = mirrored ? actorX - celX - cel.width + x : actorX + celX + x;
					const drawY = actorY + celY + y;
					pixels.push({ x: drawX, y: drawY, color, limb: state.limb });
				}
			}
			xMove += cel?.moveX ?? 0;
			yMove -= cel?.moveY ?? 0;
		}
	}
	return { pixels, palette: costume.palette, animation, step, actorX, actorY, mirrored };
}
