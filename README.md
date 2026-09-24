# RadioFloppy

WiFi-floppy-emulator voor de Atari ST op basis van een ESP32-S3 (Otronic DevKitC-clone,
16 MB flash, 8 MB PSRAM). Framework: ESP-IDF v5.5.5 (C).

## Hardwaretests

`main/main.c` kiest welke test draait (roep de gewenste functie uit `main/tests.h` aan):

| Test | Bestand | Wat |
| ---- | ------- | --- |
| `floppy_emu_run()` (actief) | `main/floppy_app.c`, `ext_flash.c`, `image_store.c`, `drive_emu.c`, `flux_stream.c`, `mfm_track.c`, `disk_image.c` | **Read-only** floppy-emulatie als drive B: (`EMU_SELECT_LINE`). De image komt uit de image store op de externe SPI-flash U2 (S25FL128L, zie `docs/IMAGE_STORE.md`) of, als ingestelde fallback, uit de firmware. Alle 160 sporen worden bij het opstarten naar MFM gecodeerd in PSRAM (FlashFloppy-layout voor .ST: geen IAM, GAP4a 80, GAP2 22, GAP3 84). RMT+DMA speelt de fluxstroom op GPIO41 (0,8 us pulsen, 10 MHz), een tweede RMT-kanaal de INDEX-puls op GPIO1 (3 ms per 200 ms, synchroon gestart). WPROT actief zodra geselecteerd; schrijven is uitgeschakeld. |
| (oud) `legacy/input_monitor.c` | niet gebouwd | Selectiebewuste monitor met TRACK0 uit de vorige fase, ter referentie. |
| `loopback_test_run()` | `main/loopback_test.c` | Zoekt continu welke Shugart-uitgang (ULN2003A) via een jumper op J1 met welke ingang (SN74LVC245A) verbonden is. |
| `button_test_run()` | `main/button_test.c` | Knoppen: GPIO19 = links (SW_NEXT), GPIO8 = rechts (SW_PREV); piept 0,5 s bij opstarten. |

**Loopbacktest:** de Shugart-lijnen hebben op de PCB geen pull-ups. Plaats op de jumper een
pull-up van 4,7–10 kΩ naar +3V3 (bijv. J3 pin 3, displayconnector), anders komt de lijn na het
vrijgeven van de uitgang niet betrouwbaar HIGH en wordt er niets gemeld.

**Buzzer:** hangt aan het +5V-net van de PCB, dat alleen via J2 (floppyvoeding / Atari) gevoed
wordt: de 5V-pin van de Otronic-clone is alleen een ingang. Bij voeding via USB alleen is de
buzzer dus stil.

**Veiligheid Shugart-uitgangen:** `bootloader_components/early_pins` zet de zes uitgangs-GPIO's
(ULN2003A-ingangen) al in de bootloader op LOW; `shugart_outputs_release()` doet dat opnieuw als
eerste stap in `app_main()`.

**Floppy images:** alleen `images/RETROLOFT_TEST_720K.ST` (eigen testimage) zit in de repository.
Andere images, zoals commerciële spellen, blijven lokaal (`.gitignore`). Leg ze in `images/`; de
build embedt `images/CRYSTAL_CASTLES.ST` automatisch als het bestand aanwezig is. Kies de image met
`DISK_IMAGE_SELECT` in `main/disk_image.h`; ontbreekt de gekozen image, dan wordt de testimage
gebruikt.

Volledige pinmapping: `main/board_pins.h`.

## Welke USB-poort?

Gebruik **uitsluitend de USB-poort van de USB-naar-UART-brug** (op het board meestal
gemarkeerd als `COM` of `UART`). De andere poort (`USB`) is de native USB van de ESP32-S3 en
zit direct op GPIO19/GPIO20. Die poort mag **niet** worden aangesloten: de host trekt D-/D+
via 15 kΩ naar GND, waardoor de rechterknop (GPIO19) permanent "ingedrukt" leest.

Controle onder Linux na het insteken:

```sh
lsusb
ls -l /dev/ttyUSB* /dev/ttyACM*
```

| `lsusb` toont                         | Poort                          |
| ------------------------------------- | ------------------------------ |
| `10c4:ea60` Silicon Labs CP210x       | UART-brug → goed (`/dev/ttyUSB0`) |
| `1a86:55d3` / `1a86:7523` QinHeng CH343/CH340 | UART-brug → goed (`/dev/ttyACM0` of `/dev/ttyUSB0`) |
| `303a:1001` Espressif USB JTAG/serial | native USB → **verkeerde poort** |

## Compileren, flashen en monitor

```sh
. ~/esp/esp-idf/export.sh           # eenmalig per terminal
cd FloppyEmulator-ESP32S3
idf.py set-target esp32s3           # alleen de eerste keer
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Vervang `/dev/ttyUSB0` door de poort die hierboven is gevonden. Stoppen van de monitor:
`Ctrl+]`.

Verwachte uitvoer:

```
Floppy Emulator - Button Test
Ready. Press a button.
Left Button
Right Button
```
