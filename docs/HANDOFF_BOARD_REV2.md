# Handoff: board revision 2 (state at 2026-09-29)

For whoever continues this work (a new Claude session or a person). Read
this first, next to `docs/HANDOFF_HFE.md` (the HFE work, now on `main`).

## Where things stand

| Branch | State |
| --- | --- |
| `main` | Firmware for board revision 1, HFE player merged. Runs on the rev 1 board. |
| `wip/board-rev2` | **Firmware prepared for board revision 2 (`hardware/v1.1`). Builds without warnings; never flashed or tested:** the rev 2 board is being manufactured. |

Board revision 1 is finished and will not be supported any more: the
rev 2 firmware drops everything that only existed for rev 1.

**Do not flash `wip/board-rev2` on a rev 1 board**: GPIO20 is the buzzer
there and the status LED on rev 2, so the buzzer would sound with the
WiFi blink pattern.

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

## Checklist when the rev 2 board arrives

1. Flash `wip/board-rev2` (app only; no chip erase without permission).
2. Buttons: left/right still as on rev 1? (On rev 1 SW_PREV was physically
   the RIGHT button, the opposite of the schematic.) Adjust
   `main/buttons.c` if not.
3. LEDs: ACTIVITY while the Atari reads B:; STATUS blinks at start-up, goes
   off once connected, on after 60 s with WiFi off; fast blink in setup
   mode (both buttons 10 s).
4. Buzzer on GPIO46: STEP clicks (needs +5 V on J2).
5. DS0/DS1 pull-ups: with the Atari off `/api/v1/status` should show
   `"selected": false` (on rev 1 the line floated). MOTOR may still float.
6. ST regression (`tests/api/api_test.py`, `tests/web/ui_test.py`) and one
   HFE image (e.g. Pink Panther, Klax) on the Atari.
7. Disk change while a game holds the drive (from the HFE round, not yet
   tested on the Atari).

Then merge `wip/board-rev2` into `main`.

## Next: the SD card (J5)

Not started. Plan agreed so far (see `docs/HANDOFF_HFE.md`, "Decision for
later"):

- SD in SPI mode on the same SPI2 bus as the NOR flash (ESP-IDF `sdspi`
  device on the existing bus, own CS). Keep the `ext_flash` mutex rules in
  mind: every access to one device goes through its own lock; the bus lock
  arbitrates between the two devices.
- Card init needs >= 74 clocks with CS high at 400 kHz; the flash must not
  be accessed with a different clock setting during that. Test with a card
  inserted and removed; a card that is removed must never block the flash.
- Images as plain files on the card; HFE tracks read per cylinder when the
  Atari asks for them instead of the whole disk in PSRAM. Then raise
  `HFE_MAX_SEGMENTS` (keep smoothing, do not play every jitter step).
- Large images (the 6 HFE files that do not fit now) go to the card.

## Working with this user

Same rules as in `docs/HANDOFF_HFE.md`: Dutch in conversation, English in
code and docs; phase by phase, ask before starting the next step; commit
and push only when asked.
