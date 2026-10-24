# RadioFloppy external flash layout

The floppy images live on the **SD card**. The external SPI NOR flash U2
(separate from the ESP32-S3 firmware flash) only holds the device settings
and a **copy of the last disk** inserted from the card, so RadioFloppy can
also start without the card. No file system. All constants are in
`main/flash_layout.h`.

Fitted chip: Infineon/Cypress **S25FL128L** (JEDEC `01 60 18`), 16 MiB,
4 KiB erase sectors, 64 KiB erase blocks, 256-byte pages, erased = `0xFF`.
Driven by the ESP-IDF generic chip driver in single SPI mode
(`main/ext_flash.c`), on the SPI bus it shares with the SD card. Only the
first 3.1 MiB are used, so a 4 MiB chip is enough; a later board can take
a smaller one (8 MiB leaves room for a copy of a 3 MiB HD image as well).

## Map

| Start      | End        | Size    | Use                                          |
| ---------- | ---------- | ------- | -------------------------------------------- |
| `0x000000` | `0x00BFFF` | 48 KiB  | Unused (the old image library's catalogs)    |
| `0x00C000` | `0x00CFFF` | 4 KiB   | Settings copy A                              |
| `0x00D000` | `0x00DFFF` | 4 KiB   | Settings copy B                              |
| `0x00E000` | `0x00FFFF` | 8 KiB   | Unused                                       |
| `0x010000` | `0x010FFF` | 4 KiB   | Disk copy: header                            |
| `0x011000` | `0x01FFFF` | 60 KiB  | Unused                                       |
| `0x020000` | `0x31FFFF` | 3 MiB   | Disk copy: data (64 KiB aligned)             |
| `0x320000` | end        |         | Unused                                       |

**The old image library.** Earlier firmware kept an image library on this
flash (catalogs at `0x000000`–`0x00BFFF`, image data in 64 KiB blocks
from `0x010000`). At start-up the firmware looks for its catalog magics
(`RFIL`, `RFSL`, `RFCT` at `0x0000`, `0x1000`, `0x2000`, `0x3000` or
`0x6000`); when one is found, `0x000000`–`0x00BFFF` is erased
once (log: *Flash: old image library erased*). The old image data is not
read any more and is overwritten by disk copies.

## Disk copy (`main/disk_copy.c`)

One copy of the disk last inserted from the SD card: the file itself
(`.ST` or `.HFE`), byte for byte, up to 3 MiB. It is written by the
*remember* task in `main/disk_switch.c` about 5 s after the last disk
change, only when the copy in the flash is not already that file (same
path and CRC-32). The file is streamed from the card in 8 KiB pieces; the
SD card is locked per piece, so the API stays responsive (a 2 MB file
takes about 17 s).

The copy is also the **journal for writing** (see *Writing* in
`docs/API.md`): a save patches the changed 64 KiB blocks of the copy,
commits it as *pending* (newer than the file on the card), writes the
file on the card and then marks the copy *synced*.

Header (at `0x010000`, little endian):

| Offset | Type      | Field      | Meaning                                         |
| ------ | --------- | ---------- | ----------------------------------------------- |
| 0      | u32       | magic      | `0x59504352` ("RCPY")                           |
| 4      | u16       | version    | 3 (2 without pending/base_crc, 1 with its data at `0x011000`; neither is read) |
| 6      | u8        | hfe        | 1 = HFE file, 0 = .ST                           |
| 7      | u8        | pending    | 1 = written sectors not on the card yet (until the sync word) |
| 8      | u32       | size       | File size in bytes, 1 … 3 MiB                   |
| 12     | u32       | crc        | CRC-32 (IEEE, as zlib `crc32()`) of the data    |
| 16     | u32       | base_crc   | Pending: CRC-32 of the file on the card the changes belong to |
| 20     | char[201] | path       | The file, relative to the root directory, NUL terminated |
| 221    | 3         | reserved   | 0                                               |
| 224    | u32       | header_crc | CRC-32 of bytes 0–223                           |
| 4088   | u32       | sync       | `0x434E5953` ("SYNC"): a pending copy is on the card now (programmed later, no erase) |
| 4092   | u32       | commit     | `0x21544D43` ("CMT!"), written last             |

Writing a copy: erase the header sector (from now on there is no valid
copy) → write the data, erasing each 64 KiB block when the write enters it
(the last one only in the 4 KiB sectors it needs) → read all data back
and check its CRC-32 → write the header → write the commit word. A power
cut at any point leaves either no copy or a complete one.

Patching a copy (a save): erase the header sector → erase and write only
the changed 64 KiB blocks → read *all* data back and check the new
CRC-32 → header with `pending` = 1 and `base_crc` → commit word. After
the file on the card is written and checked: the sync word.

Start-up (`main/disk_image.c`):

- A **pending** copy is written to the card first, but only onto the file
  it belongs to (CRC-32 `base_crc`; or the new CRC already: then it is
  only marked synced). If the file was replaced or deleted, the copy is
  stored next to it as `<name> (changes).st`. Without a card it stays
  pending (and the disk is played from it, write-protected).
- The last disk (`last_sd` in the settings) is read from the card. Only
  when there is no card or it cannot be read, the copy is used, and only
  when its path matches `last_sd` and its CRC-32 (or, pending, its
  `base_crc`) matches `last_sd_crc`; its data is checked against the CRC
  again while it is read. A card without the file: the drive starts empty
  (the copy is not used).
- The *remember* task never overwrites a pending copy of another disk.

## Settings (one 4 KiB sector per copy, little endian)

Device settings (`main/settings.c`). A save erases and writes the copy
*not* in use (generation + 1), reads it back and only then writes the
commit word; the valid copy with the highest generation wins. Until
anything is saved, the firmware uses its menuconfig defaults.

| Offset | Type      | Field          | Meaning                                         |
| ------ | --------- | -------------- | ----------------------------------------------- |
| 0      | u32       | magic          | `0x54534652` ("RFST")                           |
| 4      | u16       | version        | 9 (older versions are still read, see below)    |
| 6      | u16       | size           | record size in bytes (468)                      |
| 8      | u32       | generation     | +1 on every save                                |
| 12     | u32       | crc32          | CRC-32 over the record with this field = 0      |
| 16     | char[33]  | hostname       | NUL terminated                                  |
| 49     | char[33]  | wifi_ssid      | NUL terminated, empty = no network              |
| 82     | char[65]  | wifi_pass      | NUL terminated (plain text)                     |
| 147    | u8        | wifi_security  | 0 WPA2/WPA3, 1 WPA3 only, 2 WPA/WPA2, 3 open    |
| 148    | u8        | drive_select   | 0 = DS0 (drive A:), 1 = DS1 (drive B:)          |
| 149    | u8        | buzzer_off     | 0 = buzzer on (default), 1 = off                |
| 150    | u16       | last_image_id  | Unused (the old library's last image)           |
| 152    | u8        | machine        | 0 = Atari 16-bit (default), 1 = Commodore Amiga, 2 = IBM PC / DOS (only 0 supported yet) |
| 153    | u8        | compress       | Unused (the old library's compression setting)  |
| 154    | u8        | leds           | 1 or 2 LEDs on J4 (default 1)                   |
| 155    | u8        | reserved       | 0                                               |
| 156    | char[101] | root_dir       | SD card root directory, NUL terminated, `""` = card root |
| 257    | 3         | reserved       | 0                                               |
| 260    | char[201] | last_sd        | Last disk from the SD card (below the root directory), `""` = none |
| 461    | 3         | reserved       | 0                                               |
| 464    | u32       | last_sd_crc    | CRC-32 of that file when it was inserted or last saved |
| 4092   | u32       | commit         | `0x21544D43` ("CMT!"), written last             |

Older versions (1–8, `version_size()` in `main/settings.c` has their
sizes) are still read, and written as version 9 at the next save; the
fields they lack get their defaults (DS1, buzzer on, Atari, 1 LED, no
root directory, no last disk).

`last_sd` and `last_sd_crc` are written about 5 s after the last disk
change, and only when they differ, so stepping through disks with the
buttons writes the flash once.

The WiFi password is stored in plain text: anyone with the board in hand can
read the flash chip.
