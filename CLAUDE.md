# RadioFloppy – notes for Claude Code

WiFi floppy emulator for the Atari ST: ESP32-S3 (Otronic DevKitC, N16R8)
on a carrier board, ESP-IDF 5.5.5, C. It plays .ST and HFE (v1/v3) images
as drive B: over the Shugart interface; images live in an image library on
an external SPI NOR flash; HTTP API + web UI (`docs/API.md`).

## Read first

- `docs/HANDOFF_BOARD_REV2.md` – current work: firmware for PCB revision 2.
- `docs/HANDOFF_HFE.md` – the HFE player: design, test results, open steps.
- `README.md`, `docs/API.md`, `docs/FLASH_LAYOUT.md` for the rest.

These handoff files replace the chat history of earlier sessions. Keep
them up to date: when the user asks for "aantekeningen" (notes), update or
add a handoff file before committing.

## State (2026-10-12)

| Branch | State |
| --- | --- |
| `main` | Firmware for PCB revision 1 with the HFE player (HFEv1 and HFEv3 games run on the Atari). |
| `wip/board-rev2` | `main` + firmware for PCB revision 2 (new pin map, shared SPI bus with SD card, LEDs on J4). Builds; **not flashed or tested yet** – the rev 2 board is being manufactured. |
| `wip/hfe-player` | Old, equal to `main`; can be deleted. |

PCB revision 1 is retired: no need to keep supporting it. **Never flash
`wip/board-rev2` on a rev 1 board** (GPIO20 is the buzzer there).

Open work, in order: test rev 2 when it arrives (checklist in
`docs/HANDOFF_BOARD_REV2.md`) and merge it into `main`; SD card support;
HFE weak bits (phase 3, needs a test file with real RAND areas); a timing
measurement tool (P-47 hangs after its intro); cosmetic items listed in
`docs/HANDOFF_HFE.md`.

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
  timing on hardware, don't assume; HFE is READ_ONLY and stored
  compressed; never accept an image that cannot be activated.

## Build, flash, test

Linux PC (attic):
```sh
. ~/esp/esp-idf/export.sh
idf.py -p /dev/ttyACM0 build flash      # opening the port resets the board
tests/host/run.sh                        # host tests (gcc)
RADIOFLOPPY_HOST=<ip> python3 tests/api/api_test.py
RADIOFLOPPY_HOST=<ip> python3 tests/web/ui_test.py   # websocket-client + Chrome
```

Windows laptop: ESP-IDF in `C:\Users\frank\esp\esp-idf`
(`. C:\Users\frank\esp\esp-idf\export.ps1`, Python 3.12 and Git on PATH
first), build dirs outside OneDrive (`-B C:\Users\frank\esp\build-radiofloppy`,
`...-rev2` for the rev 2 branch), serial COM3. Host tests from Git Bash
with `PATH=/c/msys64/ucrt64/bin:$PATH`. UI test: set `CHROME` to the
Chrome executable. Turn off the WireGuard `retroloft` tunnel there: it also
routes 192.168.178.0/24 and hides the board.

Device: API `http://192.168.178.62/api/v1/status` (DHCP, may change).
After any firmware change run both device tests (ST regression) before
reporting success. `tests/api/api_test.py` needs
`images/CRYSTAL_CASTLES.ST` (git-ignored; download it from the board:
`GET /api/v1/images/1/data`, CRC 42ce7eed). The tests leave the first
original image active.

Test images: Linux `/mnt/retroloft/retro/FLOPPIES/HFE` (1442 HFEv1);
OneDrive `retro/P` (75 HFEv1 converted from STX) and `retro/HFEv3` (Klax,
HFEv3 from a Greaseweazle flux dump). Commercial images are never committed.

## Code map (main/)

- `board_pins.h` – pin map (rev 2 on `wip/board-rev2`).
- `drive_emu.c` – Shugart drive state in ISRs; `drive_swap_media` (disk
  change, also while selected unless the computer writes).
- `flux_stream.c` – RMT flux (RDATA) and INDEX; ST mode and HFE mode.
- `disk_image.c`, `disk_switch.c` – track buffers in PSRAM, disk changes.
- `hfe.c` / `hfe_import.c` – HFE parser, packing, streaming upload.
- `image_store.c`, `ext_flash.c`, `img_codec.c` – image library on the NOR flash.
- `api.c`, `web/index.html` – HTTP API and web UI. `oled.c`, `leds.c`,
  `buttons.c`, `step_sound.c` – front panel. `wifi_net.c`, `setup_mode.c` – WiFi.
