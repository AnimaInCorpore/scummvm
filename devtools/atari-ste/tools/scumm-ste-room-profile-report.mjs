import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';

const CXXFILT = process.env.CXXFILT || fileURLToPath(new URL('../../../../cross-mint/bin/m68k-atari-mintelf-c++filt', import.meta.url));

const dir = resolve(process.argv[2]);
const fields = ['enabled', 'serial', 'phase', 'frame', 'room', 'engineActive', 'rendererActive',
	'ego', 'x', 'y', 'moving', 'cameraX', 'userPut', 'conversions', 'pixels', 'rebuilds', 'mapMisses', 'visibleActors', 'sceneHeight'];
const snapshots = readdirSync(dir).filter(n => /^state-\$[0-9a-f]+\.bin$/.test(n)).map(n => {
	const data = readFileSync(`${dir}/${n}`);
	return Object.fromEntries(fields.map((field, i) => [field, data.readUInt32BE(i * 4)]));
}).sort((a, b) => a.serial - b.serial);
const log = readFileSync(`${dir}/hatari.log`, 'utf8');
const clocks = new Map();
for (const match of log.matchAll(/STE_BENCH_EVENT> evaluate CycleCounter\s+[^\n]*\$([0-9a-f]+) \(hex\)[\s\S]*?> savebin [^\n]*\/state-\$([0-9a-f]+)\.bin/g))
	clocks.set(parseInt(match[2], 16), parseInt(match[1], 16));
const symbols = [...readFileSync(`${dir}/symbols.txt`, 'utf8').matchAll(/^([0-9a-f]+) [TtWw] (.+)$/gm)]
	.map(m => ({ address: parseInt(m[1], 16), name: m[2] })).sort((a, b) => a.address - b.address);
const offset = name => symbols.find(s => s.name === name)?.address;
const rasterLo = offset('atari_ste_vbl'), rasterHi = offset('raster_exit') + 16;
function symbolAt(pc) {
	let lo = 0, hi = symbols.length;
	while (lo < hi) {
		const mid = (lo + hi) >> 1;
		if (symbols[mid].address <= pc) lo = mid + 1; else hi = mid;
	}
	return lo ? symbols[lo - 1].name : 'unknown';
}
const hotspots = { engine: new Map(), renderer: new Map(), other: new Map() };
const frames = new Map();
let frequency = 0;
for (let i = 1; i < snapshots.length; i++) {
	const previous = snapshots[i - 1], current = snapshots[i];
	if (!clocks.has(previous.serial) || !clocks.has(current.serial))
		throw Error(`Missing clock sample at ${previous.serial}/${current.serial}`);
	const path = `${dir}/segment-$${current.serial.toString(16)}.txt`;
	const profile = readFileSync(path, 'utf8');
	const base = parseInt(profile.match(/PROGRAM_TEXT:\s+0x([0-9a-f]+)/)[1], 16);
	const textEnd = parseInt(profile.match(/PROGRAM_TEXT:\s+0x[0-9a-f]+-0x([0-9a-f]+)/)[1], 16);
	frequency = Number(profile.match(/Cycles\/second:\s+(\d+)/)[1]);
	const owner = previous.rendererActive ? 'renderer' : previous.engineActive ? 'engine' : 'other';
	let total = 0, raster = 0;
	for (const match of profile.matchAll(/^([0-9a-fA-F]+) .*% \((\d+), (\d+), \d+, \d+\)$/gm)) {
		const pc = parseInt(match[1], 16), cycles = Number(match[3]);
		total += cycles;
		if (pc >= base + rasterLo && pc < base + rasterHi) { raster += cycles; continue; }
		const name = pc >= base && pc < textEnd ? symbolAt(pc - base) : 'TOS / other memory';
		hotspots[owner].set(name, (hotspots[owner].get(name) || 0) + cycles);
	}
	if (!frames.has(previous.frame)) frames.set(previous.frame, {
		frame: previous.frame, room: previous.room, x: previous.x, y: previous.y, moving: previous.moving,
		cameraX: previous.cameraX, visibleActors: previous.visibleActors,
		engine: 0, renderer: 0, other: 0, raster: 0, total: 0, wall: 0,
		pixels: 0, rebuilds: 0, mapMisses: 0, conversions: 0,
	});
	const frame = frames.get(previous.frame);
	frame[owner] += total - raster;
	frame.raster += raster;
	frame.total += total;
	const wall = (clocks.get(current.serial) - clocks.get(previous.serial)) >>> 0;
	if (Math.abs(wall - total) > 128)
		throw Error(`Profile/clock disagreement at ${current.serial}: ${total} vs ${wall}`);
	frame.wall += wall;
	if (current.phase === 4) {
		frame.pixels = current.pixels;
		frame.rebuilds = current.rebuilds;
		frame.mapMisses = current.mapMisses;
		frame.conversions = current.conversions;
	}
}
const results = [...frames.values()];
const run = JSON.parse(readFileSync(`${dir}/run.json`));
if (results.length !== run.last - run.first || results.some((f, i) => f.frame !== run.first + i || f.room !== 28))
	throw Error('Incomplete or inconsistent room-28 frame sequence');
const average = key => Math.round(results.reduce((sum, f) => sum + f[key], 0) / results.length);
const mean = Object.fromEntries(['engine', 'renderer', 'other', 'raster', 'total', 'wall', 'pixels', 'rebuilds', 'mapMisses', 'conversions'].map(k => [k, average(k)]));
const top = {};
for (const [owner, costs] of Object.entries(hotspots)) {
	const entries = [...costs].sort((a, b) => b[1] - a[1]).slice(0, 15);
	const names = execFileSync(CXXFILT, entries.map(e => e[0]), { encoding: 'utf8' }).trim().split('\n');
	top[owner] = entries.map((entry, i) => ({ function: names[i], cyclesPerFrame: Math.round(entry[1] / results.length) }));
}
const report = {
	...run, frequency, frameCount: results.length,
	prgSha256: createHash('sha256').update(readFileSync(`${dir}/HD/SCUMMVM/SCUMMVM.PRG`)).digest('hex'),
	fps: +(frequency / mean.wall).toFixed(3), mean, frames: results, hotspots: top,
	note: 'Instruction cycles partitioned at engine/renderer markers. Direct raster/VBL instruction cycles removed from each foreground category; TOS and exception-entry/return overhead outside that address range remain in foreground costs. Audio mixer is compiled out. End-to-end FPS uses CycleCounter timestamps, including raster and waits.',
};
writeFileSync(`${dir}/summary.json`, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify({ frameCount: report.frameCount, fps: report.fps, mean, frames: results, hotspots: top }, null, 2));
