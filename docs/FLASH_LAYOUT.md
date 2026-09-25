# RadioFloppy external flash layout — block-based image library

Floppy images live on the external SPI NOR flash U2, separate from the
ESP32-S3 firmware flash. No file system: the flash is divided into logical
blocks; block 0 holds the catalog and the settings, all other blocks hold
image data. An image is an ordered list of blocks, which do not have to be
contiguous. All constants are in `main/flash_layout.h`, the code in
`main/image_store.c` (host tests: `tests/host/test_image_store.c`).

Fitted chip: Infineon/Cypress **S25FL128L** (JEDEC `01 60 18`), 16 MiB,
4 KiB erase sectors, 64 KiB erase blocks, 256-byte pages, erased = `0xFF`.
Driven by the ESP-IDF generic chip driver in single SPI mode
(`main/ext_flash.c`). Nothing depends on this chip or on 16 MiB: the
capacity is detected at run time (`esp_flash_get_size`, from the JEDEC ID).

## Block geometry

Block numbers are stored as `uint8_t`, so there are at most **256 logical
blocks**: block 0 (metadata) plus at most **255 data blocks** (1…255). The
block size is the smallest power of two of at least 64 KiB for which the
capacity needs at most 256 blocks:

| Flash capacity | Block size | Data blocks | Blocks for 1.5 MiB |
| -------------: | ---------: | ----------: | -----------------: |
|          8 MiB |     64 KiB |         127 |                 24 |
|         16 MiB |     64 KiB |         255 |                 24 |
|         32 MiB |    128 KiB |         255 |                 12 |
|         64 MiB |    256 KiB |         255 |                  6 |
|        128 MiB |    512 KiB |         255 |                  3 |

Block *n* starts at `n × block_size`. A logical block is not an erase unit:
it is erased in 4 KiB sectors / 64 KiB erase blocks as needed, and only as
far as an image actually uses it.

The fitted 16 MiB chip: **64 KiB blocks, 255 data blocks** (blocks 1–255 =
`0x010000`–`0xFFFFFF`).

## Block 0

Whatever the block size, only the first 64 KiB of block 0 are used:

| Start      | End        | Size   | Use                 |
| ---------- | ---------- | ------ | ------------------- |
| `0x000000` | `0x005FFF` | 24 KiB | Catalog copy A      |
| `0x006000` | `0x00BFFF` | 24 KiB | Catalog copy B      |
| `0x00C000` | `0x00CFFF` | 4 KiB  | Settings copy A     |
| `0x00D000` | `0x00DFFF` | 4 KiB  | Settings copy B     |
| `0x00E000` | `0x00FFFF` | 8 KiB  | Reserved            |

## Catalog (one 24 KiB copy, little endian)

| Offset | Size     | Content                                               |
| ------ | -------- | ----------------------------------------------------- |
| 0      | 64       | Header                                                |
| 64     | 255 × 96 | Image records (at most one image per data block)      |
| 24 544 | 28       | Unused (`0xFF`)                                       |
| 24 572 | 4        | Commit word `0x21544D43` ("CMT!"), written **last**   |

Header:

| Offset | Type | Field                | Value                                         |
| ------ | ---- | -------------------- | --------------------------------------------- |
| 0      | u32  | magic                | `0x4C494652` ("RFIL")                         |
| 4      | u16  | version              | 2 (1 was the 20-slot catalog "RFSL")          |
| 6      | u16  | header_size          | 64                                            |
| 8      | u32  | generation           | +1 on every catalog write; highest valid wins |
| 12     | u32  | crc32                | CRC-32 over header + records, this field = 0  |
| 16     | u32  | capacity             | Flash size the catalog was made for           |
| 20     | u32  | block_size           | Logical block size                            |
| 24     | u16  | data_blocks          | Number of data blocks                         |
| 26     | u16  | record_size          | 96                                            |
| 28     | u16  | record_count         | 255                                           |
| 30     | u8   | max_blocks_per_image | 24                                            |
| 31     | u8   | reserved             | 0                                             |
| 32     | u32  | max_image_size       | 1 572 864 (1.5 MiB)                           |
| 36     | u16  | next_id              | Id for the next new image                     |
| 38     | 26   | reserved             | 0                                             |

Image record (96 bytes):

| Offset | Type      | Field          | Meaning                                                     |
| ------ | --------- | -------------- | ----------------------------------------------------------- |
| 0      | u16       | id             | Image id 1–65535, fixed for the life of the image           |
| 2      | u16       | sequence       | Display order; changing it never moves data                 |
| 4      | u8        | status         | `0xFF` unused record, `0x01` valid, `0x02` incomplete       |
| 5      | u8        | format         | `0x01` = `.ST`                                              |
| 6      | u8        | storage_format | `0x00` = RAW (the only one for now; reserved for compression) |
| 7      | u8        | block_count    | Blocks used (0 when incomplete)                             |
| 8      | u32       | original_size  | Image size in bytes                                         |
| 12     | u32       | stored_size    | Bytes in the blocks (RAW: = original_size)                  |
| 16     | u32       | crc32          | CRC-32 (IEEE, as zlib `crc32()`) of the image               |
| 20     | u8[24]    | blocks         | Block numbers in image byte order (unused entries 0)        |
| 44     | char[52]  | name           | Title, NUL terminated when shorter than 52 characters       |

