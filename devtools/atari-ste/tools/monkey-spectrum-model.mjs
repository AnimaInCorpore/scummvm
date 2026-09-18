import { getSpectrum512ColorSlotIndex } from './spectrum512-slots.mjs';

// A 32-word candidate keeps one of the two visible reloads plus the border
// reload that initializes the following line. Each register still switches
// at its own x position; there is no simultaneous 16-register zone boundary.
// Removing the other reload requires a new timed raster on the STE. These
// functions model fidelity only, not recovered CPU time.
export function spectrum32EarlySlot(x, register) {
	return Math.min(getSpectrum512ColorSlotIndex(x, register), register + 16);
}

export function spectrum32LateSlot(x, register) {
	return getSpectrum512ColorSlotIndex(x, register) >= 32 ? register + 16 : register;
}
