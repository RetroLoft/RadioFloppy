# RadioFloppy image store — format version 1

Floppy images are stored as raw files on the external SPI NOR flash (U2),
which is separate from the ESP32-S3 firmware flash. This is RadioFloppy's own
simple layout, not a standard file system.

Fitted chip: Infineon/Cypress **S25FL128L** (JEDEC `01 60 18`), 16 MiB,
4 KiB sectors, 64 KiB blocks, 256-byte pages, erased = `0xFF`.

## Address map

| Start      | End        | Use                                           |
| ---------- | ---------- | --------------------------------------------- |
| `0x000000` | `0x000FFF` | Catalog copy A (4 KiB)                        |
| `0x001000` | `0x001FFF` | Catalog copy B (4 KiB)                        |
| `0x002000` | `0x00FFFF` | Reserved (future metadata / settings)         |
| `0x010000` | end        | Images, variable length, 4 KiB aligned        |

## Catalog sector (4 KiB, little endian)

| Offset | Size    | Content                                                   |
| ------ | ------- | --------------------------------------------------------- |
| 0      | 32      | Header                                                    |
| 32     | 60 × 64 | Records (60 images max)                                   |
| 3872   | 220     | Unused (`0xFF`)                                           |
| 4092   | 4       | Commit word `0x21544D43` ("CMT!"), written **last**       |

Header:

| Offset | Type   | Field          | Value / meaning                                  |
| ------ | ------ | -------------- | ------------------------------------------------ |
| 0      | u32    | magic          | `0x54434652` ("RFCT")                            |
| 4      | u16    | version        | 1                                                |
| 6      | u16    | header_size    | 32                                               |
| 8      | u32    | generation     | incremented on every catalog write; highest wins |
| 12     | u16    | record_size    | 64                                               |
| 14     | u16    | record_count   | 60                                               |
| 16     | u32    | crc32          | CRC-32 (IEEE) over header + records, this field 0 |
| 20     | 12     | pad            | `0xFF`                                           |

Record (64 bytes):

| Offset | Type     | Field    | Meaning                                              |
| ------ | -------- | -------- | ---------------------------------------------------- |
| 0      | char[40] | name     | NUL terminated, e.g. `Crystal Castles`               |
| 40     | u32      | start    | flash address, multiple of 4096                      |
| 44     | u32      | size     | real image length in bytes (padding is not data)     |
| 48     | u32      | reserved | allocated length, `size` rounded up to 4096          |
| 52     | u32      | crc32    | CRC-32 (IEEE, as zlib `crc32()`) over `size` bytes   |
| 56     | u8       | format   | 1 = `.ST`                                            |
| 57     | u8       | status   | `0xFF` unused, 1 valid, 2 building, 3 deleted        |
| 58     | 6        | pad      | `0xFF`                                               |

A catalog copy is valid when magic, version, sizes, CRC and commit word are
all correct. At start-up the valid copy with the highest generation is used.

## Updating the catalog

The current copy is never modified. A new catalog (generation + 1) is written
to the *other* sector: erase that sector, program header + records, read back
and compare, and only then program the commit word. A power loss at any point
leaves the previous catalog in force.

## Adding an image

1. Allocate after the end of every non-unused record (valid, building or
   deleted), starting at `0x010000`; reserved length = size rounded up to 4 KiB.
2. Without a valid catalog the catalog sectors and the image area must read
   as blank (`0xFF`); unknown data is never overwritten.
3. Write a catalog with the record in state *building*.
4. Erase only the image's own sectors (64 KiB block erase where aligned).
5. Program, then read back and compare every byte.
6. Write a catalog with the record in state *valid*.

## Replacing / deleting (planned)

Deleting marks the record *deleted*; its area is erased only when the space
is needed again. Replacing writes and verifies the new copy first and then
switches the catalog to it, when there is enough free space.

Capacity with this layout (16 MiB − 64 KiB): 45 × 360 KiB or 22 × 720 KiB
images (limited by space), 60 records at most.
