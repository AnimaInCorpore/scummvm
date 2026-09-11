const CODEC_NAMES = new Map([
	[18, 'zigzag-vertical-v8'],
	[28, 'zigzag-horizontal-h8'],
	[68, 'major-minor-horizontal-h8'],
]);

class DecodeError extends Error {

	constructor(message, offset = 0) {
		super(`${message} at strip byte ${offset}`);
		this.name = 'DecodeError';
	}
}

class LsbBitReader {

	constructor(bytes, offset = 0, initialBits = 0, initialCount = 0) {
		this.bytes = bytes;
		this.offset = offset;
		this.bits = initialBits;
		this.count = initialCount;
	}

	fill(minimum) {

		while (this.count < minimum) {
			if (this.offset >= this.bytes.length) throw new DecodeError('Unexpected end of compressed strip', this.offset);
			this.bits |= this.bytes[this.offset++] << this.count;
			this.count += 8;
		}
	}

	fillLegacy() {

		if (this.count <= 8) {
			if (this.offset >= this.bytes.length) throw new DecodeError('Unexpected end of compressed strip', this.offset);
			this.bits |= this.bytes[this.offset++] << this.count;
			this.count += 8;
		}
	}

	read(count) {

		if (count === 0) return 0;
		this.fill(count);
		const value = this.bits & ((1 << count) - 1);
		this.bits >>>= count;
		this.count -= count;
		return value;
	}

	readLegacy(count) {

		if (count === 0) return 0;
		this.fillLegacy();
		const value = this.bits & ((1 << count) - 1);
		this.bits >>>= count;
		this.count -= count;
		return value;
	}

	readRaw(count) {

		if (count === 0) return 0;
		if (this.count < count) throw new DecodeError('Compressed strip bit stream ended', this.offset);
		const value = this.bits & ((1 << count) - 1);
		this.bits >>>= count;
		this.count -= count;
		return value;
	}

	get position() {

		return this.offset;
	}
}

function updateColor(color, reader, shift, state) {

	if (!reader.readRaw(1)) return color;
	if (!reader.readRaw(1)) {
		state.increment = -1;
		reader.fillLegacy();
		return reader.readRaw(shift);
	}
	if (!reader.readRaw(1)) return (color + state.increment) & 0xff;
	state.increment = -state.increment;
	return (color + state.increment) & 0xff;
}

function decodeZigzag(bytes, height, vertical) {

	if (bytes.length < 3) throw new DecodeError('Missing zigzag strip header');
	const code = bytes[0];
	const shift = code % 10;
	const reader = new LsbBitReader(bytes, 3, bytes[2], 8);
	const pixels = new Uint8Array(8 * height);
	const state = { increment: -1 };
	let color = bytes[1];
	if (vertical) {
		for (let x = 0; x < 8; x++) {
			for (let y = 0; y < height; y++) {
				pixels[y * 8 + x] = color;
				if (x !== 7 || y !== height - 1) {
					reader.fillLegacy();
					color = updateColor(color, reader, shift, state);
				}
			}
		}
	} else {
		for (let y = 0; y < height; y++) {
			for (let x = 0; x < 8; x++) {
				pixels[y * 8 + x] = color;
				if (x !== 7 || y !== height - 1) {
					reader.fillLegacy();
					color = updateColor(color, reader, shift, state);
				}
			}
		}
	}
	return { pixels, consumed: reader.position };
}

function decodeMajorMinor(bytes, height) {

	if (bytes.length < 4) throw new DecodeError('Missing major-minor strip header');
	const reader = new LsbBitReader(bytes, 4, bytes[2] | (bytes[3] << 8), 16);
	let color = bytes[1];
	let repeatCount = 0;
	const pixels = new Uint8Array(8 * height);
	for (let y = 0; y < height; y++) {
		for (let x = 0; x < 8; x++) {
			pixels[y * 8 + x] = color;
			if (repeatCount) {
				if (--repeatCount === 0) repeatCount = 0;
				continue;
			}
			if (!reader.readLegacy(1)) continue;
			if (!reader.readLegacy(1)) {
				color = reader.readLegacy(8);
				continue;
			}
			const delta = reader.readLegacy(3) - 4;
			if (delta) color = (color + delta) & 0xff;
			else repeatCount = reader.readLegacy(8) - 1;
		}
	}
	return { pixels, consumed: reader.position };
}

export function codecName(code) {

	return CODEC_NAMES.get(code) ?? `unsupported-${code}`;
}

export function decodeStrip(bytes, height, transparency = null) {

	if (bytes.length < 1) throw new DecodeError('Empty strip');
	const code = bytes[0];
	let result;
	if (code === 18) result = decodeZigzag(bytes, height, true);
	else if (code === 28) result = decodeZigzag(bytes, height, false);
	else if (code === 68) result = decodeMajorMinor(bytes, height);
	else throw new DecodeError(`Unsupported SCUMM V5 strip codec ${code}`, 0);
	if (transparency !== null) {
		for (let i = 0; i < result.pixels.length; i++) {
			if (result.pixels[i] === transparency) result.pixels[i] = transparency;
		}
	}
	return { ...result, code, codec: codecName(code) };
}

export function decodeSmap(bytes, smap, width, height, transparency = null) {

	if (smap.tag !== 'SMAP') throw new Error(`Expected SMAP, got ${smap.tag}`);
	if (width % 8 !== 0) throw new Error(`SCUMM strip decoder requires an 8-pixel-aligned width, got ${width}`);
	const stripCount = width / 8;
	const tableEnd = smap.payloadOffset + stripCount * 4;
	if (tableEnd > smap.end) throw new Error('SMAP strip offset table is truncated');
	const pixels = new Uint8Array(width * height);
	const strips = [];
	for (let strip = 0; strip < stripCount; strip++) {
		const tableOffset = smap.payloadOffset + strip * 4;
		const offset = bytes[tableOffset] | (bytes[tableOffset + 1] << 8) | (bytes[tableOffset + 2] << 16) | (bytes[tableOffset + 3] * 0x1000000);
		if (offset < 8 || smap.offset + offset >= smap.end) {
			throw new Error(`SMAP strip ${strip} has invalid relative offset ${offset}`);
		}
		const stripStart = smap.offset + offset;
		// The original decoder reads one or two padding bytes after the final
		// compressed strip while priming its bit reader. Keep that read bounded
		// and deterministic instead of allowing it to consume the next resource.
		const stripBytes = new Uint8Array(smap.end - stripStart + 8);
		stripBytes.set(bytes.subarray(stripStart, smap.end));
		const decoded = decodeStrip(stripBytes, height, transparency);
		for (let y = 0; y < height; y++) pixels.set(decoded.pixels.subarray(y * 8, y * 8 + 8), y * width + strip * 8);
		strips.push({ strip, offset, code: decoded.code, codec: decoded.codec, compressedBytes: decoded.consumed });
	}
	return { width, height, pixels, strips };
}

export function codecSummary(strips) {

	const summary = {};
	for (const strip of strips) summary[strip.codec] = (summary[strip.codec] ?? 0) + 1;
	return summary;
}
