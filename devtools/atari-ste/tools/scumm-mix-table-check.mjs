// What a mixing table does to every colour a room can show.
//
// monkey-flicker-compare.mjs reports the error of the frames it fitted, which
// is dominated by the background. This reads a table back and compares, per
// palette index, the colour its pair produces with the colour the table was
// built for, so a costume colour that covers few pixels counts as much as the
// wall behind it. Both come from scumm-scene-colours.mjs's union, which says
// which indices the room can show at all and which of them only a costume uses.
//
//   node devtools/atari-ste/tools/scumm-mix-table-check.mjs TABLE.bin COLOURS.json
//
// Errors are squared Oklab distances on the evaluator's 127 scale; "visibly
// off" is more than 0.06 in plain Oklab, as there.
import { readFileSync } from 'node:fs';

const [table, unionFile] = process.argv.slice(2);
if (!table || !unionFile) throw Error('Usage: scumm-mix-table-check.mjs TABLE.bin COLOURS.json');

// The STE word keeps the lowest bit of each 4-bit channel in the high bit.
const fromWord = word => (((word & 0x777) << 1) | ((word & 0x888) >> 3)) & 0xfff;
const srgbToLinear = value => {
	const s = value / 255;
	return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
};
const oklab = (r, g, b) => {
	const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
	const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
	const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
	return [
		(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s) * 127,
		(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s) * 127,
		(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s) * 127,
	];
};

const lut = readFileSync(table);
const union = JSON.parse(readFileSync(unionFile, 'utf8'));
if (union.schema !== 'scumm-ste-scene-colours/1') throw Error(`${unionFile} is not a scene-colour union`);
// palette words, then per region 256 pairs, then the 768-byte VGA palette.
const regions = 2;
const fields = (lut.length - regions * 512 - 768) / (regions * 16 * 2);
if (!(fields === 1 || fields === 2)) throw Error(`Unexpected table size ${lut.length}`);
const words = fields * regions * 16;
const palette = (field, region) => Array.from({ length: 16 },
	(_, slot) => fromWord(lut.readUInt16BE(((field * regions + region) * 16 + slot) * 2)));
const slots = region => lut.subarray(words * 2 + region * 512, words * 2 + (region + 1) * 512);
const source = lut.subarray(words * 2 + regions * 512);

// The room region; the verb bar has its own pairs and no costumes.
const region = 0, pairs = slots(region), first = palette(0, region), second = palette(fields - 1, region);
const costumeOnly = new Set(union.indices.costumeOnly);
const rows = [];
for (let index = 0; index < 256; index++) {
	if (!(union.indices.total[index] > 0)) continue;
	const a = first[pairs[index * 2]], b = second[pairs[index * 2 + 1]];
	const mixed = oklab(...[0, 1, 2].map(channel => {
		const shift = 8 - 4 * channel;
		return (srgbToLinear(((a >> shift) & 15) * 17) + srgbToLinear(((b >> shift) & 15) * 17)) / 2;
	}));
	const wanted = oklab(...[0, 1, 2].map(channel => srgbToLinear(source[index * 3 + channel])));
	const error = mixed.reduce((sum, value, i) => sum + (value - wanted[i]) ** 2, 0);
	rows.push({ index, error, distance: Math.sqrt(error) / 127, costume: costumeOnly.has(index) });
}

const report = (name, subset) => {
	if (!subset.length) return;
	const mean = subset.reduce((sum, row) => sum + row.error, 0) / subset.length;
	const off = subset.filter(row => row.distance > 0.06).length;
	console.log(`  ${name}: ${subset.length} indices, mean ${mean.toFixed(2)}, ${off} visibly off`);
};
console.log(`${table} against room ${union.room} of ${union.game}`);
report('all colours the room can show', rows);
report('only a costume uses', rows.filter(row => row.costume));
report('background and objects', rows.filter(row => !row.costume));
console.log(`  worst: ${[...rows].sort((a, b) => b.error - a.error).slice(0, 6)
	.map(row => `${row.index}${row.costume ? '*' : ''}:${row.distance.toFixed(3)}`).join(' ')} (* only a costume)`);
