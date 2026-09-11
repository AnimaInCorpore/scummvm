// Shared palette-selection helpers for the STE fidelity tools.
//
// choosePalette reduces a colour demand (a list of RGB12 ids, repeats acting as
// weight) to n representative colours by count-weighted closest-pair merging,
// the same idea the Spectrum slot merger uses. nearest maps one colour onto the
// closest entry of a palette under the fixed-point Oklab metric.
import { distance } from './reference.mjs';

const centroid = c => {
	const r = Math.round(c.sr / c.count), g = Math.round(c.sg / c.count), b = Math.round(c.sb / c.count);
	return (r << 8) | (g << 4) | b;
};

export function choosePalette(ids, n) {
	const counts = new Map();
	for (const id of ids) counts.set(id, (counts.get(id) || 0) + 1);
	const clusters = [...counts].map(([id, c]) => ({
		id, count: c, sr: (id >> 8) * c, sg: ((id >> 4) & 15) * c, sb: (id & 15) * c,
	}));
	if (clusters.length <= n)
		return clusters.map(c => c.id);
	while (clusters.length > n) {
		let best = Infinity, bi = 0, bj = 1;
		for (let i = 0; i < clusters.length; i++) {
			for (let j = i + 1; j < clusters.length; j++) {
				const d = distance(centroid(clusters[i]), centroid(clusters[j]))
					* (clusters[i].count * clusters[j].count) / (clusters[i].count + clusters[j].count);
				if (d < best) { best = d; bi = i; bj = j; }
			}
		}
		const a = clusters[bi], b = clusters[bj];
		a.count += b.count; a.sr += b.sr; a.sg += b.sg; a.sb += b.sb;
		a.id = centroid(a);
		clusters.splice(bj, 1);
	}
	return clusters.map(c => c.id);
}

export function nearest(id, palette) {
	let best = Infinity, bestId = palette[0];
	for (const p of palette) {
		const d = distance(id, p);
		if (d < best) { best = d; bestId = p; }
	}
	return bestId;
}
