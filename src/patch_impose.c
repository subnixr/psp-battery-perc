/*
 * HOME screen (impose.prx). Its status line is built from 16x15 sprites:
 * the clock routine is called with the x where the text must end (450 when
 * a battery is present, the icon sits at 450-480) and draws date and time
 * right-aligned from digit sprites. We take over that call, draw "NN%"
 * with impose's own digits and blit routine, and hand the clock a smaller
 * right edge.
 * Every call impose makes to that blit routine is also taken over, so the
 * low battery notice can swap the battery sprite for a red one.
 */
#include <pspkernel.h>
#include <psploadcore.h>
#include "common.h"
#include "percent_glyph.h"

#define ICON_X		450
#define SPACE_W		5	/* width of the blank impose leaves after the clock */
#define GLYPH_W		16
#define GLYPH_H		15
#define GLYPH_BYTES	(GLYPH_W * GLYPH_H * 2)
#define PERCENT_ADV	13

/* Offsets inside the clock routine, identical on 01g-11g and 05g builds. */
#define CLK_LUI_STATE	0x008	/* lui  $s5, hi(state_ptr) */
#define CLK_LW_STATE	0x03C	/* lw   $v1, lo(state_ptr)($s5) */
#define CLK_IS_DIGIT	0x274	/* sltiu $t6, $a0, 10 */
#define CLK_LW_ODD	0x298	/* lw   $t3, digits_odd($t0) */
#define CLK_JAL_BLIT	0x2B0	/* jal  blit */
#define CLK_LW_EVEN	0x324	/* lw   $t9, digits_even($t1) */

static int (*impose_clock)(void *time, int right, int y, int mode);
static int (*impose_blit)(int x, int y, int w, int h, const void *src);
static u32 *state_ptr;
static int digits_even, digits_odd;
/* Runs of clock_hook: impose only draws its clock while the HOME screen is up. */
static volatile u32 clock_draws;

#ifdef DEBUG
/* What the hooks last saw, logged from impose_debug_tick. */
static volatile u32 clock_calls, clock_passes, blit_calls;
static volatile int clock_right, clock_y, clock_mode;
/* Percentages drawn, and the x the last one starts at; draws given up on. */
static volatile u32 percent_draws, percent_bails;
static volatile int percent_left;
#endif

static int advance(int digit)
{
	return digit == 1 ? 10 : digit == 2 ? 11 : 12;
}

/* Draw "NN%" ending at x = right, return the x it starts at. */
static int draw_percent(int percent, int right, int y)
{
	const u8 *even, *odd;
	int digits[3];
	int n = 0;
	int width = PERCENT_ADV;
	int x, left, i;

	even = *state_ptr ? *(const u8 **)(*state_ptr + digits_even) : NULL;
	odd = *state_ptr ? *(const u8 **)(*state_ptr + digits_odd) : NULL;
	if (!even || !odd) {
#ifdef DEBUG
		percent_bails++;
#endif
		return right;
	}

	if (percent >= 100)
		digits[n++] = percent / 100;
	if (percent >= 10)
		digits[n++] = percent / 10 % 10;
	digits[n++] = percent % 10;
	for (i = 0; i < n; i++)
		width += advance(digits[i]);

	left = x = right - width;
	for (i = 0; i < n; i++) {
		impose_blit(x & ~1, y, GLYPH_W, GLYPH_H,
			    (x & 1 ? odd : even) + digits[i] * GLYPH_BYTES);
		x += advance(digits[i]);
	}
	impose_blit(x & ~1, y, GLYPH_W, GLYPH_H, x & 1 ? percent_odd : percent_even);
#ifdef DEBUG
	percent_draws++;
	percent_left = left;
#endif

	return left;
}

static int clock_hook(void *time, int right, int y, int mode)
{
	int percent = battery_percent;

	clock_draws++;
#ifdef DEBUG
	clock_calls++;
	clock_right = right;
	clock_y = y;
	clock_mode = mode;
#endif
	if (mode == 0 && right == ICON_X && percent >= 0) {
#ifdef DEBUG
		clock_passes++;
#endif
		right = draw_percent(percent, right - SPACE_W, y);
	}

	return impose_clock(time, right, y, mode);
}

