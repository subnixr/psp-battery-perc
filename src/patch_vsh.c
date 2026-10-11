/*
 * XMB (vshmain.prx). The clock text is built once a minute into a 100
 * character stack buffer by paf's wide strftime and then set on the clock
 * widget. vshmain runs in user mode and cannot call into this module, so
 * the strftime call is pointed at a small stub placed in user memory that
 * calls the original and appends a suffix string this module keeps current.
 * The routine around that call runs every second, off a timer on the clock
 * widget, and returns early when the time it kept from the last text still
 * has the same month, day, hour and minute. Spoiling the minute it kept
 * makes the next run build the text again.
 */
#include <pspkernel.h>
#include <pspsysmem.h>
#include "common.h"

#define SUFFIX_LEN	8	/* " 100%" + terminator, in UTF-16 units */

/* Stub layout, in words. */
#define STUB_WORDS	19
#define STUB_PTR	STUB_WORDS		/* u32: active suffix buffer */
#define STUB_BUF0	(STUB_WORDS + 1)	/* u16[SUFFIX_LEN] */
#define STUB_BUF1	(STUB_BUF0 + SUFFIX_LEN / 2)
#define STUB_TOTAL	(STUB_BUF1 + SUFFIX_LEN / 2)

/* Words from the call site to the instructions that give away the kept time. */
#define SITE_LUI_KEPT	-26	/* lui   $s1, hi(kept) */
#define SITE_ADDIU_KEPT	-17	/* addiu $a0, $s1, lo(kept) */
#define SITE_LHU_MONTH	-15	/* lhu   $v0, 2($a0) */
#define SITE_LHU_MINUTE	66	/* lhu   $v1, 8($a0) */
#define KEPT_MINUTE	8

static u32 *stub;
static u16 *kept_minute;

/* Have the clock text, and with it the suffix, built again within a second. */
void vsh_refresh(void)
{
	if (kept_minute)
		*kept_minute = 0xFFFF;
}

/*
 * Write the new text into the buffer the stub is not reading, then switch
 * the pointer, so a clock update in progress never sees a half-written one.
 */
void vsh_update(int percent)
{
	u16 *buf;
	int n = 0;

	if (!stub)
		return;

	buf = (u16 *)(stub[STUB_PTR] == (u32)&stub[STUB_BUF0] ? &stub[STUB_BUF1] : &stub[STUB_BUF0]);
	if (percent >= 0) {
		buf[n++] = ' ';
		if (percent >= 100)
			buf[n++] = '0' + percent / 100;
		if (percent >= 10)
			buf[n++] = '0' + percent / 10 % 10;
		buf[n++] = '0' + percent % 10;
		buf[n++] = '%';
	}
	buf[n] = 0;
	stub[STUB_PTR] = (u32)buf;
}

void patch_vsh(u32 text_addr, u32 text_size)
{
	/* move $a0, $s0; li $a2, 100; jal wcsftime; move $a1, $sp */
	static const u32 sig[] = { 0x02002021, 0x24060064, 0x0C000000, 0x03A02821 };
	static const u32 mask[] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFC000000, 0xFFFFFFFF };
	u32 *site = sig_find(text_addr, text_size, sig, mask, 4);
	u32 ptr, orig;
	SceUID block;
	int i;

	dlog("vsh call site", (u32)site);
	if (!site)
		return;

	block = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "BatteryPerc",
					      PSP_SMEM_High, STUB_TOTAL * 4, NULL);
	if (block < 0) {
		dlog("vsh stub alloc failed", block);
		return;
	}
	stub = sceKernelGetBlockHeadAddr(block);
	orig = JAL_TARGET(&site[2], site[2]);
	ptr = (u32)&stub[STUB_PTR];

	for (i = STUB_WORDS; i < STUB_TOTAL; i++)
		stub[i] = 0;
	stub[STUB_PTR] = (u32)&stub[STUB_BUF0];

	i = 0;
	stub[i++] = 0x27BDFFF0;				/* addiu $sp, $sp, -16 */
	stub[i++] = 0xAFBF0000;				/* sw    $ra, 0($sp) */
	stub[i++] = 0xAFA50004;				/* sw    $a1, 4($sp) */
	stub[i++] = MAKE_CALL(orig);			/* jal   wcsftime */
	stub[i++] = 0x24060000 | (100 - SUFFIX_LEN);	/*  li   $a2, 100 - SUFFIX_LEN */
	stub[i++] = 0x8FA50004;				/* lw    $a1, 4($sp) */
	stub[i++] = 0x3C060000 | ((ptr + 0x8000) >> 16);	/* lui   $a2, hi(ptr) */
	stub[i++] = 0x8CC60000 | (ptr & 0xFFFF);	/* lw    $a2, lo(ptr)($a2) */
	stub[i++] = 0x94A80000;				/* 1: lhu $t0, 0($a1) */
	stub[i++] = 0x5500FFFE;				/* bnezl $t0, 1b */
	stub[i++] = 0x24A50002;				/*  addiu $a1, $a1, 2 */
	stub[i++] = 0x94C80000;				/* 2: lhu $t0, 0($a2) */
	stub[i++] = 0x24C60002;				/* addiu $a2, $a2, 2 */
	stub[i++] = 0xA4A80000;				/* sh    $t0, 0($a1) */
	stub[i++] = 0x1500FFFC;				/* bnez  $t0, 2b */
	stub[i++] = 0x24A50002;				/*  addiu $a1, $a1, 2 */
	stub[i++] = 0x8FBF0000;				/* lw    $ra, 0($sp) */
	stub[i++] = 0x03E00008;				/* jr    $ra */
	stub[i++] = 0x27BD0010;				/*  addiu $sp, $sp, 16 */

	/*
	 * Only the call is redirected. ARK-5's XMBControl finds this same site
	 * by its "li $a2, 100" and its SELECT menu crashes when that is gone,
	 * so the stub is what keeps room in the buffer for the suffix.
	 */
	site[2] = MAKE_CALL(stub);
	flush_caches();
	dlog("vsh patched, stub", (u32)stub);

	kept_minute = NULL;
	if ((site[SITE_LUI_KEPT] & 0xFFFF0000) != 0x3C110000 ||
	    (site[SITE_ADDIU_KEPT] & 0xFFFF0000) != 0x26240000 ||
	    site[SITE_LHU_MONTH] != 0x94820002 ||
	    site[SITE_LHU_MINUTE] != 0x94830008) {
		dlog("vsh kept time not recognised", (u32)site);
		return;
	}
	kept_minute = (u16 *)((site[SITE_LUI_KEPT] << 16) + (s16)site[SITE_ADDIU_KEPT] +
			      KEPT_MINUTE);
	dlog("vsh kept minute", (u32)kept_minute);
}
