// Minimal surface/rect adapters for compiling the unmodified scene renderer.
// This is not an engine benchmark. The Atari build uses the real c2p assembly;
// the host build uses an independent scalar planar encoder with bounds checks.
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
using byte = uint8_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint = unsigned int;
// Just enough of ScummVM's file and logging API to compile
// AtariSteSceneRenderer::loadMix(); the tests do not load tables.
inline void warning(const char *, ...) {}
inline void debug(const char *, ...) {}
namespace Common {
struct String {
	std::string value;
	const char *c_str() const { return value.c_str(); }
};
struct Path {
	static const char kNativeSeparator = '/';
	std::string value;
	String toString(char) const { return String{value}; }
};
struct SeekableReadStream {
	int64 size() const { return 0; }
	uint16 readUint16BE() { return 0; }
	uint32 read(void *, uint32) { return 0; }
	bool err() const { return true; }
};
struct FSNode {
	explicit FSNode(const Path &) {}
	SeekableReadStream *createReadStream() const { return nullptr; }
};
template<class T> class ScopedPtr {
	T *_pointer;
public:
	explicit ScopedPtr(T *pointer) : _pointer(pointer) {}
	~ScopedPtr() { delete _pointer; }
	ScopedPtr(const ScopedPtr &) = delete;
	ScopedPtr &operator=(const ScopedPtr &) = delete;
	T *operator->() const { return _pointer; }
	bool operator!() const { return !_pointer; }
};
}
template<class T> T MIN(T a, T b) { return std::min(a, b); }
template<class T> T MAX(T a, T b) { return std::max(a, b); }
template<class T> T CLIP(T a, T lo, T hi) { return MIN(MAX(a, lo), hi); }
struct _RGB { byte reserved, red, green, blue; };
namespace Common {
struct Rect {
	int left, top, right, bottom;
	Rect(int w, int h) : left(0), top(0), right(w), bottom(h) {}
	Rect(int l, int t, int r, int b) : left(l), top(t), right(r), bottom(b) {}
	int width() const { return right - left; }
	bool isEmpty() const { return right <= left || bottom <= top; }
	void clip(Rect r) {
		left = MAX(left, r.left); top = MAX(top, r.top);
		right = MIN(right, r.right); bottom = MIN(bottom, r.bottom);
	}
	bool operator==(const Rect &r) const {
		return left == r.left && top == r.top && right == r.right && bottom == r.bottom;
	}
};
}
namespace std {
template<> struct hash<Common::Rect> {
	size_t operator()(const Common::Rect &r) const {
		return 31 * (31 * (31 * r.left + r.top) + r.right) + r.bottom;
	}
};
}
struct Screen { using DirtyRects = std::unordered_set<Common::Rect>; };
namespace Graphics {
struct Surface {
	int w, h, pitch;
	std::vector<byte> pixels;
	Surface(int width, int height, int padding = 0)
		: w(width), h(height), pitch(width + padding), pixels(pitch * h) {}
	const void *getBasePtr(int x, int y) const {
		assert(x >= 0 && x < w && y >= 0 && y < h);
		return pixels.data() + y * pitch + x;
	}
};
}
#ifdef __m68k__
extern "C" void asm_c2p1x1_4(const byte *, const byte *, byte *);
extern "C" void asm_c2p1x1_4_rect(const byte *, const byte *, uint32, uint32, byte *, uint32);
#endif
class AtariSurface {
public:
	int w = 320, h = 200, pitch = 160;
	std::vector<byte> pixels = std::vector<byte>(32000);
	uint32 convertedPixels = 0;
	void *getPixels() { return pixels.data(); }
	void copyRectToSurface(const void *buffer, int srcPitch, int x, int y, int width, int height) {
		assert(x >= 0 && y >= 0 && x + width <= w && y + height <= h);
		assert(width > 0 && width % 16 == 0 && x % 16 == 0);
		convertedPixels += width * height;
		const byte *src = static_cast<const byte *>(buffer);
#ifdef __m68k__
		const byte *end = src + (height - 1) * srcPitch + width;
		if (srcPitch == width && srcPitch / 2 == pitch)
			asm_c2p1x1_4(src, end, pixels.data() + y * pitch + x / 2);
		else
			asm_c2p1x1_4_rect(src, end, width, srcPitch, pixels.data() + y * pitch + x / 2, pitch);
#else
		for (int row = 0; row < height; ++row)
			for (int col = 0; col < width; ++col)
				for (int p = 0; p < 4; ++p) {
					int xx = x + col;
					int o = (y + row) * pitch + (xx / 16) * 8 + p * 2 + ((xx % 16) / 8);
					byte bit = 0x80 >> (xx % 8);
					pixels[o] = (pixels[o] & ~bit) | ((src[row * srcPitch + col] & (1 << p)) ? bit : 0);
				}
#endif
	}
};
