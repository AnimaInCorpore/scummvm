#!/usr/bin/env node
// Writes a kyra.dat that holds only the data of the DOS versions of Eye of
// the Beholder 1 and 2, for the Atari STE build (build-ste-eob.sh).
//
// The full kyra.dat holds 5,442 entries for every Kyrandia, Lands of Lore and
// Eye of the Beholder release. Kyra reads the whole archive index at start
// and keeps it in a hash map, which on an 8 MHz 68000 takes a long part of
// the start and several hundred KiB of the 4 MiB. The output keeps INDEX
// (listing only the kept games), their ID maps and every entry those maps
// name, in the same PAK layout and with the same MD5 trailer that
// devtools/create_kyradat writes, so other builds accept it too.
//
// usage: node devtools/atari-ste/tools/kyradat-eob.mjs [in] [out]
//   in   defaults to dists/engine-data/kyra.dat
//   out  defaults to build-ste-eob/HD/SCUMMVM/KYRA.DAT

import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';

const repo = fileURLToPath(new URL('../../..', import.meta.url));
const input = process.argv[2] || `${repo}dists/engine-data/kyra.dat`;
const output = process.argv[3] || `${repo}build-ste-eob/HD/SCUMMVM/KYRA.DAT`;

// Game IDs as in engines/kyra/resource/staticres.cpp: 3 = EOB1, 4 = EOB2.
// Platform 0 = DOS; special 0 = the regular (non-talkie, non-demo) release.
const keepGame = def => {
	const game = def >> 12, platform = (def >> 8) & 0xF, special = (def >> 4) & 0xF;
	return (game === 3 || game === 4) && platform === 0 && special === 0;
};

const data = readFileSync(input);
const body = data.subarray(0, data.length - 16);
const digest = createHash('md5').update(body).digest();
if (!digest.equals(data.subarray(data.length - 16)))
	throw Error(`${input}: MD5 trailer does not match`);

// Index: [u32 LE offset][name\0] per entry, then [u32 LE end][5 zero bytes].
const entries = [];
let pos = 0;
for (;;) {
	const offset = body.readUInt32LE(pos);
	pos += 4;
	const end = body.indexOf(0, pos);
	const name = body.toString('latin1', pos, end);
	pos = end + 1;
	if (!name) {
		if (entries.length)
			entries[entries.length - 1].end = offset;
		break;
	}
	if (entries.length)
		entries[entries.length - 1].end = offset;
	entries.push({ name, offset });
}
const files = new Map(entries.map(e => [e.name, body.subarray(e.offset, e.end)]));

const index = files.get('INDEX');
const version = index.readUInt32BE(0);
const games = [];
for (let i = 0; i < index.readUInt32BE(4); ++i)
	games.push(index.readUInt16BE(8 + 2 * i));
const kept = games.filter(keepGame);
if (!kept.length)
	throw Error('no DOS Eye of the Beholder data in the input');

const out = new Map();
const newIndex = Buffer.alloc(8 + 2 * kept.length);
newIndex.writeUInt32BE(version, 0);
newIndex.writeUInt32BE(kept.length, 4);
kept.forEach((def, i) => newIndex.writeUInt16BE(def, 8 + 2 * i));
out.set('INDEX', newIndex);

// ID map "0GPS000L": [u16 BE count] then per ID [u16 id][u8 type][u32 file].
for (const def of kept) {
	const g = def >> 12, p = (def >> 8) & 0xF, s = (def >> 4) & 0xF, l = def & 0xF;
	const mapName = `0${g.toString(16)}${p.toString(16)}${s.toString(16)}000${l.toString(16)}`.toUpperCase();
	const map = files.get(mapName);
	if (!map)
		throw Error(`ID map ${mapName} missing`);
	out.set(mapName, map);
	for (let i = 0, n = map.readUInt16BE(0); i < n; ++i) {
		const file = map.readUInt32BE(2 + 7 * i + 3).toString(16).toUpperCase().padStart(8, '0');
		if (!files.has(file))
			throw Error(`${mapName} names missing entry ${file}`);
		out.set(file, files.get(file));
	}
}

const names = [...out.keys()];
const tableSize = names.reduce((sum, n) => sum + 4 + n.length + 1, 0);
let address = tableSize + 4 + 5;
const header = [];
for (const name of names) {
	const entry = Buffer.alloc(4 + name.length + 1);
	entry.writeUInt32LE(address, 0);
	entry.write(name, 4, 'latin1');
	header.push(entry);
	address += out.get(name).length;
}
const end = Buffer.alloc(9);
end.writeUInt32LE(address, 0);
const pak = Buffer.concat([...header, end, ...out.values()]);
writeFileSync(output, Buffer.concat([pak, createHash('md5').update(pak).digest()]));

const langs = kept.map(d => `EOB${(d >> 12) - 2}/lang ${d & 0xF}`).join(', ');
console.log(`${output}: ${out.size} of ${entries.length} entries, ${pak.length + 16} bytes (${langs})`);
