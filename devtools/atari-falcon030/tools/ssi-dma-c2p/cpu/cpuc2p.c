/*
 * The 68030 c2p the ScummVM Atari backend uses (Mikael Kalms' routines in
 * backends/graphics/atari/atari-c2p-asm.S), timed on the screen the DSP
 * c2p test streams: 320x200, 8 bits, the same pseudo-random pixels. This is
 * the figure the DSP route is to be compared with.
 *
 * Each case runs repeatedly for at least MIN_TICKS of the TOS 200 Hz tick,
 * with the system's interrupts running as they do under ScummVM, and
 * reports milliseconds per call:
 *
 *   c2p full    asm_c2p1x1_8, the whole screen, checked against a direct
 *               c2p first
 *   c2p rect    asm_c2p1x1_8_rect on a rectangle at the screen's origin,
 *               the path dirty rectangles take, at several sizes
 *   copy        a 64,000-byte copy in longword moves, the floor of
 *               reading and writing a screen's worth of ST-RAM
 *
 * All buffers are in ST-RAM, as on a stock Falcon; the planar buffer is not
 * the displayed screen. Results go to the console and CPUC2P.TXT.
 */
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char byte;
typedef unsigned int uint32;

extern void asm_c2p1x1_8(const byte *pChunky, const byte *pChunkyEnd, byte *pScreen);
extern void asm_c2p1x1_8_rect(const byte *pChunky, const byte *pChunkyEnd, uint32 chunkyWidth,
                              uint32 chunkyPitch, byte *pScreen, uint32 screenPitch);

#define WIDTH 320
#define HEIGHT 200
#define SCREEN_BYTES (WIDTH * HEIGHT)
#define MIN_TICKS 400 /* 2 s */

static FILE *report;

static long read_hz200(void) {
	return *(volatile long *)0x4ba;
}

static long ticks(void) {
	return Supexec(read_hz200);
}

static void line(const char *text) {
	fputs(text, stdout);
	fputs("\r\n", stdout);
	if (report) {
		fputs(text, report);
		fputs("\r\n", report);
	}
}

static void fill_screen(byte *chunky) {
	unsigned long state = 0x12345678UL;
	for (int i = 0; i < SCREEN_BYTES; i++) {
		state = state * 1103515245UL + 12345UL;
		chunky[i] = (byte)(state >> 24);
	}
}

/* Falcon 8-plane words: per 16 pixels, planes 0..7, bit 15 the leftmost. */
static void reference(const byte *chunky, unsigned short *planar) {
	for (int group = 0; group < SCREEN_BYTES / 16; group++) {
		const byte *pixels = chunky + 16 * group;
		for (int k = 0; k < 8; k++) {
			unsigned short word = 0;
			for (int i = 0; i < 16; i++)
				word = (unsigned short)((word << 1) | ((pixels[i] >> k) & 1));
			*planar++ = word;
		}
	}
}

static void copy_screen(const byte *from, byte *to) {
	const unsigned long *s = (const unsigned long *)from;
	unsigned long *d = (unsigned long *)to;
	for (int n = SCREEN_BYTES / 16; n > 0; n--) {
		d[0] = s[0];
		d[1] = s[1];
		d[2] = s[2];
		d[3] = s[3];
		d += 4;
		s += 4;
	}
}

static const byte *g_chunky;
static byte *g_planar;
static int g_width, g_height;

static void run_full(void) {
	asm_c2p1x1_8(g_chunky, g_chunky + SCREEN_BYTES, g_planar);
}

static void run_rect(void) {
	asm_c2p1x1_8_rect(g_chunky, g_chunky + (g_height - 1) * WIDTH + g_width, g_width, WIDTH, g_planar, WIDTH);
}

static void run_copy(void) {
	copy_screen(g_chunky, g_planar);
}

/* Microseconds per call. */
static long time_case(void (*body)(void)) {
	long calls = 0, start, elapsed;
	body(); /* warm the caches */
	start = ticks();
	while (ticks() == start)
		;
	start = ticks();
	do {
		body();
		calls++;
		elapsed = ticks() - start;
	} while (elapsed < MIN_TICKS);
	return (long)((elapsed * 5000LL + calls / 2) / calls);
}

static void report_time(const char *name, long us, long pixels) {
	char text[256];
	long per_pixel_ns = (long)((us * 1000LL + pixels / 2) / pixels);
	sprintf(text, "%-22s %4ld.%03ld ms  (%ld ns a pixel, %ld pixels)", name, us / 1000, us % 1000,
	        per_pixel_ns, pixels);
	line(text);
}

int main(void) {
	static const int rects[][2] = {{320, 200}, {160, 100}, {64, 64}, {32, 32}};
	char text[160];
	byte *chunky = (byte *)Mxalloc(SCREEN_BYTES + 16, 0);
	byte *planar = (byte *)Mxalloc(SCREEN_BYTES + 16, 0);
	unsigned short *want = (unsigned short *)malloc(SCREEN_BYTES);
	int failures = 0;

	report = fopen("CPUC2P.TXT", "wb");
	line("68030 c2p (ScummVM backend, Kalms), 320x200x8");
	if (!chunky || !planar || !want) {
		line("RESULT: FAIL (no memory)");
		return 1;
	}
	chunky = (byte *)(((unsigned long)chunky + 15) & ~15UL);
	planar = (byte *)(((unsigned long)planar + 15) & ~15UL);
	g_chunky = chunky;
	g_planar = planar;

	fill_screen(chunky);
	reference(chunky, want);
	memset(planar, 0, SCREEN_BYTES);
	run_full();
	if (memcmp(planar, want, SCREEN_BYTES) != 0) {
		line("c2p full: output differs from the reference  FAIL");
		failures++;
	} else {
		line("c2p full: output matches the reference");
	}

	report_time("c2p full 320x200", time_case(run_full), SCREEN_BYTES);
	for (unsigned r = 0; r < sizeof(rects) / sizeof(rects[0]); r++) {
		g_width = rects[r][0];
		g_height = rects[r][1];
		sprintf(text, "c2p rect %dx%d", g_width, g_height);
		report_time(text, time_case(run_rect), (long)g_width * g_height);
	}
	report_time("copy 64000 bytes", time_case(run_copy), SCREEN_BYTES);

	line(failures ? "RESULT: FAIL" : "RESULT: PASS");
	if (report)
		fclose(report);
	return failures;
}
