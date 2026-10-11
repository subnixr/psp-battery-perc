#ifndef COMMON_H
#define COMMON_H

#include <pspkernel.h>

#define MAKE_CALL(f)	(0x0C000000 | (((u32)(f) >> 2) & 0x03FFFFFF))
#define JAL_TARGET(pc, op)	(((u32)(pc) & 0xF0000000) | (((op) & 0x03FFFFFF) << 2))
#define IS_JAL(op)	(((op) >> 26) == 3)

/* Battery charge 0-100, or -1 when there is no battery to report. */
extern volatile int battery_percent;

/* First word-aligned match of sig (under mask) in [start, start + size), or NULL. */
u32 *sig_find(u32 start, u32 size, const u32 *sig, const u32 *mask, int n);
void flush_caches(void);

int patch_impose(void);
/* Whether the HOME screen has come up since the last call. Once a second. */
int home_opened(void);
void patch_vsh(u32 text_addr, u32 text_size);
void vsh_update(int percent);
void vsh_refresh(void);
void notice_init(void);
void notice_tick(int low);
/* src, or a red copy of it if it is impose's w x h battery sprite and the notice is up. */
const void *notice_home_sprite(int w, int h, const void *src);

#ifdef DEBUG
void dlog(const char *msg, u32 val);
void impose_debug_tick(void);
#else
#define dlog(msg, val)	((void)0)
#define impose_debug_tick()	((void)0)
#endif

#ifdef BATLOG
void batlog_tick(int shown);
#else
#define batlog_tick(shown)	((void)0)
#endif

#endif
