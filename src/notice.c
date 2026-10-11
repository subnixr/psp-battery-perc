/*
 * Low battery notice: an empty battery icon in red. It has to show over the
 * XMB and over any game, so it is not tied to a screen: the
 * sceDisplaySetFrameBuf syscall is hooked and the icon is blended into each
 * frame just before it is displayed.
 * Some games hand the frame over while the GE is still drawing into it, or
 * do not hand one over every frame, so a thread also blends the icon into
 * the frame on screen at every vblank, and again until scan-out has passed
 * it. Some keep drawing into the frame on screen; there the icon is also put
 * back whenever the game has waited for the GE to finish, through a hook on
 * sceGeDrawSync.
 * The HOME screen is not drawn into the game's frame: impose puts a buffer of
 * its own on screen, above it, and the thread blends the icon into that one
 * for as long as it is there.
 * The icon is a coverage mask made by tools/gen_notice.py from the XMB
 * battery icon, and goes where the XMB draws it. When the stock icon is found
 * under it, its white is taken out as the red goes in, so the stock icon
 * turns red pixel by pixel. The HOME screen's icon is drawn a little
 * differently and has a mask of its own.
 * Impose redraws its icon as it goes, and a repaint after the fact can lose
 * to scan-out (it does in pops), so there the icon is made red at the
 * source: impose's blits go through notice_home_sprite, which hands back a
 * tinted copy of the battery sprite. The thread stays out of the HOME screen
 * once that has happened.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspsdk.h>
#include <pspinit.h>
#include <systemctrl.h>
#include <string.h>
#include "common.h"
#include "notice_glyph.h"

/* added to where the mask goes, in pops */
#define POPS_DX	0
#define POPS_DY	0

/* impose's battery sprite, ABGR1555 */
#define HOME_SPRITE_W	28
#define HOME_SPRITE_H	17

/*
 * Where a stock icon that is not quite where the mask goes is looked for: the
 * HOME screen's sprite and a margin, short of the clock. It is one if this
 * many pixels are white and they span about what its outline does. Whiteness
 * from the floor up turns red, but not in the middle, where its charge bars
 * are: that starts and ends this far from the right, top and bottom of the
 * outline.
 */
#define TINT_X		446
#define TINT_Y		2
#define TINT_W		34
#define TINT_H		22
#define TINT_MIN	32
#define TINT_SPAN_W	22
#define TINT_SPAN_H	12
#define TINT_SPAN_SLACK	4
#define TINT_FLOOR	0x60
#define TINT_BARS_R0	17
#define TINT_BARS_R1	2
#define TINT_BARS_Y	2

/* icon colour, #BF0000 */
#define NOTICE_R	0xBF
#define NOTICE_G	0x00
#define NOTICE_B	0x00

/* the stock icon is there if the pixels it covers this much are this white */
#define STOCK_ALPHA	0xE0
#define STOCK_WHITE	0xC0

#define SCREEN_H	272
#define NID_SET_FRAME_BUF	0x289D82FE
#define NID_GET_FRAME_BUF	0xEEDA2E54
#define NID_WAIT_VBLANK_START	0x984C27E7
#define NID_GET_HCOUNT	0x773DD3A3
/* sceGe_user */
#define NID_GE_DRAW_SYNC	0xB287BD61
/* sceDisplay_driver, sceDisplayGetFrameBufferInternal: on 6.61, and before */
#define NID_GET_FRAME_BUF_INT	0xF338AAE0
#define NID_GET_FRAME_BUF_INT_OLD	0x5B5AEFAD

#define HOME_PRI	0	/* the level impose sets its buffer at */

/*
 * Passes of the vblank thread after the one at vblank start, and the time
 * between them: with the scan line to go by, until it is past the icon, about
 * 2 ms; without it, for the 1 ms that takes it to the first line.
 */
#define LATE_PASSES	10
#define LATE_DELAY	300
#define BLIND_PASSES	2
#define BLIND_DELAY	500

#define DEBUG_STATS_SECS	300

/* Channel layout of the four display pixel formats; red is always at bit 0. */
static const struct {
	u8 rbits, gshift, gbits, bshift, bbits;
	u32 opaque;
} formats[] = {
	{ 5, 5, 6, 11, 5, 0 },		/* 565 */
	{ 5, 5, 5, 10, 5, 0x8000 },	/* 5551 */
	{ 4, 4, 4, 8, 4, 0xF000 },	/* 4444 */
	{ 8, 8, 8, 16, 8, 0xFF000000 },	/* 8888 */
};

