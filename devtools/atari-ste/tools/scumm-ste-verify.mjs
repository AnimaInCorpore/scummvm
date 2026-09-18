// Checks a scumm-ste-capture.mjs capture: rebuilds the frame Hatari should
// have displayed from the dumped four-plane screen and the packed palette
// schedule, following the write model of atari-ste-raster.S, and compares it
// pixel by pixel with Hatari's screenshot.
//
//   node devtools/atari-ste/tools/scumm-ste-verify.mjs OUTDIR VBL [LINES]
import { readFileSync } from 'node:fs';
import { readPng, writePng } from './png.mjs';
import { getSpectrum512ColorSlotIndex as slotAt } from './spectrum512-slots.mjs';

const [dir, vbl, linesArg] = process.argv.slice(2);
const lines = Number(linesArg || 144);
const screen = readFileSync(`${dir}/screen-${vbl}.bin`);
const schedule = readFileSync(`${dir}/palette-${vbl}.bin`);
const shot = readPng(`${dir}/shot-${vbl}.png`);
const scale = shot.width / 320;
if (shot.width !== 320 * scale || shot.height !== 200 * scale) throw Error(`Unexpected screenshot size ${shot.width}x${shot.height}`);

const word = i => schedule.readUInt16BE(i * 2);
// STE register nibble -> linear 0..15 -> 8 bit.
const channel = nibble => (((nibble & 7) << 1) | (nibble >> 3)) * 17;
const rgbOf = w => [channel((w >> 8) & 15), channel((w >> 4) & 15), channel(w & 15)];
const group = (line, g) => Array.from({ length: 16 }, (_, i) => word(32 + 48 * (line - 2) + 16 * g + i));
const header = g => Array.from({ length: 16 }, (_, i) => word(16 * g + i));

// Palette state at the start of each line and the two visible groups.
const start = [], group1 = [], group2 = [];
for (let y = 0; y < 200; y++) {
	if (y < 2) { start[y] = header(0); group1[y] = group2[y] = null; }
	else if (y === 2) { start[y] = header(1); group1[y] = group(2, 0); group2[y] = group(2, 1); }
	else if (y < lines) { start[y] = group(y - 1, 2); group1[y] = group(y, 0); group2[y] = group(y, 1); }
	else { start[y] = group(lines - 1, 2); group1[y] = group2[y] = null; }
}
const index = (x, y) => {
	const block = y * 160 + (x >> 4) * 8, bit = 15 - (x & 15);
	let v = 0;
	for (let p = 0; p < 4; p++) v |= ((screen.readUInt16BE(block + p * 2) >> bit) & 1) << p;
	return v;
};
const visible = (x, y, i) => {
	if (!group1[y]) return start[y][i];
	const slot = slotAt(x, i);
	return slot < 16 ? start[y][i] : slot < 32 ? group1[y][i] : group2[y][i];
};

const expected = new Uint8Array(320 * 200 * 3);
const perLine = new Array(200).fill(0);
let mismatches = 0, first = [];
for (let y = 0; y < 200; y++) for (let x = 0; x < 320; x++) {
	const rgb = rgbOf(visible(x, y, index(x, y)));
	expected.set(rgb, (y * 320 + x) * 3);
	const o = ((y * scale) * shot.width + x * scale) * 4;
	const got = [shot.rgba[o], shot.rgba[o + 1], shot.rgba[o + 2]];
	if (got.some((v, i) => v !== rgb[i])) {
		mismatches++; perLine[y]++;
		if (first.length < 8) first.push(`(${x},${y}) index ${index(x, y)} expected ${rgb} got ${got}`);
	}
}
writePng(`${dir}/expected-${vbl}.png`, 320, 200, expected);
const badLines = perLine.map((n, y) => n ? `${y}:${n}` : null).filter(Boolean);
console.log(`VBL ${vbl}: ${mismatches} mismatching pixels of 64000 over ${badLines.length} lines`);
if (badLines.length) console.log(`lines: ${badLines.slice(0, 40).join(' ')}${badLines.length > 40 ? ' ...' : ''}`);
for (const f of first) console.log(`  ${f}`);
// Distinct colours per line show that the palette differs across lines.
const distinct = new Set();
for (let y = 0; y < lines; y++) for (const w of start[y]) distinct.add(w);
console.log(`distinct start-of-line colours over the ${lines} raster lines: ${distinct.size}`);
process.exitCode = mismatches ? 1 : 0;
