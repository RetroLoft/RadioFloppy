# RadioFloppy HTTP API — version 1

RadioFloppy is a WiFi floppy drive emulator for the Atari ST. It replaces a
real floppy drive (here: external drive **B:**) and plays `.ST` disk images
to the Atari. This API lets you put disk images on its SD card and choose
which one the Atari sees — from a script, a web page or any HTTP client.

- [Web interface](#web-interface)
- [Quick start](#quick-start)
- [Concepts](#concepts)
- [Authentication](#authentication)
- [Endpoint reference](#endpoint-reference)
- [Uploading images](#uploading-images)
- [Errors](#errors)
- [Client examples](#client-examples)
- [Limits and behaviour](#limits-and-behaviour)
- [Device configuration](#device-configuration)
- [WiFi setup mode](#wifi-setup-mode)

## Web interface

Open `http://<device>/` in a browser (PC or phone). The page is served by
the device itself, uses only this API and needs no internet access:

- **Current Floppy** — the disk the Atari sees now, its origin (the SD
  card, its copy in the flash, or a temporary PSRAM image), whether the
  Atari can write to it (and whether its changes are saved), and a
  warning when its file was replaced or deleted since.
- **Load a floppy temporarily** — upload a `.ST` file straight into the
  drive (PSRAM, not stored).
- **New disk** — a name and *Create*: an empty, formatted 720 KiB Atari
  disk as a file in the folder that is open in the SD card block, and
  put into the drive at once (only when the computer is set to Atari
  16-bit).
- **SD card** — your floppy images: the files below the root directory,
  with a breadcrumb, folders first, then the files with their size;
  **Insert** (.ST and .HFE), a lock per .ST file (read-only or writable
  for the Atari), **Upload here…** (one or more files), **New folder**
  and a delete button per file and folder.
- **Admin access** — only shown when the device has a token; it is kept only in the open page.
- **Settings** (gear icon, top right, `/#settings`) — connected computer, host name, floppy drive (A:/B:), buzzer on/off, front panel LEDs, SD card root directory, WiFi status,
  firmware version and a *Check for updates* button (not functional yet).

The page polls `GET /current` every 3 s and the SD card folder every 15 s
(and after changes), one request at a time, and pauses while it uploads
or loads. Source: `web/index.html`, embedded in the firmware.

## Front panel buttons

**Left** = previous disk, **right** = next disk: the image files (.ST and
.HFE) of the folder of the disk inserted last (through the web page or
the API), in the order the web page shows them (case-insensitive, numbers
by value). At the first file the left button does nothing, at the last
one the right button. A press gives a click on the buzzer; the switch
follows the same rules as `PUT /current`.

Both buttons held **3 s**: the display shows the host name and IP address
for 10 s. Held on to **10 s**: RadioFloppy restarts in
[WiFi setup mode](#wifi-setup-mode). In setup mode, both buttons held 3 s
leave it without changes.

## Quick start

```sh
DEV=192.168.178.35            # the device address (see "Device configuration")
# What is the device doing?
curl -s http://$DEV/api/v1/status

# Which images are on the SD card (root directory)?
curl -s http://$DEV/api/v1/sd/files

# Insert one of them
curl -s -X PUT -H "Content-Type: application/json" \
     -d '{"sd_path":"Bolo (1987)(Application Systems Heidelberg)(de-en).st"}' http://$DEV/api/v1/current

# Put a game on the drive right now (temporary, not stored)
F="Crystal Castles.st"
URL=$(curl -s -H "Content-Type: application/json" \
      -d "{\"filename\":\"$F\",\"size\":$(stat -c %s "$F"),\"destination\":\"psram\"}" \
      http://$DEV/api/v1/uploads | sed -n 's/.*"upload_url":"\([^"]*\)".*/\1/p')
curl -s -X PUT -H "Content-Type: application/octet-stream" --data-binary @"$F" http://$DEV$URL
```

(With a token configured, add `-H "Authorization: Bearer $TOKEN"`.)

## Concepts

### Base URL and formats

All endpoints live under `http://<device>/api/v1/`. Requests and responses
use JSON (`Content-Type: application/json`), except the image data itself,
which is sent as raw bytes (`application/octet-stream`) — never base64.
Plain HTTP on port 80; the API is meant for a trusted local network.

`v1` is the API version. Fields may be *added* to responses within v1;
clients should ignore fields they do not know. Incompatible changes get a
new version path.

### The active disk

At any moment the drive holds at most one disk: the **active disk**. It is
what the Atari reads from drive B:. It can come from

- **the SD card** — a `.ST` or `.HFE` file below the root directory, or
- **PSRAM** — one temporary `.ST` image that is lost when the device restarts.

`GET /api/v1/current` tells you which one it is. A disk from the SD card
is remembered: after a restart it is read from the card again. A few
seconds after inserting it, RadioFloppy also keeps a **copy in the
flash**; when the card is missing or cannot be read at start-up, the disk
is played from that copy. A PSRAM image is gone after a restart. A disk is
always prepared completely in PSRAM before the Atari sees it.

### My floppy images: the SD card

Your images are plain files on the SD card, below the **root directory**
(setting `root_dir`, e.g. `My Floppies`), in any folders you like. You
can fill the card on a PC as well, or through the API (see
[SD card files](#sd-card-files)). The flash of RadioFloppy holds no
images; only the settings and the copy of the last disk
(`docs/FLASH_LAYOUT.md`).

### Supported images

Raw `.ST` images (sector dumps without header) with

| Property          | Supported                                      |
| ----------------- | ---------------------------------------------- |
| Cylinders         | 79–84 (with 79, track 79 reads as unformatted) |
| Sectors per track | 9, 10 or 11 (512 bytes)                        |
| Sides             | 1 or 2                                         |
| File size         | up to 946 176 bytes (84/2/11)                  |

Examples: 360 KiB (80/1/9), 400 KiB (80/1/10), 410 KiB (82/1/10, e.g.
Nebulus), 440 KiB (80/1/11), 720 KiB (80/2/9), 800 KiB (80/2/10), 820 KiB
(82/2/10), 880 KiB (80/2/11).

The geometry follows from the file size alone (no supported size fits two
geometries). The boot sector (BPB) is only checked and reported in the
device log: many game disks have a custom or stale one, and some dumps are
shorter than their file system — the missing tracks then read as
unformatted, as they would from the real disk. Other geometries (8 sectors, 40 or 70
cylinders, HD with 18 sectors, …) are recognised but not supported
(`UNSUPPORTED_GEOMETRY`); anything above 1.5 MiB (1 572 864 bytes) is
refused as `IMAGE_TOO_LARGE`. `.MSA` is not supported. The Atari can write to
an .ST file on the SD card unless the file is read-only (see
[Writing](#writing)).

**HFE** (HxC Floppy Emulator) files (`.hfe`) are played from the SD card;
they cannot be loaded temporarily. An upload to the card is checked as it
arrives: HFEv1 and HFEv3 by their header; ISO MFM, 250 kbit/s, 1 or 2
sides, at most 84 cylinders, single step; the track table before the
track data and the tracks in ascending order (as HxC tools write them;
other layouts are refused with `UNSUPPORTED_FORMAT`); and that the decoded
tracks fit the drive's track buffer (else `IMAGE_TOO_LARGE`), so every
HFE file on the card that came through the API can be inserted. HFEv2
("HXCPICFE" revision 1) is refused: convert it to HFEv1 or HFEv3. An HFE
file on the card may be up to 8 MiB (the track buffer decides); its copy
in the flash only up to 3 MiB. Inserting reads the file straight into the
track buffer (a 2 MB file takes about 5 s).

HFE images are played in their own timing: every track at its own cell
times, with the index pulse on the real start of the revolution. HFEv3
timing changes (`BITRATE`) are played; files made from flux dumps, with
thousands of tiny speed changes per track, are smoothed into zones that
keep the exact duration of every revolution. Weak bits (`RAND`) are
not yet played as random data.

The track layout follows FlashFloppy: GAP3 84 (9 sectors), 30 (10
sectors) or 3 with interleave 2 (11 sectors). An 11-sector track is
longer than a standard track and is played with slightly shorter bitcells
(1.945 µs instead of 2 µs), so a revolution still takes 200 ms.

### Disk changes

When the active disk changes, the drive behaves like a real drive whose
door is opened and closed: for 0.7 s it shows no disk. The Atari (TOS)
notices this and reads the new disk's directory the next time you open
drive B:.

A disk can be changed while the Atari uses drive B:, as with a real drive
(many games keep the drive selected all the time). The device first waits
up to 0.3 s for the Atari to finish its access. It never changes a disk the
Atari is writing to: see [DRIVE_BUSY](#drive_busy).

## Authentication

By default there is **no token**: everyone on the local network can use the
whole API and the web page. RadioFloppy is meant for a home network.

Changing calls must send the right `Content-Type` (`application/json`, or
`application/octet-stream` for image data); anything else gets
`415 UNSUPPORTED_MEDIA_TYPE`. Browsers can only send such requests from
another web site after a CORS check, which RadioFloppy never allows, so a
foreign web page cannot change disks through your browser.

On a shared network a token can be configured (menuconfig →
RadioFloppy → *Optional API token*). Then every changing call needs

```
Authorization: Bearer <token>
```

(`X-API-Token: <token>` is accepted as well), a missing or wrong token gives
`401 UNAUTHORIZED`, `GET /status` reports `"auth_required": true`, and the
web page asks for the token.

## Endpoint reference

| Method and path                 | Token | Purpose (token only if one is configured)   |
| ------------------------------- | ----- | ------------------------------------------- |
| `GET /api/v1/status`            |       | Device, WiFi, SD card and drive status      |
| `GET /api/v1/current`           |       | The active disk                             |
| `PUT /api/v1/current`           | (yes) | Insert a file from the SD card              |
| `POST /api/v1/uploads`          | (yes) | Start an upload (PSRAM or SD card)          |
| `PUT /api/v1/uploads/{id}/data` | (yes) | Send the image bytes of an upload           |
| `GET /api/v1/uploads/{id}`      |       | State of an upload                          |
| `GET /api/v1/sd/files?path=`    |       | Contents of a folder on the SD card         |
| `DELETE /api/v1/sd/files?path=` | (yes) | Delete a file on the SD card                |
| `PUT /api/v1/sd/files?path=`    | (yes) | Make a file read-only or writable           |
| `POST /api/v1/sd/folders`       | (yes) | Create a folder on the SD card              |
| `DELETE /api/v1/sd/folders?path=` | (yes) | Delete a folder (with contents: confirm)  |
| `POST /api/v1/sd/disks`         | (yes) | Create a new, empty, formatted disk file    |
| `GET /api/v1/settings`          |       | Device settings, WiFi and firmware          |
| `PUT /api/v1/settings`          | (yes) | Change settings                             |
| `POST /api/v1/system/restart`   | (yes) | Restart the device                          |
| `GET /api/v1/system/update`     |       | Check for firmware updates (placeholder)    |

---

### `GET /api/v1/status`

Overall state of the device.

```json
{
  "device": "RadioFloppy",
  "api": "v1",
  "firmware": "1.0.0",
  "hostname": "RadioFloppy",
  "wifi": { "connected": true, "ssid": "MyNetwork", "ip": "192.168.178.62", "rssi": -38 },
  "sd": { "present": true, "size_mb": 29818, "root_exists": true },
  "machine": { "id": "ATARI", "name": "Atari 16-bit", "supported": true },
  "drive": { "select_line": "DS1", "armed": true, "selected": false, "motor": false, "cylinder": 0 },
  "current": { "inserted": true, "source": "sd", "sd_path": "C/Crystal Castles.st",
               "sd_copy": false, "image_changed_since": false,
               "name": "Crystal Castles", "size": 368640, "crc32": "42ce7eed", "sides": 1 },
  "psram_free": 3928064,
  "auth_required": false,
  "psram_image": { "name": "Retroloft test", "size": 737280, "crc32": "c413f8cc", "sides": 2 },
  "upload": { "upload_id": "25", "state": "done", "...": "..." }
}
```

| Field                       | Meaning                                                           |
| --------------------------- | ----------------------------------------------------------------- |
| `wifi.rssi`                 | Signal strength in dBm                                            |
| `sd`                        | `present` (a card is mounted), with a card `size_mb` and `root_exists` (the root directory is there) |
| `machine`                   | Machine profile in use; `supported: false` = floppy emulation off  |
| `drive.select_line`         | Which drive-select line the emulator answers to (`DS1` = drive B:) |
| `drive.armed`               | The emulator has seen the Atari powered on and answers to it      |
| `drive.selected`            | The Atari is using our drive right now                            |
| `drive.motor`               | The floppy motor line is on                                       |
| `drive.cylinder`            | Current head position (0–83)                                      |
| `current`                   | Same object as [`GET /current`](#get-apiv1current)                |
| `psram_free`                | Free PSRAM in bytes                                               |
| `auth_required`             | A token is configured: changing calls need it                     |
| `psram_image`               | The kept temporary PSRAM image, if any                            |
| `upload`                    | The latest upload, if any (same object as [`GET /uploads/{id}`](#get-apiv1uploadsid)) |

---

### `GET /api/v1/current`

The active disk.

```json
{ "inserted": true, "source": "sd", "sd_path": "N/Nebulus.st", "sd_copy": false,
  "image_changed_since": false, "name": "Nebulus", "size": 419840, "crc32": "40881c6a",
  "sides": 1, "cylinders": 82, "sectors": 10,
  "write": { "state": "read_only", "writable": false, "reason": "...", "...": "..." } }
```

| Field                 | Meaning                                                          |
| --------------------- | ---------------------------------------------------------------- |
| `inserted`            | `false`: no disk in the drive (then only `source: "none"`)       |
| `source`              | `sd` (a file on the SD card), `psram` (temporary) or `none`      |
| `sd_path`             | Only for `sd`: the file, relative to the root directory           |
| `sd_copy`             | Only for `sd`: `true` when it was read from its copy in the flash (the card was not available at start-up) |
| `image_changed_since` | Only for `sd`: the file was replaced or deleted since; the drive still plays the disk as it was when inserted |
| `name`, `size`, `crc32` | The disk image (`name`: the file name without extension)      |
| `sides`, `cylinders`, `sectors` | Geometry of an .ST image: 1 or 2 sides, 79–84 cylinders, 9–11 sectors per track |
| `write`               | Write state of the inserted disk, see [Writing](#writing)        |

---

### `PUT /api/v1/current`

Insert a file from the SD card (relative to the root directory, see
[SD card files](#sd-card-files)). Token required.

```json
{ "sd_path": "Bolo (1987)(Application Systems Heidelberg)(de-en).st" }
```

`.ST` and `.HFE` files can be inserted; an .ST file is writable for the
Atari unless it is read-only, an HFE file is always write-protected
(see [Writing](#writing)). The disk is remembered: after a restart it is read from the card
again. About 5 s after the last disk change RadioFloppy also keeps a
**copy in the flash** (one copy, of the last disk only; up to 3 MiB;
written only when it differs from the copy already there). When the card
is missing or cannot be read at start-up, the disk is played from that
copy (`sd_copy: true`); when the card is there but the file is gone, the
drive starts empty.

The folder of the file becomes the folder of the
[front panel buttons](#front-panel-buttons).

Response `200`: the new [`current`](#get-apiv1current) object. An .ST
image is read and checked at once; an HFE file is decoded into the track
buffer while it is read (a 2 MB file takes about 5 s).

| Error                  | When                                             |
| ---------------------- | ------------------------------------------------ |
| `400 INVALID_REQUEST`  | no `sd_path`                                     |
| `400 INVALID_PATH`     | `sd_path` breaks the path rules                  |
| `404 FILE_NOT_FOUND`   | No such file on the SD card                      |
| `422 UNSUPPORTED_FORMAT` | Not an `.ST` or `.HFE` file                    |
| `422 INVALID_IMAGE` / `UNSUPPORTED_GEOMETRY` | The file is not a usable image |
| `503 SD_NOT_PRESENT`   | No SD card                                       |
| `409 DRIVE_BUSY`       | The Atari kept writing to drive B:; nothing changed |
| `409 MACHINE_NOT_SUPPORTED` | Floppy emulation is off for this computer   |
| `500 SD_ERROR`         | The card could not be read                       |
| `507 INSUFFICIENT_MEMORY` | No memory to prepare the tracks              |

---

### `POST /api/v1/uploads`

Start an upload. Token required. See [Uploading images](#uploading-images).

Request fields:

| Field         | Type    | Required | Meaning                                                   |
| ------------- | ------- | -------- | --------------------------------------------------------- |
| `filename`    | string  | yes      | Original file name. `psram`: becomes the title (TOSEC tags removed, max. 52 characters). `sd`: the file name on the card. |
| `size`        | number  | yes      | Exact size of the file in bytes                           |
| `destination` | string  | yes      | `"psram"` (play now, temporary) or `"sd"` (a file on the SD card, see [SD card files](#sd-card-files)) |
| `activate`    | boolean | no       | `"psram"`: always activated, `false` is refused. `"sd"`: must be `false` (insert the file with `PUT /current` afterwards). |
| `crc32`       | string  | no       | CRC-32 of the file as hex, e.g. `"42ce7eed"`. If given, the received data must match. |

A PSRAM upload takes `.ST` images only (`422 UNSUPPORTED_FORMAT` for
`.hfe`: put an HFE file on the SD card instead).

Response `201 Created`: an [upload object](#get-apiv1uploadsid) in state
`ready`, with the `upload_url` to send the data to:

```json
{ "upload_id": "26", "state": "ready", "name": "Crystal Castles", "size": 368640,
  "received": 0, "destination": "psram", "activate": true,
  "upload_url": "/api/v1/uploads/26/data" }
```

The size and geometry are checked here already, so a client learns about
most problems before sending any data.

| Error                        | When                                               |
| ---------------------------- | -------------------------------------------------- |
| `400 INVALID_REQUEST`        | Missing/wrong fields, `psram` with `activate: false`, `sd` with `activate: true` |
| `409 UPLOAD_BUSY`            | Another upload is still in progress                |
| `413 IMAGE_TOO_LARGE`        | `size` above the limit                             |
| `422 INVALID_IMAGE`          | `size` cannot be a floppy image                    |
| `422 UNSUPPORTED_GEOMETRY`   | `size` is a `.ST` geometry that is not supported   |
| `422 UNSUPPORTED_FORMAT`     | Not a supported file type for this destination     |
| `507 INSUFFICIENT_MEMORY`    | Not enough memory for the upload                   |

---

### `PUT /api/v1/uploads/{id}/data`

Send the image bytes. Token required. Body: the complete file, nothing
else; `Content-Type: application/octet-stream`; `Content-Length` must equal
the announced `size`. Data can be sent only once per upload.

The request returns when everything is finished — received, checked,
stored and/or activated — typically a few seconds. The response is the
final [upload object](#get-apiv1uploadsid):

```json
{ "upload_id": "26", "state": "done", "name": "Crystal Castles", "size": 368640,
  "received": 368640, "destination": "psram", "activate": true,
  "crc32": "42ce7eed", "active": true }
```

On failure the HTTP status matches the error, and the upload object has
`"state": "failed"` and an `error`:

```json
{ "upload_id": "27", "state": "failed", "name": "Crystal Castles", "size": 368640,
  "received": 100000, "destination": "psram", "activate": true,
  "error": { "code": "UPLOAD_INCOMPLETE", "message": "received 100000 of 368640 bytes" } }
```

| Error                      | When                                                        |
| -------------------------- | ----------------------------------------------------------- |
| `400 UPLOAD_INCOMPLETE`    | `Content-Length` ≠ `size`, or the connection dropped        |
| `404 UPLOAD_NOT_FOUND`     | Unknown or superseded upload id                             |
| `409 UPLOAD_STATE`         | Data was already sent for this upload                       |
| `409 DRIVE_BUSY`           | `psram`: not activated, nothing kept — see below            |
| `422 INVALID_IMAGE` / `UNSUPPORTED_GEOMETRY` | The content is not a supported image      |
| `422 CHECKSUM_MISMATCH`    | Data does not match the given `crc32`                       |
| `422 UNSUPPORTED_FORMAT`   | A recognised but unplayable format (e.g. HFEv2)             |
| `500 SD_ERROR`             | `sd`: writing or reading back the file failed (nothing left behind) |
| `500 PREPARE_FAILED`       | The disk could not be prepared for the drive                |
| `507 INSUFFICIENT_MEMORY`  | Not enough memory                                           |

---

### `GET /api/v1/uploads/{id}`

State of an upload. Only the most recent upload is kept; older ids give
`404 UPLOAD_NOT_FOUND`.

| Field            | Present            | Meaning                                         |
| ---------------- | ------------------ | ----------------------------------------------- |
| `upload_id`      | always             | Id (a string)                                   |
| `state`          | always             | `ready`, `receiving`, `processing`, `done`, `failed` |
| `name`           | always             | Title derived from `filename`                   |
| `size`, `received` | always           | Announced and received byte count               |
| `destination`    | always             | `psram` or `sd`                                 |
| `path`, `file`   | `sd`               | Folder (below the root directory) and file name |
| `activate`       | always             | Whether it will be / was activated              |
| `upload_url`     | state `ready`      | Where to `PUT` the data                         |
| `crc32`          | state `done`       | CRC-32 of the received image                    |
| `active`         | state `done`       | It is now the active disk                       |
| `error`          | state `failed`     | `{ "code": ..., "message": ... }`               |

### SD card files

The SD card holds the images as plain files below the **root directory**
(setting `root_dir`, e.g. `My Floppies`). Every path in these calls is
**relative to that root directory**: `""` is the root directory itself,
`L` a folder in it, `L/Larry.st` a file in that folder.

Paths are checked strictly and only ever used below the root directory:
names separated by single `/`, at most 200 characters in all; a name is
1–120 printable ASCII characters except `\ / : * ? " < > |`, not starting
with `.` or a space (so no `.`, `..` or hidden names) and not ending with
`.` or a space. Anything else: `400 INVALID_PATH`. In a query string the
path is URL-encoded (`%20` or `+` for a space). Hidden files (starting
with `.`) are not listed. No card: `503 SD_NOT_PRESENT`.

`GET /api/v1/sd/files?path=L` — the folder's contents (at most 400
entries, `truncated` when there are more), not sorted:

```json
{ "root_dir": "My Floppies", "path": "L", "truncated": false,
  "entries": [ { "name": "Leisure Suit Larry (1987)(Sierra).st", "type": "file", "size": 737280 },
               { "name": "Disk 2", "type": "folder" } ] }
```

`404 FOLDER_NOT_FOUND` when it does not exist.

`POST /api/v1/sd/folders` `{"path": "L"}` — creates the folder, and every
folder on the way (`{"path": "Games/L"}` also creates `Games`).
`201 Created`; `409 FOLDER_EXISTS` when a folder or file of that name
is already there.

`DELETE /api/v1/sd/folders?path=L` — deletes an empty folder
(`409 FOLDER_NOT_EMPTY` otherwise). With its contents:
`?path=L&recursive=true&confirm=L` — `confirm` must repeat the path
(`400 CONFIRMATION_REQUIRED` otherwise): everything in it is lost. The
root directory itself cannot be deleted.

`DELETE /api/v1/sd/files?path=L/Larry.st` — deletes a file, also a
read-only one (`404 FILE_NOT_FOUND`). Deleting the file of the active disk does not
take the disk out of the drive (`image_changed_since` becomes `true`).

`PUT /api/v1/sd/files?path=L/Larry.st` `{"read_only": true}` — sets or
clears the file's FAT read-only attribute (the same attribute a PC
shows): a read-only file is write-protected for the Atari, like the tab
on a real floppy, and is not overwritten by an upload
(`409 FILE_READ_ONLY`). Takes effect at once, also for the inserted
disk. Response `200 {"path": ..., "read_only": true}`. Errors:
`400 INVALID_REQUEST` (no boolean), `400 INVALID_PATH`,
`404 FILE_NOT_FOUND`, `409 UNSAVED_CHANGES` (making the inserted disk
read-only while the Atari's changes are not saved yet; retry in a few
seconds). Listings show `"read_only"` for every file.

`POST /api/v1/sd/disks` `{"path": "L", "name": "Save games"}` — creates
a new, empty, formatted disk as a file in folder `path` (default `""`).
`.st` is added to `name` unless it ends with it. The disk follows the
**configured** computer (`machine` in the settings). **Atari 16-bit**
(the only one for now): 720 KiB = 737 280 bytes, 80 tracks × 2 sides ×
9 sectors × 512 bytes, formatted as TOS does it (boot sector with BPB, not
executable, random serial number; two empty FATs; empty root directory
for 112 entries; 711 free clusters of 1 KiB). It is written like an
upload (temporary file, read back, then renamed) and not inserted (the
web page inserts it right after).
Response `201 {"path": "L/Save games.st", "size": 737280}`. Errors:
`422 INVALID_NAME`, `400 INVALID_PATH`, `404 FOLDER_NOT_FOUND`,
`409 FILE_EXISTS`, `409 MACHINE_NOT_SUPPORTED`, `503 SD_NOT_PRESENT`,
`500 SD_ERROR`, `507 INSUFFICIENT_MEMORY`.

**Uploading to the SD card:** `POST /api/v1/uploads` with
`"destination": "sd"`, then the data as usual:

| Field       | Meaning                                                              |
| ----------- | -------------------------------------------------------------------- |
| `filename`  | The file name on the card (rules above, `422 INVALID_NAME`); `.st` or `.hfe` only (`422 UNSUPPORTED_FORMAT`) |
| `size`      | Exact size: a supported .ST size, or an HFE file of at most 8 MiB    |
| `path`      | Folder below the root directory (default `""`); must exist (`404 FOLDER_NOT_FOUND`) |
| `overwrite` | `true`: replace a file of that name, unless it is read-only (`409 FILE_READ_ONLY`; default `false`: `409 FILE_EXISTS`) |
| `crc32`     | Optional, as for PSRAM uploads                                       |
| `activate`  | Must be `false`: insert the file with `PUT /current` afterwards      |

Only images that can be played are stored (.ST geometry, HFE format and
track buffer fit; an HFE file named `.st` or the other way round is
refused). The data is written straight to a hidden temporary file in the
target folder while it arrives; after the checks it is read back and
compared (CRC-32), and only then it gets its name (replacing an older file
only with `overwrite`). An interrupted or refused upload leaves nothing
behind.

---

### `GET /api/v1/settings`

```json
{
  "hostname": "RadioFloppy",
  "drive_select": "DS1",
  "drive_select_active": "DS1",
  "buzzer": true,
  "leds": 1,
  "root_dir": "My Floppies",
  "sd": { "present": true, "size_mb": 29818, "root_exists": true },
  "last_sd_path": "C/Crystal Castles.st",
  "machine": "ATARI",
  "machine_active": "ATARI",
  "machine_supported": true,
  "machines": [ { "id": "ATARI", "name": "Atari 16-bit", "supported": true },
                { "id": "AMIGA", "name": "Commodore Amiga", "supported": false },
                { "id": "DOS", "name": "IBM PC / DOS", "supported": false } ],
  "restart_required": false,
  "wifi": { "ssid": "MyNetwork", "security": "wpa2", "connected": true,
            "ip": "192.168.178.62", "rssi": -38, "mac": "DC:DA:0C:12:34:56" },
  "firmware": { "version": "1.0.0", "idf": "v5.5.5" }
}
```

The WiFi password is never returned. `restart_required` is `true` when a
saved setting only takes effect after a restart (e.g. a new host name).

### `PUT /api/v1/settings`

Body (`Content-Type: application/json`), every field optional:

```json
{ "hostname": "Atari-B", "drive_select": "DS1", "buzzer": false, "machine": "ATARI" }
```

- `hostname`: 1–32 letters, digits or `-`, not starting or ending with `-`
  (else `422 INVALID_HOSTNAME`). Used for DHCP and for the name of the
  setup network. Takes effect after a restart.
- `drive_select`: `"DS1"` = drive **B:** (default, external second drive)
  or `"DS0"` = drive **A:** (only when the internal drive is disconnected);
  else `422 INVALID_DRIVE_SELECT`. Like the drive-select jumper of a real
  drive. Takes effect after a restart; `drive_select_active` in the answer
  is the line in use until then.
- `buzzer`: `true`/`false` — clicks for head steps and button presses.
  Takes effect at once.
- `leds`: `1` or `2` (default `1`) — how many LEDs are fitted on J4.
  The LED on J4 pin 3 always shows disk activity (on while the drive is
  selected and its motor runs). The WiFi status (blinking while
  connecting, on when no connection can be made, off when connected, fast
  blinking in setup mode) goes to J4 pin 1 with `2`, and with `1` to the
  ESP32 board's own RGB LED (blue). Takes effect at once.
- `root_dir`: the folder on the SD card where RadioFloppy works, e.g.
  `"My Floppies"` or `"Retro/My Floppies"`; `""` (default) is the root of
  the card. Folder names separated by `/`, at most 100 characters;
  letters, digits, space and `- _ . ( ) & + , !` only; no `.`/`..`, no `/`
  at the start or end, no name starting with `.` or a space or ending with
  `.` or a space; else `422 INVALID_ROOT_DIR`. It is always used below the
  card's mount point. The folder is created at once when a card is in
  (`500 SD_ERROR` if that fails: the setting is then not changed), else
  as soon as a card is inserted.
- `sd` (read only): `present` (a card is mounted), and with a card
  `size_mb` and `root_exists`. A card inserted later is found within
  about 10 s.
- `machine`: the connected computer, `"ATARI"` (Atari 16-bit, default),
  `"AMIGA"` (Commodore Amiga) or `"DOS"` (IBM PC / DOS); else
  `422 INVALID_MACHINE`. Takes effect after a restart; `machine_active` is
  the profile in use until then. **Only `ATARI` is supported for now.** With
  another profile RadioFloppy starts without floppy emulation: no drive
  interrupts, no flux stream, all floppy outputs released; the web page,
  the API, the SD card and WiFi setup keep working, activating a disk
  gives `409 MACHINE_NOT_SUPPORTED`, and `GET /status` reports
  `"machine": {"supported": false}`. Choose `ATARI` again and restart to
  get the drive back.

`last_sd_path` (read only) is the SD card file that is inserted at the
next start-up (`""`: none). It is stored about 5 s after the last disk
change (so stepping through disks with the buttons writes the flash once)
and only when it changed; a temporary PSRAM image is not remembered.

Answer: the settings as with `GET`. The WiFi network is changed in
[WiFi setup mode](#wifi-setup-mode), not here.

### `POST /api/v1/system/restart`

Body `{}`. Answers `202 {"restarting": true}` and restarts about 1 s
later. The Atari loses drive B: for a few seconds; a PSRAM image is gone.

### `GET /api/v1/system/update`

Placeholder for online updates; always answers:

```json
{ "current": "1.0.0", "status": "not_available", "update_available": false,
  "message": "Online update checks are not available yet." }
```

## Writing

The Atari can write to an **.ST file on the SD card** that is not
read-only. Always write-protected: HFE files, temporary PSRAM disks, a
disk played from its copy in the flash (no card at start-up), and a disk
whose file was replaced or deleted since it was inserted. What happens:

1. **Receive** — WDATA is sampled by an RMT receive channel with DMA
   (0.1 µs); a write ends when WDATA stays quiet for 16 µs.
2. **Decode** — MFM: three A1 syncs, data mark, 512 bytes, CRC. Only a
   complete sector with a correct CRC is used; anything else is ignored
   and counted in `writes_rejected` (the image is not touched; TOS
   retries). Formatting a track from the Atari is not supported.
3. **Which sector** — the WD1772 writes only the data field. RadioFloppy
   takes the rotation position when WGATE is asserted (time since the
   INDEX pulse) and the cylinder and side of that moment: the sector whose
   ID field just passed.
4. **Apply** — the sector is patched into the image kept in PSRAM and its
   track is encoded again, so the next read returns the new data.
5. **Save** — when the drive motor has been off for 2 s (one save per disk
   operation) or drive B: has not been selected for 3 s (the MOTOR line
   can read as on all the time on board revision 2); at the latest a
   minute after the last write:
   1. the changed 64 KiB blocks go into the **copy of the disk in the
      flash**, which is read back, checked against the new CRC-32 and
      marked *not on the card yet*;
   2. the same blocks are written into the **file on the SD card** (in
      place: same size, no new clusters), after checking that the file
      still is the one the changes belong to; the file is read back and
      checked against the new CRC-32;
   3. the flash copy is marked *on the card*.

   After a power cut in the middle the file on the card is the old or the
   new version: at the next start-up a copy marked *not on the card yet*
   is written to the card first.

If the file was replaced or deleted meanwhile (through the API, or on a
PC), the changes are not written over it: they are stored next to it as
`<name> (changes).st` (`write.note` says so).

`GET /api/v1/current` → `write`:

```json
{ "state": "pending", "writable": true, "reason": "", "unsaved": true,
  "sectors_written": 14, "writes_rejected": 0, "saves": 3 }
```

| `state`     | Meaning                                                                    |
| ----------- | -------------------------------------------------------------------------- |
| `read_only` | Write-protected; `reason` says why (`the file is read-only`, `HFE image (always read-only)`, `temporary disk (PSRAM)`, `no SD card`, …) |
| `writable`  | Writes accepted, everything saved                                          |
| `pending`   | Written sectors not saved yet (saved a few seconds after the last access)  |
| `saving`    | Being saved                                                                |
| `error`     | Saving failed (`error`): the disk is write-protected, the changes stay in memory (and in the flash once they got there) and are retried every 30 s (e.g. when the card is back) |

`note` (only when there is one): e.g. where changes went whose file was
replaced.

Guarantees and limits:

- Changes are **not** lost by inserting another disk, a restart through
  the API or the buttons (WiFi setup): those save first; if saving fails,
  they are refused with `409 UNSAVED_CHANGES`. Changing the root
  directory or making the inserted file read-only while there are unsaved
  changes is refused the same way.
- A **power cut** loses the changes not saved yet: those of the disk
  operation in progress, plus the few seconds before the save starts and the time it takes. Wait until the page shows *Writable — all
  changes saved* before switching off.
- If a file operation is interrupted by a power cut, the disk is left as a
  real floppy would be after a cut in the middle of writing: the saved
  version is consistent at block level, but the file system may lack the
  last sectors TOS wrote.

## Uploading images

An upload always takes two requests:

1. `POST /api/v1/uploads` with the options → `upload_url`
2. `PUT <upload_url>` with the file bytes → final result

```
ready ──PUT data──> receiving ──> processing ──> done
  │                     │              │
  │ (60 s, no data)     │ (dropped)    │ (invalid, SD error, drive busy)
  └──> failed <─────────┴──────────────┘
```

### Two kinds of upload

| You want to…                                   | Request body                                                       |
| ---------------------------------------------- | ------------------------------------------------------------------ |
| Play an .ST image now, without storing it      | `{"filename": "Game.st", "size": 368640, "destination": "psram"}` |
| Put an image on the SD card                    | `{"filename": "Game.st", "size": 368640, "destination": "sd", "path": "G"}` |

To play a file from the card afterwards: `PUT /api/v1/current`
`{"sd_path": "G/Game.st"}`.

### Guarantees

- A PSRAM upload is received completely into memory first. An image that
  is incomplete, invalid or does not match its `crc32` never becomes the
  active disk. It replaces the previous PSRAM image only once it is
  complete and prepared; there is only one PSRAM image.
- An SD card upload goes to a hidden temporary file; only a complete,
  checked and read-back file gets its name. An existing file is replaced
  only with `overwrite: true`, and only by a complete new file.

### DRIVE_BUSY

A new active disk is switched in right away while the Atari is not using
drive B:, and after 0.3 s also while it is (reading only). A disk the Atari
writes to is never changed under it. The device waits up to 3 seconds; if
the Atari keeps the drive busy longer, the request ends with
`409 DRIVE_BUSY` and the old disk stays in the drive (a PSRAM upload is
then not kept — upload it again later).

## Errors

Every error response has the same shape:

```json
{ "error": { "code": "FILE_NOT_FOUND", "message": "no such file" } }
```

`code` is stable and meant for programs; `message` is for people and may
change. A failed upload carries the same `error` inside its upload object.

| HTTP | Code                   | What to do                                             |
| ---- | ---------------------- | ------------------------------------------------------ |
| 400  | `INVALID_REQUEST`      | Fix the request                                        |
| 400  | `INVALID_PATH`         | Follow the [path rules](#sd-card-files)                |
| 400  | `CONFIRMATION_REQUIRED` | Repeat the path in `confirm`                          |
| 400  | `UPLOAD_INCOMPLETE`    | Start a new upload and send the complete file          |
| 401  | `UNAUTHORIZED`         | Send the correct token (only when one is configured)   |
| 415  | `UNSUPPORTED_MEDIA_TYPE` | Send `Content-Type: application/json` / `application/octet-stream` |
| 404  | `FILE_NOT_FOUND`, `FOLDER_NOT_FOUND` | Use a path from `GET /sd/files`          |
| 404  | `UPLOAD_NOT_FOUND`     | Start a new upload                                     |
| 409  | `FILE_EXISTS`, `FOLDER_EXISTS` | Choose another name (or `overwrite: true`)     |
| 409  | `FILE_READ_ONLY`       | Make the file writable first                           |
| 409  | `UNSAVED_CHANGES`      | The Atari wrote to the disk and it is not saved yet — wait and retry |
| 409  | `FOLDER_NOT_EMPTY`     | Delete with `recursive=true&confirm=<path>`            |
| 409  | `UPLOAD_BUSY`          | Wait for the running upload (or its 60 s timeout)      |
| 409  | `UPLOAD_STATE`         | Start a new upload                                     |
| 409  | `DRIVE_BUSY`           | Retry when the Atari is idle (see above)               |
| 409  | `MACHINE_NOT_SUPPORTED` | Floppy emulation is off for this computer — choose `ATARI` and restart |
| 413  | `IMAGE_TOO_LARGE`      | .ST up to 1 572 864 bytes; HFE up to 8 MiB and the track buffer |
| 422  | `INVALID_IMAGE`        | The file is not a floppy image                         |
| 422  | `UNSUPPORTED_GEOMETRY` | Use an image with 79–84 tracks and 9–11 sectors        |
| 422  | `CHECKSUM_MISMATCH`    | The file was damaged in transit — send it again        |
| 422  | `UNSUPPORTED_FORMAT`   | A file type or format that cannot be played (yet), e.g. HFEv2 |
| 422  | `INVALID_NAME`         | Follow the name rules                                  |
| 422  | `INVALID_HOSTNAME`     | Use 1–32 letters, digits or `-`                        |
| 422  | `INVALID_DRIVE_SELECT` | Use `"DS0"` or `"DS1"`                                 |
| 422  | `INVALID_MACHINE`      | Use `"ATARI"`, `"AMIGA"` or `"DOS"`                    |
| 422  | `INVALID_ROOT_DIR`     | Follow the root directory rules                        |
| 500  | `SD_ERROR`             | SD card problem — retry; check the card and the device log |
| 500  | `PREPARE_FAILED`       | Internal problem — check the device log                |
| 503  | `SD_NOT_PRESENT`       | Insert an SD card                                      |
| 507  | `INSUFFICIENT_MEMORY`  | Retry later / after a restart                          |

## Client examples

### Shell (curl)

```sh
DEV=192.168.178.62
TOKEN=your-api-token
H="Authorization: Bearer $TOKEN"
J="Content-Type: application/json"

# upload FILE DESTINATION [extra JSON, e.g. ',"path":"G"']
upload() {
    size=$(stat -c %s "$1")
    url=$(curl -s -H "$H" -H "$J" \
          -d "{\"filename\":\"$(basename "$1")\",\"size\":$size,\"destination\":\"$2\"$3}" \
          http://$DEV/api/v1/uploads | sed -n 's/.*"upload_url":"\([^"]*\)".*/\1/p')
    [ -n "$url" ] || { echo "upload refused"; return 1; }
    curl -s -X PUT -H "$H" -H "Content-Type: application/octet-stream" \
         --data-binary @"$1" http://$DEV$url; echo
}

upload "Crystal Castles.st" psram                  # play now, not stored
curl -s -X POST -H "$H" -H "$J" -d '{"path":"C"}' http://$DEV/api/v1/sd/folders
upload "Crystal Castles.st" sd ',"path":"C"'       # put it on the card in folder C

# Insert it / list folder C / delete it
curl -s -X PUT -H "$H" -H "$J" -d '{"sd_path":"C/Crystal Castles.st"}' http://$DEV/api/v1/current
curl -s "http://$DEV/api/v1/sd/files?path=C"
curl -s -X DELETE -H "$H" "http://$DEV/api/v1/sd/files?path=C/Crystal%20Castles.st"
```

(To see the full answer of the first step, run it without the `sed` part.)

### Python (standard library only)

```python
import http.client, json, os, zlib

DEV, TOKEN = "192.168.178.62", "your-api-token"

def call(method, path, body=None, data=None):
    conn = http.client.HTTPConnection(DEV, 80, timeout=60)
    headers = {"Authorization": "Bearer " + TOKEN}
    if body is not None:
        data, headers["Content-Type"] = json.dumps(body).encode(), "application/json"
    elif data is not None:
        headers["Content-Type"] = "application/octet-stream"
    conn.request(method, path, body=data, headers=headers)
    resp = conn.getresponse()
    result = json.loads(resp.read() or b"{}")
    conn.close()
    return resp.status, result

def upload(path, destination="sd", **options):
    data = open(path, "rb").read()
    request = {"filename": os.path.basename(path), "size": len(data),
               "destination": destination, "crc32": "%08x" % zlib.crc32(data), **options}
    status, up = call("POST", "/api/v1/uploads", request)
    if status != 201:
        raise RuntimeError(up["error"])
    status, up = call("PUT", up["upload_url"], data=data)
    if up.get("state") != "done":
        raise RuntimeError(up.get("error", up))
    return up

print(upload("Crystal Castles.st", "sd", path=""))
print(call("PUT", "/api/v1/current", {"sd_path": "Crystal Castles.st"})[1])
```

A complete test suite that exercises every endpoint and error is in
`tests/api/api_test.py`.

## Limits and behaviour

| Item                          | Value                                             |
| ----------------------------- | ------------------------------------------------- |
| .ST size / geometry           | 79–84 cyl, 9–11 sectors, 1–2 sides (up to 946 176 bytes); upload limit 1.5 MiB |
| HFE file                      | up to 8 MiB and what fits the track buffer; its flash copy up to 3 MiB |
| PSRAM images                  | 1                                                 |
| Uploads at the same time      | 1                                                 |
| Upload without data expires   | after 60 s                                        |
| Network timeout while sending | 10 s without data → `UPLOAD_INCOMPLETE`           |
| Wait for drive to be idle     | 0.3 s, then changed anyway unless the Atari writes to it; up to 3 s → else `DRIVE_BUSY` |
| Disk change signal to the Atari | 0.7 s "no disk"                                 |
| Typical duration              | Insert a 2 MB HFE file ≈ 5 s; PSRAM upload 720 KB ≈ 3 s; flash copy of 2 MB ≈ 17 s (in the background) |
| SD folder listing             | at most 400 entries                               |

- The device handles one request at a time; a long upload delays other
  requests until it is done. The flash copy runs in the background and
  holds the SD card only per 8 KiB read.
- The emulated drive keeps working while the API is used. Uploading or
  deleting never disturbs the disk the Atari is reading.
- Restarting the device removes the PSRAM image and inserts the SD card
  file that was active last (`last_sd_path`): from the card, or from its
  copy in the flash when the card cannot be read. Otherwise the drive
  starts empty.

## Device configuration

The host name and the WiFi network are stored on the external flash
(`docs/FLASH_LAYOUT.md`, settings sectors). They are set with the web page:
the host name under **Settings**, the WiFi network in
[WiFi setup mode](#wifi-setup-mode).

Until settings are saved there, the values from the firmware configuration
are used (`idf.py menuconfig` → *RadioFloppy*: WiFi SSID, WiFi password,
Hostname). These are stored only in the local `sdkconfig` file, which is
not part of the repository. The optional API token is only set there:

```sh
idf.py menuconfig        # → RadioFloppy: defaults, API token
idf.py build flash
```

- No WiFi network set at all: RadioFloppy starts in WiFi setup mode.
- No token (default): the API is open on the local network.
- Its address: hold both buttons 3 s (display), or see the serial console:
  `WiFi: connected to "…", IP 192.168.178.62, API http://192.168.178.62/api/v1/status`.

If you set a token, choose a long random one, e.g. `python3 -c "import secrets; print(secrets.token_hex(16))"`.

## WiFi setup mode

For first use and for moving to another network. RadioFloppy opens its own
WiFi network with a single setup page; **the floppy emulation is off**.

It starts:

- automatically when no WiFi network is set;
- when both buttons are held for 10 s (the display shows
  *WiFi setup mode / Restarting...*);
- automatically when new WiFi settings did not work (see below).

The display shows what to do:

```
WiFi setup mode
RadioFloppy-3A4F        <- network name (host name + end of the MAC address)
Key k7mqx2pa            <- WPA2 key, new random key every time
http://192.168.4.1/
```

Join that network with a phone or computer. Most devices open the setup
page by themselves ("sign in to network"); otherwise open
`http://192.168.4.1/`. The page lists the networks nearby (strongest
first, with signal and lock); pick one or type the name of a hidden
network, choose the security and enter the password (*Show password*
shows it while typing). *Save and connect* stores the settings and
restarts RadioFloppy, which then joins the network.

If it cannot join within 45 s, it returns to setup mode and the page says
why: network not found, wrong password, security type, or no address.
Setup mode is left without changes with *Leave setup without changes* or
both buttons for 3 s (only when a network was set before). A reset or
power cycle also returns to normal mode.

Security options (the lowest security accepted):

| Value  | Page text                           |
| ------ | ----------------------------------- |
| `wpa2` | WPA2/WPA3 Personal (recommended)    |
| `wpa3` | WPA3 Personal only                  |
| `wpa`  | WPA/WPA2 Personal (older routers)   |
| `open` | Open network (no password)          |

Setup API (only in setup mode, on the device's own network):

| Method and path              | Purpose                                                   |
| ---------------------------- | --------------------------------------------------------- |
| `GET /api/v1/setup`          | Reason for setup mode, current network (no password), last join error |
| `GET /api/v1/setup/scan`     | `{"networks": [{"ssid", "rssi", "security", "supported"}]}` |
| `PUT /api/v1/setup/wifi`     | `{"ssid", "security", "password"}` — save and restart. An empty password for the same network keeps the saved one |
| `POST /api/v1/setup/exit`    | `{}` — restart without changes (`409 NOT_CONFIGURED` if no network is set) |

Every other URL redirects to the setup page; a small DNS server answers
every name with the device's address.

## Implementation notes

For firmware developers; clients do not need this.

- Code: `main/api.c` (endpoints, uploads), `main/st_image.c` (image
  checks), `main/disk_image.c` (active disk, two PSRAM track buffers),
  `main/sd_card.c` (SD card files), `main/disk_copy.c` (the copy of the
  last disk in the flash; see `docs/FLASH_LAYOUT.md`),
  `main/wifi_net.c`, `main/settings.c` (settings on the external flash),
  `main/setup_mode.c` (setup access point, DNS, setup page `web/setup.html`).
- WiFi, lwIP and the HTTP server run on core 1; the floppy interrupts and
  the flux stream on core 0. WiFi keeps its credentials in RAM and starts
  before the flux stream, so no internal-flash write stalls the caches while
  the drive runs.
- New disks are encoded into the inactive of two PSRAM track buffers,
  switched under the drive lock, never while the computer writes to a
  writable disk (the switch inverts WPROT and gates INDEX/RDATA for 0.7 s,
  so a selected drive sees an opened and closed door), and then verified
  again in the background (`DISK_BACKGROUND_VERIFY`).
- API, buttons and web page change disks through one module,
  `main/disk_switch.c`, serialised by one mutex.
