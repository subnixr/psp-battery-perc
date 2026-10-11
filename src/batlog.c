#include <pspkernel.h>
#include <psppower.h>
#include <string.h>
#include "common.h"

#ifdef BATLOG

#define LOG_PERIOD	5	/* seconds; the firmware refreshes the voltage that often */

static char *put_int(char *p, int v)
{
	char tmp[12];
	int i = 0;
	unsigned int u = v < 0 ? -v : v;

	if (v < 0)
		*p++ = '-';
	do {
		tmp[i++] = '0' + u % 10;
		u /= 10;
	} while (u);
	while (i)
		*p++ = tmp[--i];

	return p;
}

static void append(const char *buf, int len)
{
	SceUID fd;

	fd = sceIoOpen("ms0:/battery_log.csv", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd < 0)
		fd = sceIoOpen("ef0:/battery_log.csv", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd < 0)
		return;
	sceIoWrite(fd, buf, len);
	sceIoClose(fd);
}

/*
 * Called once a second from main_thread. One line per LOG_PERIOD:
 * seconds since the plugin started, mV, firmware percent, shown percent,
 * AC online, charging, CPU MHz, firmware low battery flag. A header line
 * marks each plugin start (boot, or going between the XMB and a game).
 */
void batlog_tick(int shown)
{
	static const char header[] = "#secs,mv,fw_pct,shown_pct,online,charging,cpu_mhz,low\n";
	static int secs = -1;
	char line[80];
	char *p = line;

	if (++secs % LOG_PERIOD)
		return;
	if (secs == 0)
		append(header, sizeof(header) - 1);

	p = put_int(p, secs);
	*p++ = ',';
	p = put_int(p, scePowerGetBatteryVolt());
	*p++ = ',';
	p = put_int(p, scePowerGetBatteryLifePercent());
	*p++ = ',';
	p = put_int(p, shown);
	*p++ = ',';
	p = put_int(p, scePowerIsPowerOnline());
	*p++ = ',';
	p = put_int(p, scePowerIsBatteryCharging());
	*p++ = ',';
	p = put_int(p, scePowerGetCpuClockFrequency());
	*p++ = ',';
	p = put_int(p, scePowerIsLowBattery());
	*p++ = '\n';
	append(line, p - line);
}

#endif