static volatile int notice_visible;
/* impose has been handed a tinted sprite: it draws the HOME screen's icon itself */
static volatile int home_hooked;
static int (*set_frame_buf)(void *top, int width, int format, int sync);
static int (*get_frame_buf)(void **top, int *width, int *format, int sync);
static int (*wait_vblank_start)(void);
static int (*get_hcount)(void);
static int (*ge_draw_sync)(int mode);
static int (*get_frame_buf_internal)(int pri, void **top, int *width, int *format, int sync);

/*
 * What draw last wrote, per mask pixel. A pixel that still holds it already
 * has the icon and is left alone, so drawing the same frame twice is safe.
 * The game's frames and the HOME screen buffer each have their own, and
 * their own mask and place for it. With tint set, a stock icon near that
 * place has its outline turned red as it is and the mask is left out: the
 * HOME screen's is not at the same x everywhere, and pops puts that screen in
 * the game's frame.
 */
struct drawn {
	u32 pixels[NOTICE_W * NOTICE_H];
	int format;
	const u8 *alpha;
	int x, y;
	int tint;
};

static struct drawn drawn_frame = {
	.format = -1, .alpha = notice_alpha, .x = NOTICE_X, .y = NOTICE_Y,
};
static struct drawn drawn_home = {
	.format = -1, .alpha = home_alpha, .x = NOTICE_X, .y = NOTICE_Y, .tint = 1,
};

#ifdef DEBUG
/* What the hook last saw, logged from notice_tick. */
static volatile u32 hook_calls, hook_draws, hook_top;
static volatile int hook_width, hook_format;
/* Passes of the vblank thread, and pixels it found without the icon. */
static volatile u32 thread_draws, thread_pixels;
/* Passes that went to the HOME screen buffer. */
static volatile u32 home_draws;
/* Battery sprites tinted for impose. */
static volatile u32 home_tints;
/* Passes that found a stock icon to tint, and pixels they turned red. */
static volatile u32 tint_draws, tint_pixels;
/* Pixels the later passes of the vblank thread found without the icon. */
static volatile u32 late_pixels;
/* Waits for the GE the hook saw, and pixels it found without the icon. */
static volatile u32 ge_calls, ge_pixels;
static volatile int hook_sync;
#endif

/*
 * One channel of blend: take weight a of what is there out of v and put as
 * much of the icon colour in. Over the stock icon what is there is white.
 */
static u32 mix(u32 v, u32 max, u32 colour, int a, int stock)
{
	u32 out = ((stock ? max : v) * a) >> 8;

	return (v > out ? v - out : 0) + (((colour * max / 255) * a) >> 8);
}

/* Blend the icon colour over pixel c with weight a (0-256). */
static u32 blend(u32 c, int a, int stock, int format)
{
	u32 rmax = (1 << formats[format].rbits) - 1;
	u32 gmax = (1 << formats[format].gbits) - 1;
	u32 bmax = (1 << formats[format].bbits) - 1;
	u32 r = mix(c & rmax, rmax, NOTICE_R, a, stock);
	u32 g = mix((c >> formats[format].gshift) & gmax, gmax, NOTICE_G, a, stock);
	u32 b = mix((c >> formats[format].bshift) & bmax, bmax, NOTICE_B, a, stock);

	return formats[format].opaque | b << formats[format].bshift |
	       g << formats[format].gshift | r;
}

static int is_white(u32 c, int format)
{
	u32 rmax = (1 << formats[format].rbits) - 1;
	u32 gmax = (1 << formats[format].gbits) - 1;
	u32 bmax = (1 << formats[format].bbits) - 1;

	return (c & rmax) >= STOCK_WHITE * rmax / 255 &&
	       ((c >> formats[format].gshift) & gmax) >= STOCK_WHITE * gmax / 255 &&
	       ((c >> formats[format].bshift) & bmax) >= STOCK_WHITE * bmax / 255;
}

/* Whether the frame has the stock icon where ours goes. */
static int stock_under(void *top, int width, int format, const struct drawn *drawn)
{
	const u8 *src = drawn->alpha;
	int x, y;
	u32 c;

	for (y = drawn->y; y < drawn->y + NOTICE_H; y++) {
		for (x = drawn->x; x < drawn->x + NOTICE_W; x++) {
			if (*src++ < STOCK_ALPHA)
				continue;
			if (format == PSP_DISPLAY_PIXEL_FORMAT_8888)
				c = ((u32 *)top)[y * width + x];
			else
				c = ((u16 *)top)[y * width + x];
			if (!is_white(c, format))
				return 0;
		}
	}

	return 1;
}

