import assert from 'node:assert/strict';
import { convertLine } from './reference.mjs';
import { spectrum32EarlySlot, spectrum32LateSlot } from './monkey-spectrum-model.mjs';

for (const [name, slotAt, delay] of [['early', spectrum32EarlySlot, 0], ['late', spectrum32LateSlot, 160]]) {
	for (let register = 0; register < 16; register++) {
		const transition = 10 * register + (register & 1 ? -5 : 1) + delay;
		assert.equal(slotAt(transition - 1, register), register);
		assert.equal(slotAt(transition, register), register + 16);
	}
	for (const line of [
		Uint16Array.from({ length: 320 }, () => 0xabc),
		Uint16Array.from({ length: 320 }, (_, x) => ((x * 53) ^ (x >> 2)) & 4095),
	]) {
		const result = convertLine(line, { registerCount: 16, slotAt });
		const used = new Set();
		for (let x = 0; x < 320; x++) {
			let register = 0;
			for (let plane = 0; plane < 4; plane++)
				register |= ((result.planar.readUInt16BE((x >> 4) * 8 + plane * 2) >> (15 - (x & 15))) & 1) << plane;
			const slot = slotAt(x, register);
			assert(slot >= 0 && slot < 32);
			used.add(slot);
			const id = result.slots[slot];
			assert.deepEqual(Array.from(result.rgb.subarray(x * 3, x * 3 + 3)),
				[(id >> 8) * 17, ((id >> 4) & 15) * 17, (id & 15) * 17]);
		}
		assert(used.size <= 32);
	}
	console.log(`Spectrum 32 ${name}: register transitions and planar/RGB round-trip PASS`);
}
