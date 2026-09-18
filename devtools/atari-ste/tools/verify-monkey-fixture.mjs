import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { sha256 } from './monkey-resource.mjs';

function parseArgs(argv) {

	const fixture = process.env.MONKEY_FIXTURE ?? 'devtools/atari-ste/monkey-bar/fixture';
	const comparison = process.env.MONKEY_COMPARISON ?? 'devtools/atari-ste/monkey-bar/comparison';
	let fixtureDir = fixture;
	let comparisonDir = comparison;
	for (let i = 2; i < argv.length; i++) {
		if (argv[i] === '--fixture') fixtureDir = argv[++i];
		else if (argv[i] === '--comparison') comparisonDir = argv[++i];
		else if (argv[i] === '--help') {
			console.log('Usage: node devtools/atari-ste/tools/verify-monkey-fixture.mjs [--fixture DIR] [--comparison DIR]');
			process.exit(0);
		} else throw new Error(`Unknown argument ${argv[i]}`);
	}
	return { fixtureDir: resolve(fixtureDir), comparisonDir: resolve(comparisonDir) };
}

function assert(condition, message) {

	if (!condition) throw new Error(message);
}

function readJson(path) {

	return JSON.parse(readFileSync(path, 'utf8'));
}

function verifyFixture(fixtureDir) {

	const manifest = readJson(`${fixtureDir}/room-manifest.json`);
	assert(manifest.schema === 'spectrum512-monkey-bar-fixture/1', 'Unexpected fixture manifest schema');
	assert(manifest.fixture.roomId === 28, `Expected room 28, got ${manifest.fixture.roomId}`);
	assert(manifest.fixture.width === 640 && manifest.fixture.height === 144, 'Unexpected room dimensions');
	assert(manifest.fixture.objectRecords === 52, `Expected 52 object records, got ${manifest.fixture.objectRecords}`);
	assert(manifest.palette.block.entries === 256, 'Expected a 256-entry CLUT');
	assert(manifest.palette.cycleBlock?.payload.join(',') === '0,0', 'Unexpected room CYCL payload');

	const index = readFileSync(`${fixtureDir}/${manifest.background.indexFile}`);
	const palette = readFileSync(`${fixtureDir}/${manifest.palette.file}`);
	assert(index.length === 640 * 144, `Unexpected background index bytes: ${index.length}`);
	assert(palette.length === 256 * 3, `Unexpected palette bytes: ${palette.length}`);
	assert(sha256(index) === manifest.background.sha256, 'Background index hash does not match manifest');
	assert(sha256(palette) === manifest.palette.sha256, 'Palette hash does not match manifest');
	const stripInfo = readJson(`${fixtureDir}/room-strips.json`);
	assert(stripInfo.length === 80, `Expected 80 background strips, got ${stripInfo.length}`);
	for (let i = 1; i < stripInfo.length; i++) assert(stripInfo[i].offset > stripInfo[i - 1].offset, `Strip offsets are not increasing at ${i}`);

	for (const object of manifest.objectBitmaps) {
		const data = readFileSync(`${fixtureDir}/${object.indexFile}`);
		assert(data.length === object.width * object.height, `Object ${object.objectId} index size mismatch`);
		assert(sha256(data) === object.sha256, `Object ${object.objectId} hash mismatch`);
	}
	return { room: manifest.fixture.roomId, backgroundBytes: index.length, objects: manifest.objectBitmaps.length };
}

function verifyComparison(comparisonDir) {

	const manifest = readJson(`${comparisonDir}/comparison-manifest.json`);
	assert(manifest.schema === 'spectrum512-monkey-bar-comparison/1', 'Unexpected comparison manifest schema');
	assert(manifest.input.dither === 'none', 'Default comparison must be the undithered baseline');
	assert(manifest.candidates.length === 2, 'Expected shared-line and joint Spectrum candidates');
	for (const candidate of manifest.candidates) {
		assert(candidate.screenBytes === 23040, `${candidate.name} screen size mismatch`);
		assert(candidate.preparedBytes === candidate.screenBytes + candidate.paletteBytes, `${candidate.name} prepared size mismatch`);
		assert(Number.isFinite(candidate.totalError), `${candidate.name} has no quality metric`);
	}
	return { camera: manifest.fixture.cameraOffset, candidates: manifest.candidates.map(candidate => ({ name: candidate.name, error: candidate.totalError, bytes: candidate.preparedBytes })) };
}

const options = parseArgs(process.argv);
console.log(JSON.stringify({ fixture: verifyFixture(options.fixtureDir), comparison: verifyComparison(options.comparisonDir) }, null, 2));
