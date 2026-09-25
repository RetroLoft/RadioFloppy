/*
 * Front panel buttons: left = previous disk, right = next disk
 * (disk_switch_step, on release); both held 3 s = network name and IP
 * address on the OLED. Left is GPIO19 (SW2 / SW_NEXT on the schematic),
 * right is GPIO8 (SW1 / SW_PREV): the physical positions are what counts.
 */
#pragma once

/* Configure the buttons and start their task (core 1). */
void buttons_start(void);
