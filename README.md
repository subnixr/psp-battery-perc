# psp-battery-perc

PSP plugin: battery percentage between the clock and the battery icon, in
the XMB and in the in-game HOME (exit game) screen, and a red empty battery
icon over the XMB battery icon, in the XMB and in games, for as long as the
battery is low. Firmware 6.61, any CFW with a SystemControl start-module handler
(ARK, PRO, ME).

## Build

    make            # docker, pspdev/pspdev image
    make -C src     # local pspdev toolchain

Output: `battery_perc.prx`. `make clean && make DEBUG=1` builds a
variant that logs to `ms0:/battery_perc.log` (`ef0:` on the Go) and always
shows the low battery notice.

`make clean && make BATLOG=1` builds a variant that appends a line to
`battery_log.csv` (same place) every 5 seconds: seconds since the plugin
started, battery mV, firmware percent, shown percent, AC online, charging,
CPU MHz, firmware low battery flag. A `#` header line marks each plugin
start.

## Install

Copy `battery_perc.prx` to `SEPLUGINS` and enable it for both the XMB and
games.

ARK (`SEPLUGINS/PLUGINS.TXT`):

    vsh, ms0:/SEPLUGINS/battery_perc.prx, on
    game, ms0:/SEPLUGINS/battery_perc.prx, on

PRO / ME (`seplugins/vsh.txt` and `seplugins/game.txt`):

    ms0:/seplugins/battery_perc.prx 1

Use `ef0:` instead of `ms0:` for the PSP Go internal storage.

## How it works

- XMB: vshmain formats the clock text once a minute. The call is redirected
  to a stub in user memory that appends ` NN%` ([src/patch_vsh.c](src/patch_vsh.c)).
  When the percentage changes the plugin spoils the time vshmain kept from
  its last text, so the text is built again within a second.
- HOME screen: impose.prx draws its clock from digit sprites, right-aligned
  against the battery icon. The plugin draws `NN%` there with impose's own
  digits and moves the clock left ([src/patch_impose.c](src/patch_impose.c)).
  The `%` sprite comes from `tools/gen_percent.py`.
- Low battery notice: the `sceDisplaySetFrameBuf` syscall is hooked and the
  icon is blended into each frame before it is displayed; a thread does
  the same at every vblank and until scan-out has passed the icon, for games
  whose frame is not finished or not handed over at that point, and a hook
  on `sceGeDrawSync` does it when the game has waited for the GE, for games
  that keep drawing into the frame on screen ([src/notice.c](src/notice.c)). It stays up while the power LED
  blinks: the firmware reports low battery and the PSP is off AC power. On
  the Go the firmware reports it from 12% but the LED starts at 4%, so
  there the notice also waits for 4%. The icon is a pre-rendered mask:
  `tools/gen_notice.py` copies the XMB battery icon from
  `tools/xmb_battery.png`, a corner of an XMB screenshot, without its bars.
  The plugin draws it at the same place; where it finds the stock icon
  there, it takes the white out as the red goes in, so the stock icon turns
  red. Games get it at that place too. The HOME screen is a buffer of its
  own that impose puts on screen above the game's frame and keeps redrawing,
  so there the icon is made red at the source: impose's sprite blits go
  through the plugin, which hands it a copy of its battery sprite with the
  outline turned red ([src/patch_impose.c](src/patch_impose.c)). If that blit
  is never seen, the vblank thread works on impose's buffer instead: a
  stock icon found around that place has its outline turned red as it is,
  charge bars left alone, and otherwise a mask made from impose's own sprite,
  `tools/impose_battery.png`, is drawn. In PS1 games the same is done in the
  game's frame, where pops shows that screen; `POPS_DX`/`POPS_DY` in
  [src/notice.c](src/notice.c) move where the icon goes there when there is
  no stock icon. For PS1
  games enable the plugin for `pops` too.

The PSP Go battery has no fuel gauge. Its firmware derives the percentage
from the battery voltage in eight steps (0, 2, 4, 12, 30, 60, 90, 100). The
voltage reading alternates between two levels near each threshold, so the
plugin shows the firmware value only after a change has held for a minute,
and lets it only fall on battery and only rise on AC power. For 10 seconds
after the charger goes in or out, a wake from sleep or the HOME screen
coming up, the firmware value is shown as it is.

Both patches are located by instruction signatures. If one is not found
that screen is left untouched. `tools/sigcheck.py` runs the same checks on
decrypted firmware modules on a PC. `make fw` downloads the 6.61 updates
into `fw/`.
