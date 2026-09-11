// Generates the per-line palette schedule for raster-test/timerb-raster-test.s: 200
// display lines, each carrying one distinct solid colour in register 0 (the
// other 15 registers left 0). The packed layout matches
// atari-ste-raster.h / scummvm-ste-scene: 16 header words for lines 0/1, then
// line 1's border group (16), then 48 words per line 2..199 (two visible
// groups + the next line's border group). timerb-raster-test.mjs checks that
// every displayed row shows its assigned colour.
import { writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const LINES = 200;
// A distinct STE hardware word per line. Spread hue across the 12-bit space so
// neighbouring lines differ a lot; register encoding is bits reordered so the
// least significant channel bit sits in bit 3 (STE order).
function lineColor(y) {
	const r = (y * 5) & 15, g = (y * 11 + 3) & 15, b = (y * 7 + 9) & 15;
	const linear = (r << 8) | (g << 4) | b;
	return ((linear & 0x0eee) >> 1) | ((linear & 0x0111) << 3);
}
const palette = y => { const p = new Array(16).fill(0); p[0] = lineColor(y); return p; };

const words = [];
const push = arr => arr.forEach(w => words.push(w));
push(palette(0));               // lines 0 and 1
push(palette(2));               // line 1 border group = line 2 start
for (let y = 2; y < LINES; y++) {
	push(palette(y));           // group 1 (left)
	push(palette(y));           // group 2 (middle)
	push(palette(Math.min(y + 1, LINES - 1))); // border = next line start
}
const buf = Buffer.alloc(words.length * 2);
words.forEach((w, i) => buf.writeUInt16BE(w & 0xffff, i * 2));
const out = resolve(process.argv[2] || 'devtools/atari-ste/raster-test/timerb-schedule.bin');
writeFileSync(out, buf);
console.log(`Wrote ${buf.length} bytes (${words.length} words) to ${out}`);
console.log(`Expected header 32 + 48*(${LINES}-2) = ${32 + 48 * (LINES - 2)} words`);
