# RadioFloppy

WiFi-floppy-emulator voor de Atari ST op basis van een ESP32-S3 (Otronic DevKitC-clone,
16 MB flash, 8 MB PSRAM) op de RadioFloppy-PCB revisie 2 (`hardware/v1.1`).
Framework: ESP-IDF v5.5.5 (C).

## Hardwaretests

`main/main.c` kiest welke test draait (roep de gewenste functie uit `main/tests.h` aan):

| Test | Bestand | Wat |
| ---- | ------- | --- |
| `floppy_emu_run()` (actief) | `main/floppy_app.c`, `ext_flash.c`, `image_store.c`, `drive_emu.c`, `flux_stream.c`, `mfm_track.c`, `disk_image.c` | **Read-only** floppy-emulatie als drive B: (`EMU_SELECT_LINE`). De image komt uit de imagebibliotheek op de externe SPI-flash U2 (S25FL128L, blokgebaseerd, zie `docs/FLASH_LAYOUT.md`) of uit een tijdelijke PSRAM-upload. Alle 168 sporen worden bij het opstarten naar MFM gecodeerd in PSRAM (FlashFloppy-layout voor .ST: geen IAM, GAP4a 80, GAP2 22, GAP3 84). RMT+DMA speelt de fluxstroom op GPIO41 (0,8 us pulsen, 10 MHz), een tweede RMT-kanaal de INDEX-puls op GPIO1 (3 ms per 200 ms, synchroon gestart). WPROT actief zodra geselecteerd; schrijven is uitgeschakeld. |
| (oud) `legacy/input_monitor.c` | niet gebouwd | Selectiebewuste monitor met TRACK0 uit de vorige fase, ter referentie. |
| `loopback_test_run()` | `main/loopback_test.c` | Zoekt continu welke Shugart-uitgang (ULN2003A) via een jumper op J1 met welke ingang (SN74LVC245A) verbonden is. |

**Loopbacktest:** de Shugart-lijnen hebben op de PCB geen pull-ups (behalve DS0/DS1). Plaats op de jumper een
pull-up van 4,7–10 kΩ naar +3V3 (bijv. J3 pin 3, displayconnector), anders komt de lijn na het
vrijgeven van de uitgang niet betrouwbaar HIGH en wordt er niets gemeld.

**Webinterface:** `http://<ip>/` — Current Floppy, uploaden en *My floppy images* (laden, volgorde, vervangen, verwijderen, opslag in %) vanuit de browser
(`web/index.html`, in de firmware ingebouwd); tandwiel rechtsboven = instellingen (hostnaam,
firmware). **Knoppen:** links = vorige disk, rechts = volgende; beide 3 s = hostnaam en IP op het
display; beide 10 s = WiFi-setupmodus.

**WiFi-setupmodus:** zonder ingesteld netwerk (of na 10 s beide knoppen) opent RadioFloppy een
eigen WiFi-netwerk met één setup-pagina (netwerk, beveiliging, wachtwoord). De diskemulatie is dan
uit; het display toont netwerknaam, sleutel en adres. Zie `docs/API.md` → *WiFi setup mode*.

**WiFi en HTTP API:** `docs/API.md` — images uploaden (tijdelijk naar PSRAM of in de imagebibliotheek, tot
1,5 MiB), images activeren, ordenen, vervangen en verwijderen via `/api/v1/`. Hostnaam en WiFi-netwerk staan op de
externe flash (instellingen, setupmodus); `idf.py menuconfig` → *RadioFloppy* geeft alleen de
standaardwaarden en het optionele API-token, en staat alleen in `sdkconfig` (git-ignored). Standaard is de
API open op het thuisnetwerk. Tests:
`tests/host/run.sh` (op de PC), `tests/api/api_test.py` en `tests/web/ui_test.py` (tegen het board).

**STEP-geluid en LED's:** bij iedere verwerkte STEP-puls van drive B: geeft de buzzer (GPIO46) een tik van 3 ms (`main/step_sound.h`). De LED's op J4 (`main/leds.h`): LED_ACTIVITY (GPIO48) brandt zolang drive B: geselecteerd is en MOTOR actief is; LED_STATUS (GPIO20) toont WiFi: uit = verbonden, knippert = verbinding zoeken, brandt = na 60 s nog geen verbinding (of geen netwerk ingesteld), snel knipperen = WiFi-setupmodus.

**Buzzer:** hangt aan het +5V-net van de PCB, dat alleen via J2 (floppyvoeding / Atari) gevoed
wordt: de 5V-pin van de Otronic-clone is alleen een ingang. Bij voeding via USB alleen is de
buzzer dus stil.

**Veiligheid Shugart-uitgangen:** `bootloader_components/early_pins` zet de zes uitgangs-GPIO's
(ULN2003A-ingangen) al in de bootloader op LOW; `shugart_outputs_release()` doet dat opnieuw als
eerste stap in `app_main()`.

**Floppy images:** de firmware bevat geen images. De emulator laadt de image uit de
imagebibliotheek op de externe SPI-flash (`docs/FLASH_LAYOUT.md`); is die leeg, ontbreekt hij of is
hij onbruikbaar, dan zit er geen disk in drive B:. Een flash met het oude 20-slotformaat wordt
herkend maar niet gelezen: initialiseren met `POST /api/v1/storage/format` (of de knop op de
website). `images/RETROLOFT_TEST_720K.ST` is alleen een testbestand in de repository.
Commerciële images blijven lokaal (`.gitignore`).

Volledige pinmapping: `main/board_pins.h`.

## Welke USB-poort?

Gebruik **uitsluitend de USB-poort van de USB-naar-UART-brug** (op het board meestal
gemarkeerd als `COM` of `UART`). De andere poort (`USB`) is de native USB van de ESP32-S3 en
zit direct op GPIO19/GPIO20. Die poort mag **niet** worden aangesloten: de host trekt D-/D+
via 15 kΩ naar GND, waardoor de knop SW_NEXT (GPIO19) permanent "ingedrukt" leest.

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

Verwachte uitvoer: de opstartmelding van RadioFloppy met de imagebibliotheek, de actieve disk
en het IP-adres.
