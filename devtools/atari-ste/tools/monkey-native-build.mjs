import { execFileSync } from 'node:child_process';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

process.chdir(fileURLToPath(new URL('..', import.meta.url)));
const vasm = process.env.VASM || resolve('../../../F030Arcade/third_party/vasm/vasmm68k_mot');
execFileSync(vasm, [
	'monkey-bar/monkey-viewer.s',
	'-m68000',
	'-Ftos',
	'-L', 'monkey-bar/monkey-viewer.lst',
	'-o', 'monkey-bar/MONKEY.PRG',
], { stdio: 'inherit' });
console.log('Built devtools/atari-ste/monkey-bar/MONKEY.PRG');