Image byte *o* is in block `blocks[o / block_size]` at offset
`o % block_size`. `main/image_store.c` (`image_store_read`) is the only code
that turns image offsets into flash addresses; the MFM encoder gets the
whole image from PSRAM and knows nothing about blocks.

### Validation when loading

A catalog copy is used only if magic, version, sizes, CRC and commit word
are right, the geometry in the header equals the detected one, and every
record is consistent:

- status valid or incomplete (or unused); ids unique and not 0;
- valid: `1 ≤ original_size ≤ 1.5 MiB`, `storage_format` RAW,
  `stored_size == original_size`, `block_count` = the blocks that size
  needs, at most 24 and at most the blocks of 1.5 MiB at this block size;
- every block number in 1…`data_blocks`, and no block used twice (across
  all valid records);
- incomplete: `block_count` 0.

Otherwise the other copy is used; if neither is usable, nothing is written
(see *States* below).

### Updates

A catalog update never touches the copy in use: the new catalog
(generation + 1) is written to the other copy, read back and only then
given its commit word. A power cut during an update leaves the previous
generation in place.

**New image:** size ≤ 1.5 MiB → pick free blocks (lowest numbers first,
not necessarily contiguous; free = not in any valid record) → erase and
program only those blocks (the last one only as far as needed) → read the
image back through its block list and check the CRC-32 → catalog update
with the record *valid*. A power cut before that leaves no record; the
blocks count as free again.

**Delete:** catalog update without the record. The blocks are free at
once; they are erased only when reused.

**Replace** (keeps id and sequence): first check that the old blocks plus
the free blocks suffice (else nothing changes) → catalog update with the
old record *incomplete* and without blocks → use the old blocks first, then
free ones → erase, program, read back, CRC → catalog update with the record
*valid*. Replacing therefore works on a nearly full flash. If it fails, the
old image is lost, but the record stays *incomplete* and is never offered
as a disk.

**Order:** the image moves to a position and all sequences are renumbered
1…n (catalog update only).

### States

| State        | Condition                                                     | Action                          |
| ------------ | ------------------------------------------------------------- | ------------------------------- |
| `valid`      | A usable catalog copy                                         | —                               |
| `blank`      | Both catalog copies erased                                    | Empty catalog written at once   |
| `old_format` | 20-slot catalog ("RFSL" at `0x2000`/`0x3000`) or first catalog ("RFCT" at `0x0000`/`0x1000`) | Not read; `POST /api/v1/storage/format` |
| `invalid`    | Unknown/damaged data, or a catalog for another flash size     | Not touched; format on request only |

Formatting erases both catalog copies and writes an empty catalog
(generation 1) to copy A; image data is not erased (its blocks are free and
get erased when reused). There is no migration from the 20-slot format.

## Settings (one 4 KiB sector per copy, little endian)

Device settings (`main/settings.c`): same scheme as the catalog. A save
erases and writes the copy *not* in use (generation + 1), reads it back and
only then writes the commit word; the valid copy with the highest generation
wins. Until anything is saved, the firmware uses its menuconfig defaults.

| Offset | Type     | Field          | Meaning                                         |
| ------ | -------- | -------------- | ----------------------------------------------- |
| 0      | u32      | magic          | `0x54534652` ("RFST")                           |
| 4      | u16      | version        | 3 (2: without buzzer_off/last_image_id, those bytes 0; 1: 148 bytes, read as DS1) |
| 6      | u16      | size           | record size in bytes (152)                      |
| 8      | u32      | generation     | +1 on every save                                |
| 12     | u32      | crc32          | CRC-32 over the record with this field = 0      |
| 16     | char[33] | hostname       | NUL terminated                                  |
| 49     | char[33] | wifi_ssid      | NUL terminated, empty = no network              |
| 82     | char[65] | wifi_pass      | NUL terminated (plain text)                     |
| 147    | u8       | wifi_security  | 0 WPA2/WPA3, 1 WPA3 only, 2 WPA/WPA2, 3 open    |
| 148    | u8       | drive_select   | 0 = DS0 (drive A:), 1 = DS1 (drive B:)          |
| 149    | u8       | buzzer_off     | 0 = buzzer on (default), 1 = off                |
| 150    | u16      | last_image_id  | Image active at power-off (0 = none); written 5 s after the last disk change, only when it differs |
| 4092   | u32      | commit         | `0x21544D43` ("CMT!"), written last             |

The WiFi password is stored in plain text: anyone with the board in hand can
read the flash chip.
