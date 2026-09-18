// Runs the staged STE ScummVM build in Hatari and, at the requested VBL
// counts, captures a screenshot together with the screen and packed palette
// schedule the palette raster used for that frame (see
// backends/graphics/atari/atari-ste-raster.h) plus the
// raster's counters. scumm-ste-verify.mjs checks such a capture.
//
//   node devtools/atari-ste/tools/scumm-ste-capture.mjs OUTDIR [VBL ...]
//
// Environment: HD (GEMDOS drive directory), RUN_VBLS (default: last VBL + 60),
// LINES (raster lines, default 144), HATARI, TOS, EXTRA (extra Hatari args).
import { mkdirSync, writeFileSync, appendFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn, execFileSync } from 'node:child_process';

const output = resolve(process.argv[2] || 'scumm-ste-capture');
const vbls = process.argv.slice(3).map(Number).filter(v => v > 0);
if (!vbls.length) vbls.push(4000);
const lines = Number(process.env.LINES || 144);
const scheduleBytes = 2 * (32 + 48 * (lines - 2));
const repo = fileURLToPath(new URL('../../..', import.meta.url));
const runVbls = Number(process.env.RUN_VBLS || Math.max(...vbls) + 60);
const hd = resolve(process.env.HD || `${repo}build-ste-scumm-static4/HD`);
const hatari = process.env.HATARI || `${repo}../F030Arcade/third_party/hatari/build/src/hatari`;
const tos = process.env.TOS || `${repo}../F030Arcade/third_party/tos/tos206de.img`;
const extra = (process.env.EXTRA || '').split(/\s+/).filter(Boolean);

mkdirSync(output, { recursive: true });

// The PRG's fixed symbol offsets (ELF link base 0). Hatari loads the program
// at its TEXT base, so a symbol's run-time address is TEXT + offset; that is
// used directly in expressions, because Hatari's ASCII symbol table does not
// resolve names inside evaluate/savebin here.
const prg = resolve(hd, 'SCUMMVM/SCUMMVM.PRG');
const nm = process.env.NM || `${repo}../cross-mint/bin/m68k-atari-mintelf-nm`;
const addrOf = {};
for (const line of execFileSync(nm, [prg], { encoding: 'utf8' }).split('\n')) {
	const m = line.match(/^([0-9a-f]{8}) [A-Za-z] (\S+)$/);
	if (m) addrOf[m[2]] = parseInt(m[1], 16);
}
const at = name => {
	if (!(name in addrOf)) throw Error(`symbol ${name} not found in ${prg}`);
	return `(TEXT+0x${addrOf[name].toString(16)})`;
};
const write = (name, text) => writeFileSync(`${output}/${name}.ini`, text + '\n');
const counters = ['atari_ste_vbl_count', 'atari_ste_raster_frames', 'atari_ste_raster_resynced',
	'atari_ste_raster_skipped', 'atari_ste_raster_lost_ticks', 'atari_ste_raster_last_screen',
	'atari_ste_raster_last_palette', 'atari_ste_display_palette'];
for (const vbl of vbls) {
	write(`cap-${vbl}`, [
		`echo CAPTURE ${vbl}`,
		`screenshot ${output}/shot-${vbl}.png`,
		// at() already reads the long at &pointer, i.e. the screen/schedule
		// address itself, which is what savebin wants as its source address.
		`savebin ${output}/screen-${vbl}.bin '${at('atari_ste_raster_last_screen')}' #32000`,
		`savebin ${output}/palette-${vbl}.bin '${at('atari_ste_raster_last_palette')}' #${scheduleBytes}`,
		...counters.map(c => `echo ${c}\nevaluate ${at(c)}`),
		vbl === Math.max(...vbls) ? 'quit' : '',
	].join('\n'));
}
write('boot', vbls.map(vbl => `b VBL = ${vbl} :once :file ${output}/cap-${vbl}.ini`).join('\n'));

const args = ['--tos', tos, '--harddrive', hd, '--machine', 'ste', '--memsize', '4',
	'--cpulevel', '0', '--cpuclock', '8', '--cpu-exact', 'on', '--compatible', 'on',
	'--sound', 'off', '--fast-boot', 'on', '--confirm-quit', 'off', '--fast-forward', 'on',
	'--frameskips', '0', '--spec512', '1', '--borders', 'off', '--statusbar', 'off', '--drive-led', 'off', '--zoom', '1',
	'--run-vbls', String(runVbls), '--conout', '2', '--parse', `${output}/boot.ini`,
	...extra, '--auto', 'C:\\SCUMMVM\\SCUMMVM.PRG'];
console.log(`Hatari: ${vbls.length} capture(s) at VBL ${vbls.join(', ')}, running ${runVbls} VBLs`);
const child = spawn(hatari, args, {
	// The debugger must see EOF after scripted quit, not wait on an unused pipe.
	stdio: ['ignore', 'pipe', 'pipe'],
	env: { ...process.env, SDL_VIDEODRIVER: 'dummy', SDL_AUDIODRIVER: 'dummy', SDL_RENDER_DRIVER: 'software' },
});
// The log is streamed to disk so a stalled run can be inspected while it runs.
let log = '';
writeFileSync(`${output}/hatari.log`, '');
for (const stream of [child.stdout, child.stderr]) stream.on('data', data => { log += data; appendFileSync(`${output}/hatari.log`, data); });
child.on('close', code => {
	const interesting = log.split('\n').filter(l => /STE raster|CAPTURE|^atari_ste_|^= |Bus Error|Address Error|heap at|resetScummVars/.test(l));
	console.log(interesting.join('\n'));
	console.log(`Hatari exited with ${code}; log in ${output}/hatari.log`);
	if (code) process.exitCode = code;
});
