# RadioFloppy – notes for Claude Code

WiFi floppy emulator for the Atari ST: ESP32-S3 (Otronic DevKitC, N16R8)
on a carrier board, ESP-IDF 5.5.5, C. It plays .ST and HFE (v1/v3) images
as drive B: over the Shugart interface; images live as files on the SD
card (J5); the external SPI NOR flash only holds the settings and a copy
of the last disk; HTTP API + web UI (`docs/API.md`).

## Read first

- `docs/HANDOFF_BOARD_REV2.md` – PCB revision 2: what changed, test results,
  hardware notes for a next revision, the SD card plan (current work).
- `docs/HANDOFF_HFE.md` – the HFE player: design, test results, open steps.
- `README.md`, `docs/API.md`, `docs/FLASH_LAYOUT.md` for the rest.

These handoff files replace the chat history of earlier sessions. Keep
them up to date: when the user asks for "aantekeningen" (notes), update or
add a handoff file before committing.

## State (2026-10-25)

| Branch | State |
| --- | --- |
| `main` | Firmware for **PCB revision 2**, tested on the board: HFE player, LEDs, images on the SD card, writing through the flash journal. The only branch (the `wip/*` branches were merged and deleted 2026-10-25). |

**PCB revision 1 is history**: not supported any more, no need to keep it
working. Never flash current firmware on a rev 1 board (GPIO20 is the
buzzer there).

Open work, in order: HFE weak bits (phase 3, needs a test file with real RAND
areas); P-47 still hangs after its copyright screen (flux and INDEX
measured correct, see `docs/HANDOFF_HFE.md`); cosmetic items listed there.

## How the user wants to work

- Talk **Dutch** with the user; code, comments, web UI and docs in **English**.
- Work step by step. After a finished step, report and **ask before
  starting the next step or a design change**.
- Commit and push **only when asked**. "commit" means commit **and push**.
  End commit messages with the Co-Authored-By line. Separate commits per
  topic.
- Never do a full chip erase without permission. Never commit WiFi
  credentials or tokens (only in the git-ignored `sdkconfig`). Never pass
  client-provided paths to file functions. Never silently wipe or move
  images on the device.
- Requirements from the HFE work (keep): the ST route stays unchanged
  incl. write support; INDEX sits on the real revolution boundary; measure
  timing on hardware, don't assume; HFE is READ_ONLY and stored as it
  is; never accept an image that cannot be activated.

## Build, flash, test

Linux PC (attic):
```sh
. ~/esp/esp-idf/export.sh
idf.py -p /dev/ttyACM0 build flash      # the DevKitC's UART port (CH343)
tests/host/run.sh                        # host tests (gcc)
RADIOFLOPPY_HOST=<ip> python3 tests/api/api_test.py
RADIOFLOPPY_HOST=<ip> python3 tests/web/ui_test.py   # websocket-client + Chrome
```

Windows laptop: ESP-IDF in `C:\Users\frank\esp\esp-idf`
(`. C:\Users\frank\esp\esp-idf\export.ps1`, Python 3.12 and Git on PATH
first), build dirs outside OneDrive (`-B C:\Users\frank\esp\build-radiofloppy`,
`...-rev2` was used for the rev 2 branch), serial COM3. Host tests from Git Bash
with `PATH=/c/msys64/ucrt64/bin:$PATH`. UI test: set `CHROME` to the
Chrome executable. Turn off the WireGuard `retroloft` tunnel there: it also
routes 192.168.178.0/24 and hides the board.

Device: API `http://192.168.178.35/api/v1/status` (rev 2 board; DHCP, may
change). Use the DevKitC's **UART** USB port for flashing and logs: opening
it does not reset the board (reset through RTS/EN, as `idf.py flash` and
`idf.py monitor` do). The native USB port disappears once the firmware runs
(GPIO20 is the LED on J4 pin 3); it only works for flashing in download
mode (hold BOOT, press RESET).
After any firmware change run both device tests (ST regression) before
reporting success. `tests/api/api_test.py` needs
`images/CRYSTAL_CASTLES.ST` (git-ignored, CRC 42ce7eed) and HFE files in
the SD card's root directory. The tests insert the disk that was active
before again at the end.

Test images: Linux `/mnt/retroloft/retro/FLOPPIES/HFE` (1442 HFEv1);
OneDrive `retro/P` (75 HFEv1 converted from STX) and `retro/HFEv3` (Klax,
HFEv3 from a Greaseweazle flux dump). Commercial images are never committed.

## Code map (main/)

- `board_pins.h` – pin map of PCB revision 2.
- `drive_emu.c` – Shugart drive state in ISRs; `drive_swap_media` (disk
  change, also while selected unless the computer writes). A switched-off
  Atari pulls all lines low: STEP and WGATE both low = no selection, and
  the drive is disarmed (`drive_config.h`, `floppy_app.c`).
- `flux_stream.c` – RMT flux (RDATA) and INDEX; ST mode and HFE mode.
  `flux_monitor.c` – debug: measures the INDEX/RDATA pins (`/api/v1/debug/flux`).
- `disk_image.c`, `disk_switch.c` – track buffers in PSRAM, disk changes.
- `disk_write.c` – receives and decodes sectors the computer writes,
  write protection, saving (journal in the flash copy, then the file).
- `hfe.c` – HFE parser, packing, streaming reader.
- `sd_card.c`, `sd_path.c` – SD card in J5 (shared SPI bus, own lock, FAT
  with long names), root directory, path rules, file reader and writer.
- `ext_flash.c`, `disk_copy.c` – NOR flash: the copy of the last disk
  (also the write journal);
  `settings.c` – settings record (v9). Layout: `flash_layout.h`.
- `api.c`, `web/index.html` – HTTP API and web UI. `oled.c`, `leds.c`,
  `buttons.c`, `step_sound.c` – front panel. `wifi_net.c`, `setup_mode.c` – WiFi.
