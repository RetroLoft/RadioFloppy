# Handoff: board revision 2 (state at 2026-10-23)

For whoever continues this work (a new Claude session or a person). Read
this first, next to `docs/HANDOFF_HFE.md` (the HFE player, on `main`).

## Where things stand

The rev 2 firmware is **merged into `main`** and runs on the rev 2 board.
**Revision 1 is history**: not supported any more. Do not flash current
firmware on a rev 1 board (GPIO20 is the buzzer there and the LED on J4
pin 3 on rev 2).

## What changed from revision 1 to 2

Source: the U1 pads and their nets in `hardware/v1.1/Floppy emulator.kicad_pcb`
(all 44 pads checked) and the user's hardware description.

| Function | Rev 1 | Rev 2 | Circuit |
| --- | --- | --- | --- |
| Buzzer | GPIO20 | **GPIO46** | 1k to the BC817 base. GPIO46 is a strapping pin, pulled low at reset: silent while booting. |
| LED_STATUS | GPIO48 (DevKitC RGB LED) | **GPIO20** | 470R to J4, active high. Native USB D+ pad: `usb_phy_release_pins()` stays needed. |
| LED_ACTIVITY | GPIO38 | **GPIO48** | 470R to J4, active high. Also the DevKitC RGB LED data input: used as plain GPIO, that LED stays dark. |
| SD_CS | – | **GPIO38** | J5, external SD card module (SPI: CS, MOSI, CLK, MISO, 3V3, GND). |
| DS0/DS1 pull-ups | – | **10k to +5 V** (R4/R5) | Connector side of the SN74LVC245A. MOTOR and the other inputs still float while the Atari is off. |

Unchanged: all Shugart inputs and outputs, NOR flash (CS 10, MOSI 11,
CLK 12, MISO 13, IO2 14, IO3 9), I2C (SDA 47, SCL 21), buttons (SW_PREV 8,
SW_NEXT 19), UART0.

## What the firmware does now (branch `wip/board-rev2`)

- `main/board_pins.h`: rev 2 pin map; the SPI bus pins are named once
  (`PIN_SPI_*`) because the NOR flash and the SD card share them.
- **Shared SPI bus**: SD_CS is driven HIGH in the bootloader hook
  (`bootloader_components/early_pins`) and again with NOR_CS in
  `spi_cs_release()` (`main/board.c`, called from `app_main`), so a card in
  J5 can never answer NOR flash traffic, even before there is SD code.
- **Changed 2026-10-21 (user request):** setting `leds` (1 or 2 LEDs on
  J4, web Settings / API, settings record v6). J4 pin 3 (GPIO20) is now
  always disk activity; the WiFi status goes to J4 pin 1 (GPIO48) with 2
  LEDs, and with 1 LED to the DevKitC RGB LED (WS2812 on GPIO48, RMT
  channel without DMA, dim blue). The description below is the original
  rev 2 design, kept for history.
- `main/leds.c` (replaces the RMT/WS2812 `status_led.c`), plain GPIOs:
  - LED_ACTIVITY: on while drive B: is selected and MOTOR is active (armed).
  - LED_STATUS (WiFi): off = connected; 1 Hz blink = starting or joining;
    on = no connection for `LEDS_WIFI_GIVE_UP_MS` (60 s) or no network set
    (it keeps retrying and goes off once connected); 4 Hz blink = WiFi
    setup mode. The 60 s and the setup-mode blink were chosen by Claude,
    the rest by the user.
  - `wifi_net_get_status()` now reports `started` and `down_ms` for this.
  - Started in normal mode, without emulation (unsupported machine) and in
    setup mode.
- Removed: `led_test.c`, `button_test.c`, `status_led.c/.h`. The loopback
  test stays (useful on rev 2 too).
- README updated for rev 2.
- Build on the Windows laptop into a separate directory:
  `idf.py -B C:\Users\frank\esp\build-radiofloppy-rev2 -p COMx build flash`.

## Test results on the rev 2 board (2026-10-21 .. 10-23)