static int fits(void *top, int width, int format, const struct drawn *drawn)
{
	return top && format >= 0 && format <= 3 &&
	       drawn->x >= 0 && drawn->x + NOTICE_W <= width &&
	       drawn->y >= 0 && drawn->y + NOTICE_H <= SCREEN_H;
}

/* top comes from user code: never follow it into kernel memory */
static int drawable(void *top, int width, int format)
{
	return fits(top, width, format, &drawn_frame) && !((u32)top & 0x80000000);
}

/* How white pixel c is, 0-255: its darkest channel. */
static int whiteness(u32 c, int format)
{
	u32 rmax = (1 << formats[format].rbits) - 1;
	u32 gmax = (1 << formats[format].gbits) - 1;
	u32 bmax = (1 << formats[format].bbits) - 1;
	u32 r = (c & rmax) * 255 / rmax;
	u32 g = ((c >> formats[format].gshift) & gmax) * 255 / gmax;
	u32 b = ((c >> formats[format].bshift) & bmax) * 255 / bmax;

	return r < g ? (r < b ? r : b) : (g < b ? g : b);
}

static u32 get_pixel(void *top, int i, int format)
{
	return format == PSP_DISPLAY_PIXEL_FORMAT_8888 ? ((u32 *)top)[i] : ((u16 *)top)[i];
}

/*
 * Turns the outline of a stock icon red, white or already red, if there is
 * one in the w x h box at x0,y0. Returns the number of pixels changed, or -1
 * if there is no icon there.
 */
static int tint_icon(void *px, int stride, int format, int x0, int y0, int w, int h)
{
	u32 colours = ~formats[format].opaque &
		      (format == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 0xFFFFFF : 0xFFFF);
	u32 red = blend(~0, 256, 1, format) & colours;
	int l = x0 + w, t = y0 + h, r = -1, b = -1;
	int x, y, a, i, icon = 0, count = 0;
	u32 c;

	for (y = y0; y < y0 + h; y++) {
		for (x = x0; x < x0 + w; x++) {
			c = get_pixel(px, y * stride + x, format);
			if ((c & colours) != red && !is_white(c, format))
				continue;
			icon++;
			if (x < l)
				l = x;
			if (x > r)
				r = x;
			if (y < t)
				t = y;
			if (y > b)
				b = y;
		}
	}
	x = r - l + 1 - TINT_SPAN_W;
	y = b - t + 1 - TINT_SPAN_H;
	if (icon < TINT_MIN || x < -TINT_SPAN_SLACK || x > TINT_SPAN_SLACK ||
	    y < -TINT_SPAN_SLACK || y > TINT_SPAN_SLACK)
		return -1;

	for (y = y0; y < y0 + h; y++) {
		for (x = x0; x < x0 + w; x++) {
			if (x >= r - TINT_BARS_R0 && x <= r - TINT_BARS_R1 &&
			    y >= t + TINT_BARS_Y && y <= b - TINT_BARS_Y)
				continue;
			i = y * stride + x;
			c = get_pixel(px, i, format);
			a = whiteness(c, format);
			if (a <= TINT_FLOOR)
				continue;
			a = a >= STOCK_WHITE ? 256 : (a - TINT_FLOOR) * 256 / (STOCK_WHITE - TINT_FLOOR);
			c = blend(c, a, 1, format);
			if (format == PSP_DISPLAY_PIXEL_FORMAT_8888)
				((u32 *)px)[i] = c;
			else
				((u16 *)px)[i] = c;
			count++;
		}
	}

	return count;
}

/* The same in a frame, around where the mask goes. */
static int tint_stock(void *top, int width, int format)
{
	if (width < TINT_X + TINT_W)
		return -1;

	return tint_icon(top, width, format, TINT_X, TINT_Y, TINT_W, TINT_H);
}

