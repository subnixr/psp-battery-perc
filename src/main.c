#include <pspkernel.h>
#include <psppower.h>
#include <pspinit.h>
#include <pspsysmem_kernel.h>
#include <systemctrl.h>
#include <string.h>
#include "common.h"

PSP_MODULE_INFO("BatteryPerc", PSP_MODULE_KERNEL, 1, 0);

#define HOLD_SECS	60
/*
 * How long the firmware percentage is shown as it is after the charger goes
 * in or out, a wake from sleep or the HOME screen coming up. The firmware
 * takes a few seconds to catch up with the first three.
 */
#define EVENT_SECS	10
#define MODEL_GO	4	/* sceKernelGetModel */
#define GO_LED_PERCENT	4

volatile int battery_percent = -1;

static STMOD_HANDLER previous;
static int is_go;
static volatile int resumed;

u32 *sig_find(u32 start, u32 size, const u32 *sig, const u32 *mask, int n)
{
	u32 *p = (u32 *)start;
	u32 *end = (u32 *)(start + size) - n;
	int i;

	for (; p <= end; p++) {
		for (i = 0; i < n && (p[i] & mask[i]) == sig[i]; i++)
			;
		if (i == n)
			return p;
	}

	return NULL;
}

void flush_caches(void)
{
	sceKernelDcacheWritebackInvalidateAll();
	sceKernelIcacheInvalidateAll();
}

#ifdef DEBUG
void dlog(const char *msg, u32 val)
{
	static const char hex[] = "0123456789ABCDEF";
	char num[12];
	SceUID fd;
	int i;

	fd = sceIoOpen("ms0:/battery_perc.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd < 0)
		fd = sceIoOpen("ef0:/battery_perc.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd < 0)
		return;

	num[0] = ' ';
	for (i = 0; i < 8; i++)
		num[1 + i] = hex[(val >> (28 - 4 * i)) & 0xF];
	num[9] = '\n';
	sceIoWrite(fd, msg, strlen(msg));
	sceIoWrite(fd, num, 10);
	sceIoClose(fd);
}
#endif

/* Only called from main_thread. */
static int read_battery(void)
{
	int percent;

	if (scePowerIsBatteryExist() != 1)
		return -1;
	percent = scePowerGetBatteryLifePercent();
	if (percent < 0)
		return -1;

	return percent > 100 ? 100 : percent;
}

/*
 * The Go's voltage reading alternates between two levels near each
 * threshold, and every flip moves the firmware percentage by a whole step.
 * A change is taken only after it has held for HOLD_SECS, and only downwards
 * on battery or upwards on AC power. With follow set it is taken as it is.
 *
 * Only called from main_thread, once a second.
 */
static int debounce(int raw, int online, int follow)
{
	static int shown = -1;
	static int held;

	if (raw < 0 || shown < 0 || follow) {
		held = 0;
		return shown = raw;
	}
	if (raw == shown || (raw > shown) != online) {
		held = 0;
		return shown;
	}
	if (++held >= HOLD_SECS) {
		held = 0;
		shown = raw;
	}

	return shown;
}

/*
 * Whether the power LED is blinking. On the Go the firmware reports low
 * battery from the 12% step, but the LED only starts blinking at 4%.
 *
 * Only called from main_thread.
 */
static int is_low(int percent)
{
	if (scePowerIsBatteryExist() != 1 || scePowerIsLowBattery() != 1 ||
	    scePowerIsPowerOnline() == 1)
		return 0;

	return !is_go || (percent >= 0 && percent <= GO_LED_PERCENT);
}

static int on_module_start(SceModule *mod)
{
	if (strcmp(mod->modname, "vsh_module") == 0) {
		patch_vsh(mod->text_addr, mod->text_size);
		vsh_update(battery_percent);
	}

	return previous ? previous(mod) : 0;
}

static int power_cb(int unknown, int info, void *arg)
{
	if (info & PSP_POWER_CB_RESUME_COMPLETE)
		resumed = 1;

	return 0;
}

static int main_thread(SceSize args, void *argp)
{
	int impose_done = 0;
	int tries = 0;
	int last = -2;
	int last_online = -1;
	int follow = 0;
	int refresh = 0;
	int percent, online, event;
	SceUID cbid;

	dlog("key config", sceKernelInitKeyConfig());
	cbid = sceKernelCreateCallback("BatteryPercPower", power_cb, NULL);
	if (cbid >= 0)
		dlog("power callback slot", scePowerRegisterCallback(-1, cbid));
	while (1) {
		/* impose is normally up before plugins load; retry a little in case it is not */
		if (!impose_done && tries++ < 10)
			impose_done = patch_impose();

		online = scePowerIsPowerOnline() == 1;
		event = (last_online >= 0 && online != last_online) | resumed << 1 |
			home_opened() << 2;
		last_online = online;
		resumed = 0;
		if (event) {
			dlog("refresh event", event);
			follow = EVENT_SECS;
		}

		percent = debounce(read_battery(), online, follow);
		if (follow)
			follow--;
		if (percent != last) {
			dlog("battery percent", percent);
			dlog("battery mV", scePowerGetBatteryVolt());
			battery_percent = percent;
			vsh_update(percent);
			last = percent;
			/*
			 * twice: vshmain may be past building the text but not yet
			 * past keeping its time, and would undo the first
			 */
			refresh = 2;
		}
		if (refresh) {
			vsh_refresh();
			refresh--;
		}
		notice_tick(is_low(percent));
		impose_debug_tick();
		batlog_tick(percent);

		sceKernelDelayThreadCB(1000 * 1000);
	}

	return 0;
}

int module_start(SceSize args, void *argp)
{
	SceUID thid;

	is_go = sceKernelGetModel() == MODEL_GO;
	previous = sctrlHENSetStartModuleHandler(on_module_start);
	notice_init();

	thid = sceKernelCreateThread("BatteryPercMain", main_thread, 0x18, 0x1000, 0, NULL);
	if (thid >= 0)
		sceKernelStartThread(thid, args, argp);

	return 0;
}

int module_stop(SceSize args, void *argp)
{
	return 0;
}