static int blit_hook(int x, int y, int w, int h, const void *src)
{
#ifdef DEBUG
	blit_calls++;
#endif
	return impose_blit(x, y, w, h, notice_home_sprite(w, h, src));
}

/* Called once a second from main_thread. */
int home_opened(void)
{
	static u32 seen;
	static int was_up;
	u32 draws = clock_draws;
	int up = draws != seen;
	int opened = up && !was_up;

	seen = draws;
	was_up = up;

	return opened;
}

#ifdef DEBUG
/* Called once a second from main_thread. Logs when the hooks have run since. */
void impose_debug_tick(void)
{
	static u32 logged_clock, logged_blit;
	static int secs;

	if (++secs % 10 || (clock_calls == logged_clock && blit_calls == logged_blit))
		return;
	logged_clock = clock_calls;
	logged_blit = blit_calls;

	dlog("impose clock calls", clock_calls);
	dlog("impose clock passes", clock_passes);
	dlog("impose clock right", clock_right);
	dlog("impose clock y", clock_y);
	dlog("impose clock mode", clock_mode);
	dlog("impose blit calls", blit_calls);
	dlog("impose percent draws", percent_draws);
	dlog("impose percent bails", percent_bails);
	dlog("impose percent left", percent_left);
	dlog("impose state", state_ptr ? *state_ptr : 0);
}
#endif

int patch_impose(void)
{
	/*
	 * li $a2, 450; li $a1, 480; lw $a3, 96($s0); addiu $a0, $s0, time;
	 * movn $a1, $a2, $a3; move $a3, $zr; jal clock; li $a2, 5
	 */
	static const u32 sig[] = {
		0x240601C2, 0x240501E0, 0x8E070060, 0x26040000,
		0x00C7280B, 0x00003821, 0x0C000000, 0x24060005,
	};
	static const u32 mask[] = {
		0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF0000,
		0xFFFFFFFF, 0xFFFFFFFF, 0xFC000000, 0xFFFFFFFF,
	};
	SceModule *mod = sceKernelFindModuleByName("sceImpose_Driver");
	u32 *site, *clk, *p;
	int blits = 0;

	if (!mod) {
		dlog("impose module not found", 0);
		return 0;
	}

	site = sig_find(mod->text_addr, mod->text_size, sig, mask, 8);
	dlog("impose call site", (u32)site);
	if (!site)
		return 1;
	site += 6;
	clk = (u32 *)JAL_TARGET(site, *site);

	if ((clk[CLK_LUI_STATE / 4] & 0xFFFF0000) != 0x3C150000 ||
	    (clk[CLK_LW_STATE / 4] & 0xFFFF0000) != 0x8EA30000 ||
	    clk[CLK_IS_DIGIT / 4] != 0x2C8E000A ||
	    (clk[CLK_LW_ODD / 4] & 0xFFFF0000) != 0x8D0B0000 ||
	    !IS_JAL(clk[CLK_JAL_BLIT / 4]) ||
	    clk[CLK_JAL_BLIT / 4 + 1] != 0x2407000F ||
	    (clk[CLK_LW_EVEN / 4] & 0xFFFF0000) != 0x8D390000) {
		dlog("impose clock routine not recognised", (u32)clk);
		return 1;
	}

	state_ptr = (u32 *)((clk[CLK_LUI_STATE / 4] << 16) + (s16)clk[CLK_LW_STATE / 4]);
	digits_odd = (s16)clk[CLK_LW_ODD / 4];
	digits_even = (s16)clk[CLK_LW_EVEN / 4];
	impose_blit = (void *)JAL_TARGET(&clk[CLK_JAL_BLIT / 4], clk[CLK_JAL_BLIT / 4]);
	impose_clock = (void *)clk;

	*site = MAKE_CALL(clock_hook);
	for (p = (u32 *)mod->text_addr; p < (u32 *)(mod->text_addr + mod->text_size); p++) {
		if (IS_JAL(*p) && JAL_TARGET(p, *p) == (u32)impose_blit) {
			*p = MAKE_CALL(blit_hook);
			blits++;
		}
	}
	flush_caches();
	dlog("impose patched, clock routine", (u32)clk);
	dlog("impose blit calls patched", blits);

	return 1;
}