/* Returns the number of pixels blended. */
static int draw(void *top, int width, int format, struct drawn *drawn)
{
	const u8 *src = drawn->alpha;
	u32 *last = drawn->pixels;
	int x, y, a, count = 0;
	int stale, stock;
	u32 intr;

	/*
	 * uncached, so the frame on screen does not wait for a cache flush;
	 * impose's buffer is a kernel address and already is
	 */
	if (!((u32)top & 0x80000000))
		top = (void *)((u32)top | 0x40000000);

	/* the hook and the vblank thread share drawn */
	intr = pspSdkDisableInterrupts();
	if (drawn->tint && (count = tint_stock(top, width, format)) >= 0) {
		/* the mask is not in this frame */
		drawn->format = -1;
		pspSdkEnableInterrupts(intr);
#ifdef DEBUG
		tint_draws++;
		tint_pixels += count;
#endif
		return count;
	}
	count = 0;
	stale = format != drawn->format;
	drawn->format = format;
	/* not in a frame that already has our icon; there every pixel is skipped */
	stock = stock_under(top, width, format, drawn);

	for (y = drawn->y; y < drawn->y + NOTICE_H; y++) {
		for (x = drawn->x; x < drawn->x + NOTICE_W; x++, last++) {
			a = *src++;
			if (!a)
				continue;
			a += a >> 7;
			if (format == PSP_DISPLAY_PIXEL_FORMAT_8888) {
				u32 *p = (u32 *)top + y * width + x;
				if (!stale && *p == *last)
					continue;
				*p = *last = blend(*p, a, stock, format);
			} else {
				u16 *p = (u16 *)top + y * width + x;
				if (!stale && *p == *last)
					continue;
				*p = *last = blend(*p, a, stock, format);
			}
			count++;
		}
	}
	pspSdkEnableInterrupts(intr);

	return count;
}

/*
 * Called for every sprite impose blits. For its battery icon, returns a copy
 * with the outline turned red, found in the sprite itself, so it does not
 * matter where in it the icon is.
 */
const void *notice_home_sprite(int w, int h, const void *src)
{
	static u16 tinted[HOME_SPRITE_W * HOME_SPRITE_H] __attribute__((aligned(16)));

	if (!notice_visible || w != HOME_SPRITE_W || h != HOME_SPRITE_H)
		return src;

	memcpy(tinted, src, sizeof(tinted));
	if (tint_icon(tinted, HOME_SPRITE_W, PSP_DISPLAY_PIXEL_FORMAT_5551,
		      0, 0, HOME_SPRITE_W, HOME_SPRITE_H) < 0)
		return src;
	home_hooked = 1;
#ifdef DEBUG
	home_tints++;
#endif

	return tinted;
}

static int set_frame_buf_hook(void *top, int width, int format, int sync)
{
#ifdef DEBUG
	hook_calls++;
	hook_top = (u32)top;
	hook_width = width;
	hook_format = format;
	hook_sync = sync;
#endif
	if (notice_visible && drawable(top, width, format)) {
		draw(top, width, format, &drawn_frame);
#ifdef DEBUG
		hook_draws++;
#endif
	}

	return set_frame_buf(top, width, format, sync);
}

/* Blends the icon into the game's frame on screen. Returns the number of pixels blended. */
static int draw_on_screen(void)
{
	void *top;
	int width, format, ret;
	/* the GE hook runs on the game's thread, and top is on a kernel stack */
	u32 k1 = pspSdkSetK1(0);

	ret = get_frame_buf(&top, &width, &format, PSP_DISPLAY_SETBUF_IMMEDIATE);
	pspSdkSetK1(k1);
	if (ret < 0 || !drawable(top, width, format))
		return 0;

	return draw(top, width, format, &drawn_frame);
}

/* The game has waited for the GE: what that drew over the icon is all there. */
static int ge_draw_sync_hook(int mode)
{
	int ret = ge_draw_sync(mode);

	if (notice_visible && mode == 0) {
#ifdef DEBUG
		ge_pixels += draw_on_screen();
		ge_calls++;
#else
		draw_on_screen();
#endif
	}

	return ret;
}

/* Whether scan-out is past the icon, after this many passes since vblank start. */
static int scanned(int passes)
{
	int line;

	if (!get_hcount)
		return passes >= BLIND_PASSES;
	line = get_hcount();

	return passes >= LATE_PASSES || (line >= drawn_frame.y + NOTICE_H && line < SCREEN_H);
}

