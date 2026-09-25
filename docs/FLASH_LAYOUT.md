# RadioFloppy external flash layout — slot store

Floppy images live on the external SPI NOR flash U2, separate from the
ESP32-S3 firmware flash. No file system: a 64 KiB metadata area and 20 fixed
image slots. All addresses and sizes are defined once, in
`main/flash_layout.h`.

Fitted chip: Infineon/Cypress **S25FL128L** (JEDEC `01 60 18`), 16 MiB =
16 777 216 bytes, 4 KiB erase sectors (`20h`), 64 KiB blocks (`D8h`),
256-byte pages (`02h`), erased = `0xFF`. Driven by the ESP-IDF generic chip
driver in single SPI mode (see `main/ext_flash.c`).

## Memory map

| Start      | End        | Size         | Use                                         |
| ---------- | ---------- | ------------ | ------------------------------------------- |
| `0x000000` | `0x000FFF` | 4 KiB        | Legacy v1 catalog copy A (read-only)        |
| `0x001000` | `0x001FFF` | 4 KiB        | Legacy v1 catalog copy B (read-only)        |
| `0x002000` | `0x002FFF` | 4 KiB        | Slot catalog copy A                         |
| `0x003000` | `0x003FFF` | 4 KiB        | Slot catalog copy B                         |
| `0x004000` | `0x00FFFF` | 48 KiB       | Reserved (settings, future metadata)        |
| `0x010000` | `0xFAFFFF` | 20 × 800 KiB | Image slots 1–20                            |
| `0xFB0000` | `0xFFFFFF` | 320 KiB      | Reserved, not used                          |

Slot *n* (index 0–19, shown as 1–20) starts at `0x010000 + n × 0x0C8000`:

| Slot | Start      | Slot | Start      | Slot | Start      | Slot | Start      |
| ---- | ---------- | ---- | ---------- | ---- | ---------- | ---- | ---------- |
| 1    | `0x010000` | 6    | `0x3F8000` | 11   | `0x7E0000` | 16   | `0xBC8000` |
| 2    | `0x0D8000` | 7    | `0x4C0000` | 12   | `0x8A8000` | 17   | `0xC90000` |
| 3    | `0x1A0000` | 8    | `0x588000` | 13   | `0x970000` | 18   | `0xD58000` |
| 4    | `0x268000` | 9    | `0x650000` | 14   | `0xA38000` | 19   | `0xE20000` |
| 5    | `0x330000` | 10   | `0x718000` | 15   | `0xB00000` | 20   | `0xEE8000` |

Each slot is 800 KiB = 819 200 bytes = 200 erase sectors and holds at most
one unmodified image of 1 byte … 800 KiB. Only the real image length is
part of the disk: the rest of the slot is never read, checksummed or used
for the geometry.

## Slot catalog (one 4 KiB sector per copy, little endian)

| Offset | Size    | Content                                              |
| ------ | ------- | ---------------------------------------------------- |
| 0      | 32      | Header                                               |
| 32     | 20 × 64 | Slot records, slot 1 first                           |
| 1312   | 2780    | Unused (`0xFF`)                                      |
| 4092   | 4       | Commit word `0x21544D43` ("CMT!"), written **last**  |

Header:

| Offset | Type | Field       | Value                                             |
| ------ | ---- | ----------- | ------------------------------------------------- |
| 0      | u32  | magic       | `0x4C534652` ("RFSL")                             |
| 4      | u16  | version     | 1                                                 |
| 6      | u16  | header_size | 32                                                |
| 8      | u32  | generation  | +1 on every catalog write; highest valid wins     |
| 12     | u16  | record_size | 64                                                |
| 14     | u16  | slot_count  | 20                                                |
| 16     | u32  | slot_base   | `0x010000`                                        |
| 20     | u32  | slot_size   | `0x0C8000`                                        |
| 24     | u32  | crc32       | CRC-32 over header + records, this field as 0     |
| 28     | 4    | pad         | `0xFF`                                            |

Slot record:

| Offset | Type     | Field  | Meaning                                              |
| ------ | -------- | ------ | ---------------------------------------------------- |
| 0      | char[40] | name   | title, first 40 characters; NUL terminated if shorter |
| 40     | u32      | size   | real image length in bytes                           |
| 44     | u32      | crc32  | CRC-32 (IEEE, as zlib `crc32()`) over `size` bytes   |
| 48     | u8       | format | 1 = `.ST`                                            |
| 49     | u8       | status | `0xFF` empty, 1 valid, 2 building, 3 deleted         |
| 50     | char[14] | name_ext | title continuation (characters 41-52), NUL terminated; `0xFF` = none (older records) |

Titles are at most 52 characters. A record written before `name_ext` existed has `0xFF` there
and reads as its 40-character `name`.

The start address is not stored: it follows from the slot number.

### Validation at start-up

1. Read both copies. A copy is valid when magic, version, header/record
   size, slot count, slot base, slot size, CRC-32 and commit word all match.
2. The valid copy with the highest generation is used.
3. No valid copy: if both catalog sectors are erased the store is **blank**
   (not initialised); otherwise it is **invalid/unknown** and nothing in the
   metadata area is ever written.
4. A slot is only used when its status is *valid*, its size is 1 … 800 KiB,
   its format is known and its name is NUL terminated; when loading, the
   CRC-32 of the `size` bytes must match.

### Writing the catalog

The current copy is never modified: the new catalog (generation + 1) is
written to the other sector — erase, program, read back and compare — and
only then does it get its commit word.

## Slot operations (`main/slot_store.h`)

| Operation     | What happens                                                         |
| ------------- | -------------------------------------------------------------------- |
| prepare       | slot must be empty/deleted/building; record → *building*; erase only `ceil(size / 4 KiB)` sectors from the slot start |
| write         | program data, bounds checked against the announced size              |
| commit        | read the whole image back, compare CRC-32, only then → *valid*       |
| delete        | record → *deleted* (catalog only; bytes erased when the slot is reused) |
| find free     | empty first, then deleted, then interrupted (*building*)             |
| load          | *valid* only; reads exactly `size` bytes and checks the CRC-32       |

An interrupted upload stays *building* and is never used. Replacing a *valid*
slot in place is refused (delete first); a power-fail-safe replace is not
implemented yet. Erases never leave the target slot: ESP-IDF only uses a
64 KiB block erase for blocks that lie completely inside the requested range.

## Legacy v1 catalog

The first external-flash firmware used a variable-length layout ("RFCT"
catalog at `0x000000` / `0x001000`, images from `0x010000`). The slot
firmware only reads it:

- If the slot store is blank and a legacy image named `DISK_EXTERNAL_IMAGE`
  exists, it is loaded through the legacy catalog and the log says
  `MIGRATION TO SLOTS PENDING`.
- With `DISK_MIGRATE_LEGACY 1` (`main/disk_image.h`) a blank slot store takes
  over every legacy image that already starts at a slot address and fits a
  slot, after checking its CRC-32. Only the slot catalog is written; image
  bytes and legacy sectors are not touched.

## Tests

`tests/host/run.sh` builds `main/slot_store.c` and `main/legacy_catalog.c`
against a RAM model of the NOR flash (program only clears bits, erase per
4 KiB sector, every erase recorded) and checks the layout bounds, blank /
invalid detection, upload, erase ranges, untouched neighbours and reserved
tail, CRC failure, interrupted upload, delete/reuse, A/B fallback and the
legacy migration.
