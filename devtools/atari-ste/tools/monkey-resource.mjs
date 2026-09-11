import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// Game data stays untracked in the main checkout's assets/; worktrees such as
// scummvm-ste-scene find it in the sibling scummvm checkout.
const REPO = fileURLToPath(new URL('../../..', import.meta.url));
export const DEFAULT_MONKEY_DATA_DIR = process.env.MONKEY_DATA_DIR
	?? [`${REPO}assets/monkey-cd`, `${REPO}../scummvm/assets/monkey-cd`].find(dir => existsSync(dir))
	?? `${REPO}assets/monkey-cd`;
export const RESOURCE_XOR = 0x69;

function readUInt32BE(bytes, offset) {

	if (offset < 0 || offset + 4 > bytes.length) throw new Error(`Out-of-range uint32 at ${offset}`);
	return (bytes[offset] * 0x1000000) + (bytes[offset + 1] << 16) + (bytes[offset + 2] << 8) + bytes[offset + 3];
}

function readUInt32LE(bytes, offset) {

	if (offset < 0 || offset + 4 > bytes.length) throw new Error(`Out-of-range uint32 at ${offset}`);
	return bytes[offset] + (bytes[offset + 1] << 8) + (bytes[offset + 2] << 16) + (bytes[offset + 3] * 0x1000000);
}

function readUInt16LE(bytes, offset) {

	if (offset < 0 || offset + 2 > bytes.length) throw new Error(`Out-of-range uint16 at ${offset}`);
	return bytes[offset] | (bytes[offset + 1] << 8);
}

function fourCC(bytes, offset) {

	if (offset < 0 || offset + 4 > bytes.length) throw new Error(`Out-of-range tag at ${offset}`);
	return String.fromCharCode(bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]);
}

export function sha256(bytes) {

	return createHash('sha256').update(bytes).digest('hex');
}

export function decodeResourceBytes(bytes, xor = RESOURCE_XOR) {

	return Uint8Array.from(bytes, value => value ^ xor);
}

export function parseBlock(bytes, offset = 0, limit = bytes.length) {

	if (offset + 8 > limit) throw new Error(`Truncated resource block header at ${offset}`);
	const tag = fourCC(bytes, offset);
	const size = readUInt32BE(bytes, offset + 4);
	if (size < 8 || offset + size > limit) {
		throw new Error(`Invalid ${tag} block size ${size} at ${offset} (limit ${limit})`);
	}
	return {
		tag,
		offset,
		size,
		end: offset + size,
		payloadOffset: offset + 8,
		payload: bytes.subarray(offset + 8, offset + size),
	};
}

export function childBlocks(block) {

	if (!block._bytes) throw new Error('childBlocks requires a block returned with an attached byte view');
	return parseChildren(block._bytes, block);
}

export function parseChildren(bytes, block) {

	const children = [];
	for (let offset = block.payloadOffset; offset < block.end;) {
		const child = parseBlock(bytes, offset, block.end);
		children.push(child);
		offset = child.end;
	}
	return children;
}

export function findChild(bytes, block, tag) {

	return parseChildren(bytes, block).find(child => child.tag === tag) ?? null;
}

export function listTopLevelBlocks(bytes, root) {

	return parseChildren(bytes, root);
}

function parseContainerFile(path) {

	const encoded = readFileSync(path);
	const bytes = decodeResourceBytes(encoded);
	const root = parseBlock(bytes);
	return { path, encoded, bytes, root, sha256: sha256(encoded) };
}

function parseLoff(bytes, root) {

	if (root.tag !== 'LECF') return [];
	const loff = findChild(bytes, root, 'LOFF');
	if (!loff) return [];
	if (loff.payload.length < 1) throw new Error(`Empty LOFF block in ${root.tag}`);
	const count = loff.payload[0];
	const expected = 1 + count * 5;
	if (loff.payload.length < expected) {
		throw new Error(`Truncated LOFF table: ${loff.payload.length} bytes, expected ${expected}`);
	}
	const entries = [];
	for (let i = 0, offset = 1; i < count; i++, offset += 5) {
		entries.push({ resourceId: loff.payload[offset], fileOffset: readUInt32LE(loff.payload, offset + 1) });
	}
	return entries;
}

export function loadMonkeyContainers(dataDir = DEFAULT_MONKEY_DATA_DIR, names = ['MONKEY.000', 'MONKEY.001']) {

	return names.map(name => {
		const container = parseContainerFile(`${dataDir}/${name}`);
		return {
			name,
			path: container.path,
			bytes: container.bytes,
			encodedBytes: container.encoded.length,
			sha256: container.sha256,
			root: container.root,
			loff: parseLoff(container.bytes, container.root),
		};
	});
}

export function resolveRoom(containers, roomId) {

	for (const container of containers) {
		const entry = container.loff.find(item => item.resourceId === roomId);
		if (!entry) continue;
		const room = parseBlock(container.bytes, entry.fileOffset, container.bytes.length);
		if (room.tag !== 'ROOM') {
			throw new Error(`${container.name} LOFF room ${roomId} points to ${room.tag}, not ROOM`);
		}
		return { container, entry, room };
	}
	throw new Error(`Room ${roomId} was not found in the supplied MONKEY containers`);
}

export function readUInt16LEFromBlock(block, offset) {

	return readUInt16LE(block.payload, offset);
}

export function readUInt32BEFromBlock(block, offset) {

	return readUInt32BE(block.payload, offset);
}