/* Blends the icon into the frame on screen, right after it has been latched. */
static int vblank_thread(SceSize args, void *argp)
{
	void *top;
	int width, format, passes;

	while (1) {
		if (!notice_visible) {
			sceKernelDelayThread(500 * 1000);
			continue;
		}
		if (wait_vblank_start() < 0)
			sceKernelDelayThread(16 * 1000);
		/* the HOME screen hides the game's frame */
		if (get_frame_buf_internal &&
		    get_frame_buf_internal(HOME_PRI, &top, &width, &format,
					   PSP_DISPLAY_SETBUF_IMMEDIATE) >= 0 &&
		    fits(top, width, format, &drawn_home)) {
			/* impose draws it red itself */
			if (home_hooked)
				continue;
			draw(top, width, format, &drawn_home);
#ifdef DEBUG
			home_draws++;
#endif
			continue;
		}
#ifdef DEBUG
		thread_pixels += draw_on_screen();
		thread_draws++;
#else
		draw_on_screen();
#endif
		/* what the game draws over the icon before scan-out gets to it */
		for (passes = 0; !scanned(passes); passes++) {
			sceKernelDelayThread(get_hcount ? LATE_DELAY : BLIND_DELAY);
#ifdef DEBUG
			late_pixels += draw_on_screen();
#else
			draw_on_screen();
#endif
		}
	}

	return 0;
}

/*
 * Called once a second from main_thread. The notice stays up for as long
 * as the battery is low, like the blinking power LED.
 */
void notice_tick(int low)
{
#ifdef DEBUG
	/* always shown, so it can be checked on a full battery */
	static int stats_left = DEBUG_STATS_SECS;
	static int last_low = -1;

	if (low != last_low) {
		dlog("low battery", low);
		last_low = low;
	}
	if (stats_left) {
		if (stats_left % 10 == 0) {
			dlog("hook calls", hook_calls);
			dlog("hook draws", hook_draws);
			dlog("hook top", hook_top);
			dlog("hook width", hook_width);
			dlog("hook format", hook_format);
			dlog("hook sync", hook_sync);
			dlog("thread draws", thread_draws);
			dlog("thread pixels", thread_pixels);
			dlog("home draws", home_draws);
			dlog("home tints", home_tints);
			dlog("tint draws", tint_draws);
			dlog("tint pixels", tint_pixels);
			dlog("late pixels", late_pixels);
			dlog("ge calls", ge_calls);
			dlog("ge pixels", ge_pixels);
		}
		stats_left--;
	}
	low = 1;
#endif
	notice_visible = low;
}

void notice_init(void)
{
	u32 orig = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", NID_SET_FRAME_BUF);
	SceUID thid;

	dlog("sceDisplaySetFrameBuf", orig);
	if (!orig)
		return;

	if (sceKernelInitKeyConfig() == PSP_INIT_KEYCONFIG_POPS) {
		drawn_frame.x += POPS_DX;
		drawn_frame.y += POPS_DY;
		drawn_frame.tint = 1;
		drawn_home.x += POPS_DX;
		drawn_home.y += POPS_DY;
	}

	set_frame_buf = (void *)orig;
	sctrlHENPatchSyscall((void *)orig, set_frame_buf_hook);

	get_frame_buf = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", NID_GET_FRAME_BUF);
	wait_vblank_start = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", NID_WAIT_VBLANK_START);
	dlog("sceDisplayGetFrameBuf", (u32)get_frame_buf);
	dlog("sceDisplayWaitVblankStart", (u32)wait_vblank_start);
	if (!get_frame_buf || !wait_vblank_start)
		return;

	get_frame_buf_internal = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver",
							      NID_GET_FRAME_BUF_INT);
	if (!get_frame_buf_internal)
		get_frame_buf_internal = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver",
								      NID_GET_FRAME_BUF_INT_OLD);
	dlog("sceDisplayGetFrameBufferInternal", (u32)get_frame_buf_internal);

	get_hcount = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", NID_GET_HCOUNT);
	dlog("sceDisplayGetCurrentHcount", (u32)get_hcount);

	orig = sctrlHENFindFunction("sceGE_Manager", "sceGe_user", NID_GE_DRAW_SYNC);
	dlog("sceGeDrawSync", orig);
	if (orig) {
		ge_draw_sync = (void *)orig;
		sctrlHENPatchSyscall((void *)orig, ge_draw_sync_hook);
	}

	/* above the game's threads, so the icon is in before scan-out reaches it */
	thid = sceKernelCreateThread("BatteryPercNotice", vblank_thread, 0x10, 0x1000, 0, NULL);
	if (thid >= 0)
		sceKernelStartThread(thid, 0, NULL);
}
