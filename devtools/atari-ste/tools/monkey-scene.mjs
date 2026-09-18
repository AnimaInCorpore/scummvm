import { describeCostume, loadMonkeyCostume, renderCostumeFrame } from './monkey-costume.mjs';

export const MONKEY_BAR_CAMERA_X = 160;
export const MONKEY_BAR_SCENE_SPRITES = [
	{
		name: 'hanging-costume-24',
		costumeId: 24,
		worldX: 393,
		worldY: 88,
		drawOrder: 10,
	},
	{
		name: 'guybrush-costume-26',
		costumeId: 26,
		worldX: 293,
		worldY: 132,
		drawOrder: 20,
	},
	{
		name: 'pirate-costume-37',
		costumeId: 37,
		worldX: 440,
		worldY: 132,
		drawOrder: 30,
	},
];

function roomColorToRgb12(roomPalette, index) {
	if (index === 255) return 0;
	const offset = index * 3;
	if (offset + 2 >= roomPalette.length) return 0;
	const quantize = value => Math.max(0, Math.min(15, Math.round(value / 17)));
	return (quantize(roomPalette[offset]) << 8) | (quantize(roomPalette[offset + 1]) << 4) | quantize(roomPalette[offset + 2]);
}

function selectAnimation(costume) {
	const animations = describeCostume(costume);
	return animations.find(animation => animation.steps > 1) ?? animations[0];
}

export function loadMonkeyBarScene(dataDir, definitions = MONKEY_BAR_SCENE_SPRITES) {
	return definitions.map(definition => {
		const { costume } = loadMonkeyCostume(definition.costumeId, dataDir);
		const animation = selectAnimation(costume);
		if (!animation) throw new Error(`Costume ${definition.costumeId} has no decodable animation`);
		return { ...definition, costume, animation };
	});
}

function copyRoomViewport(source, cameraX, width, height) {
	const scene = new Uint16Array(width * height);
	for (let y = 0; y < height; y++) {
		const sourceOffset = y * 640 + cameraX;
		scene.set(source.subarray(sourceOffset, sourceOffset + width), y * width);
	}
	return scene;
}

function compositeSprite(scene, sprite, frame, roomPalette, cameraX, width, height) {
	const poseStep = frame % sprite.animation.steps;
	const pose = renderCostumeFrame(
		sprite.costume,
		sprite.animation.animation,
		poseStep,
		sprite.worldX,
		sprite.worldY,
		Boolean(sprite.mirrored),
	);
	for (const pixel of pose.pixels) {
		const x = pixel.x - cameraX;
		const y = pixel.y;
		if (x < 0 || x >= width || y < 0 || y >= height) continue;
		const globalPaletteIndex = pose.palette[pixel.color] ?? 255;
		scene[y * width + x] = roomColorToRgb12(roomPalette, globalPaletteIndex);
	}
}

export function renderMonkeyBarSceneFrame(source, frame, sprites, roomPalette, {
	cameraX = MONKEY_BAR_CAMERA_X,
	width = 320,
	height = 144,
} = {}) {
	const scene = copyRoomViewport(source, cameraX, width, height);
	const orderedSprites = [...sprites].sort((a, b) => a.drawOrder - b.drawOrder);
	for (const sprite of orderedSprites) compositeSprite(scene, sprite, frame, roomPalette, cameraX, width, height);
	return { scene, frame, cameraX, sprites: orderedSprites.map(sprite => ({
		name: sprite.name,
		costumeId: sprite.costumeId,
		animation: sprite.animation.animation,
		steps: sprite.animation.steps,
		worldX: sprite.worldX,
		worldY: sprite.worldY,
	})) };
}