| Item | Result |
| --- | --- |
| Flash (app only), boot, image library on the NOR flash | OK (21 images, catalog intact; the library has since been removed) |
| DS0/DS1 pull-ups | OK with +5 V on J2 (Atari on): `"selected": false` when idle. Without +5 V on J2 (Atari off, board on USB) the pull-ups have no supply, all lines read active and the activity LED lights; the user accepts this, normal use has +5 V. |
| Atari reads a game through the emulator | OK |
| LEDs | Activity on J4 pin 3 OK. 1/2 LED setting (see below) switches at once. With 470R a standard green 3 mm LED is dim (about 2.5 mA). |
| SD card in J5 | OK: 32 GB SDHC mounted (SPI, 20 MHz), long file names, root directory created; the NOR flash keeps working next to it. |
| ST regression | `api_test.py` passes; `ui_test.py` only the known "full flash: NO_SPACE" failure (compression is on). |
| Not checked explicitly yet | Buttons left/right orientation; buzzer clicks on GPIO46; disk change while a game holds the drive. |

## Firmware changes after the first rev 2 version

- **Front panel LEDs** (setting `leds`, 1 or 2; web Settings, API): J4 pin 3
  (GPIO20) always shows disk activity. WiFi status (blinking while
  connecting, on when no connection can be made, off when connected, fast
  in setup mode): with 2 LEDs on J4 pin 1 (GPIO48), with 1 LED on the
  DevKitC RGB LED in dim blue (WS2812 on GPIO48 via an RMT channel without
  DMA). Default 1.
- **SD card** (`main/sd_card.c`): mounted at start-up on the shared SPI bus
  (own CS GPIO38, own lock), FAT with long file names, never formatted; a
  removed card is noticed, an inserted one mounted within about 10 s.
- **Root directory** (setting `root_dir`, settings record v7): the folder on
  the card where RadioFloppy works ("" = the card's root), strictly
  validated and only used below `/sd`; created when missing. The user's
  card has `My Floppies` set.

## Hardware notes for a next revision

- LED resistors R2/R3: 150R (about 7-8 mA) instead of 470R; a standard
  green LED is dim at 470R. (A modern "pure green" LED needs about 3 V and
  would need a transistor to 5 V.)
- Pull-ups on all Shugart inputs (about 1k to +5 V, jumper-selectable):
  some PC floppy controllers expect the termination at the drive end (a
  Gotek board has them).
- A +5 V sense from J2 to a free GPIO (e.g. GPIO3 or GPIO35-37, unused)
  so the firmware knows whether the computer is powered.
- Optionally read pin 2 as an input too (PC density select, Falcon HD
  signalling).

## Images on the SD card (done) and what comes next

The image library on the NOR flash is gone (`image_store.c`,
`img_codec.c`, `hfe_import.c` and their tests removed; the old catalogs
are erased once at start-up). Now:

- Images are files below the root directory. API: `/sd/files`,
  `/sd/folders`, `/sd/disks` (new blank 720K .ST), uploads with
  `"destination": "sd"`, `PUT /current {"sd_path"}` for .ST and .HFE
  (HFE decoded while it is streamed from the card, 8 KiB pieces). PSRAM
  uploads (.ST) stay for "play now".
- The NOR flash holds the settings (record v9: `last_sd`, `last_sd_crc`)
  and one copy of the last disk (`main/disk_copy.c`, up to 3 MiB,
  written about 5 s after the last change, data in 64 KiB erase blocks:
  2 MB in about 17 s). Start-up: the file from the card; without a
  readable card the copy (path and CRC must match); a card without the
  file: no disk. See `docs/FLASH_LAYOUT.md`.
- Buttons: previous/next image file in the folder of the disk inserted
  last (natural sort as the web page), no wrap-around.
- Writing: an .ST file on the card is writable unless it has the FAT
  read-only attribute (lock in the web page, `PUT /sd/files
  {"read_only"}`). A save (motor off 2 s) patches the changed 64 KiB
  blocks of the flash copy and commits it as pending (copy header v3),
  writes the same blocks into the file in place and checks its CRC, then
  marks the copy synced. A pending copy is written to the card at the
  next start-up; when the file was replaced or deleted meanwhile, the
  changes go to `<name> (changes).st`. Tested on the ST: Bolo copied from
  a real floppy onto a new disk and loaded from it (925 sectors, one
  save). Not tested yet: a power cut during a save.

Tested on the board: inserting .ST and HFE from the card, the flash
copy, the buttons (their task needs 8 KiB of stack for a step), and
starting without the card from the copy (Klax, HFEv3 2.9 MB: 6.7 s).

Later: HFE tracks read per cylinder when the Atari asks for them instead
of the whole disk in PSRAM; then raise `HFE_MAX_SEGMENTS` (keep
smoothing).

## Working with this user

Same rules as in `docs/HANDOFF_HFE.md`: Dutch in conversation, English in
code and docs; phase by phase, ask before starting the next step; commit
and push only when asked.
