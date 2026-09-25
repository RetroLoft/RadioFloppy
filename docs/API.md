# RadioFloppy HTTP API — version 1

RadioFloppy is a WiFi floppy drive emulator for the Atari ST. It replaces a
real floppy drive (here: external drive **B:**) and plays `.ST` disk images
to the Atari. This API lets you put disk images on the device and choose
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

- **Current Floppy** — the disk the Atari sees now, its origin (my floppy
  images or temporary PSRAM image) and a warning when that image was
  replaced or deleted since.
- **Add a floppy** — upload a `.ST` file: temporarily (PSRAM), or add it to
  my floppy images (optionally loading it at once).
- **My floppy images** — all stored images in their order (`sequence`),
  each with **↑ ↓** (change the order), **Load**, **Replace…** (asks before
  the old image is lost) and **Delete**; above the list the storage use,
  e.g. *Storage: 61% used*.
- **Admin access** — only shown when the device has a token; it is kept only in the open page.
- **Settings** (gear icon, top right, `/#settings`) — host name, floppy drive (A:/B:), buzzer on/off, WiFi status,
  firmware version and a *Check for updates* button (not functional yet).

The page polls `GET /current` every 3 s and `GET /images` every 15 s (and
after changes), one request at a time, and pauses while it uploads or loads.
Source: `web/index.html`, embedded in the firmware.

## Front panel buttons

**Left** = previous disk, **right** = next disk, wrapping around. The list
is: the PSRAM image (if there is one), then every valid image in
`sequence` order. A press gives a click on the buzzer; the switch takes
about 0.4 s (360 KB) to 0.9 s (880 KB) and follows the same rules as
`PUT /current`.

Both buttons held **3 s**: the display shows the host name and IP address
for 10 s. Held on to **10 s**: RadioFloppy restarts in
[WiFi setup mode](#wifi-setup-mode). In setup mode, both buttons held 3 s
leave it without changes.

## Quick start

```sh
DEV=192.168.178.62            # the device address (see "Device configuration")
# What is the device doing?
curl -s http://$DEV/api/v1/status

# Which images are stored, and how full is the storage?
curl -s http://$DEV/api/v1/images

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

- **my floppy images** — the images stored on RadioFloppy, or
- **PSRAM** — one temporary image that is lost when the device restarts.

`GET /api/v1/current` tells you which one it is. After a restart the device
inserts the library image that was active last (else the one named
*Crystal Castles*, else the first valid image in `sequence` order); a PSRAM
image is gone. A stored image is always
read completely into PSRAM and prepared before the Atari sees it.

### My floppy images

Stored images are managed as **images**, never as storage places:

| Field      | Meaning                                                                    |
| ---------- | -------------------------------------------------------------------------- |
| `id`       | Fixed number of the image (1–65535). Does not change when the order changes or the image is replaced; not reused while the image exists. |
| `sequence` | Position in lists (1 = first). Changing it never moves image data.         |
| `status`   | `valid`, or `incomplete`: replacing it failed or was interrupted — it holds no data; replace or delete it. |

The flash is divided into equal blocks (64 KiB on a 16 MiB chip, larger on
bigger chips; see `docs/FLASH_LAYOUT.md`); an image occupies as many blocks
as it needs, anywhere on the flash. Clients never see or choose blocks; the
storage use is reported as a percentage of all blocks (`used_percent`),
plus the exact numbers.

### Supported images

Raw `.ST` images (sector dumps without header) with

| Property          | Supported                                      |
| ----------------- | ---------------------------------------------- |
| Cylinders         | 79–84 (with 79, track 79 reads as unformatted) |
| Sectors per track | 9, 10 or 11 (512 bytes)                        |
| Sides             | 1 or 2                                         |
| File size         | up to 946 176 bytes (84/2/11); the storage limit is 1.5 MiB |

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
refused as `IMAGE_TOO_LARGE`. `.MSA` and `.HFE` are not supported. The
drive is **read-only** for the Atari: it reports the disk as
write-protected.

The track layout follows FlashFloppy: GAP3 84 (9 sectors), 30 (10
sectors) or 3 with interleave 2 (11 sectors). An 11-sector track is
longer than a standard track and is played with slightly shorter bitcells
(1.945 µs instead of 2 µs), so a revolution still takes 200 ms.

### Disk changes

When the active disk changes, the drive behaves like a real drive whose
door is opened and closed: for 0.7 s it shows no disk. The Atari (TOS)
notices this and reads the new disk's directory the next time you open
drive B:.

The drive never changes disks in the middle of an access: a switch only
happens while the Atari is not using drive B:. See
[DRIVE_BUSY](#drive_busy).

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
| `GET /api/v1/status`            |       | Device, WiFi, storage and drive status      |
| `GET /api/v1/images`            |       | All images in `sequence` order + storage use |
| `GET /api/v1/images/{id}`       |       | One image                                   |
| `PUT /api/v1/images/{id}`       | (yes) | Change the position (`sequence`)            |
| `DELETE /api/v1/images/{id}`    | (yes) | Delete an image                             |
| `GET /api/v1/storage`           |       | Storage use and geometry                    |
| `POST /api/v1/storage/format`   | (yes) | Initialise the storage (erases all images)  |
| `GET /api/v1/current`           |       | The active disk                             |
| `PUT /api/v1/current`           | (yes) | Make a stored image the active disk         |
| `POST /api/v1/uploads`          | (yes) | Start an upload (new, replacement or PSRAM) |
| `PUT /api/v1/uploads/{id}/data` | (yes) | Send the image bytes of an upload           |
| `GET /api/v1/uploads/{id}`      |       | State of an upload                          |
| `GET /api/v1/settings`          |       | Device settings, WiFi and firmware          |
| `PUT /api/v1/settings`          | (yes) | Change settings (host name)                 |
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
  "storage": { "state": "valid", "used_percent": 26, "...": "as GET /storage" },
  "drive": { "select_line": "DS1", "armed": true, "selected": false, "motor": false, "cylinder": 0 },
  "current": { "inserted": true, "source": "flash", "image_id": 1, "image_changed_since": false,
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
| `storage`                   | Same object as [`GET /storage`](#get-apiv1storage)                |
| `drive.select_line`         | Which drive-select line the emulator answers to (`DS1` = drive B:) |
| `drive.armed`               | The emulator has seen the Atari powered on and answers to it      |
| `drive.selected`            | The Atari is using our drive right now                            |
| `drive.motor`               | The floppy motor line is on                                       |
| `drive.cylinder`            | Current head position (0–83)                                      |
| `current`                   | Same object as [`GET /current`](#get-apiv1current)                |
| `psram_free`                | Free PSRAM in bytes                                               |
| `auth_required`             | A token is configured: changing calls need it                     |
| `psram_image`               | The kept PSRAM image, if any (selectable with the buttons)        |
| `upload`                    | The latest upload, if any (same object as [`GET /uploads/{id}`](#get-apiv1uploadsid)) |

---

### `GET /api/v1/images`

All images in `sequence` order, and the storage use.

```json
{
  "images": [
    { "id": 6, "sequence": 1, "status": "valid", "name": "Weird Dreams", "format": "st",
      "size": 839680, "stored_size": 839680, "storage_format": "raw", "crc32": "342b844d",
      "blocks_used": 13, "active": true },
    { "id": 3, "sequence": 2, "status": "incomplete", "name": "Leisure Suit Larry", "active": false }
  ],
  "storage": { "state": "valid", "used_percent": 26, "...": "as GET /storage" }
}
```

| Field            | Meaning                                                             |
| ---------------- | ------------------------------------------------------------------- |
| `id`, `sequence`, `status` | See [My floppy images](#my-floppy-images)                 |
| `name`           | Title (from the file name when it was uploaded)                     |
| `size`           | Image size in bytes (only `valid`)                                  |
| `stored_size`, `storage_format` | Bytes as stored and how: always `raw` (= `size`) for now; compression may follow |
| `crc32`          | CRC-32 of the image as 8 hex digits (same as `zlib.crc32`, `crc32` command) |
| `blocks_used`    | Number of storage blocks it occupies                                |
| `active`         | This image is the active disk (and has not been replaced since it was activated) |

`GET /api/v1/images/{id}` returns one such object (`404 IMAGE_NOT_FOUND`
if there is none).

---

### `PUT /api/v1/images/{id}`

Change the order. Token required. The image is moved to position
`sequence` (1 = first; larger than the number of images = last) and all
images are renumbered 1…n. No image data is read or written.

```json
{ "sequence": 1 }
```

Response `200`: the image object. Errors: `400 INVALID_REQUEST` (`sequence`
missing or < 1), `404 IMAGE_NOT_FOUND`, `503 STORAGE_NOT_READY`.

---

### `DELETE /api/v1/images/{id}`

Delete an image. Token required. Its blocks are free at once (erased only
when they are reused). If it is the active disk, the drive keeps playing it
until another disk is activated.

Response `200`:

```json
{ "id": 2, "status": "deleted", "storage": { "...": "as GET /storage" } }
```

| Error                   | When                                          |
| ----------------------- | --------------------------------------------- |
| `404 IMAGE_NOT_FOUND`   | No image with this id                         |
| `409 UPLOAD_BUSY`       | An upload replacing this image is in progress |
| `503 STORAGE_NOT_READY` | The storage is not usable                     |

---

### `GET /api/v1/storage`

```json
{ "state": "valid", "flash_detected": true, "jedec_id": "016018", "capacity": 16777216,
  "block_size": 65536, "blocks_total": 255, "blocks_used": 67, "blocks_free": 188,
  "used_percent": 26, "images": 6, "max_image_size": 1572864, "free_bytes": 12320768 }
```

| Field              | Meaning                                                        |
| ------------------ | -------------------------------------------------------------- |
| `state`            | `valid`; `old_format` (an earlier RadioFloppy format — initialise it); `invalid` (unknown data or a different flash size — not touched); `no_flash` |
| `capacity`         | Detected flash size in bytes                                   |
| `block_size`       | Storage block size (64 KiB for 16 MiB, 128 KiB for 32 MiB, …)  |
| `blocks_total`, `blocks_used`, `blocks_free` | Data blocks (at most 255)            |
| `used_percent`     | `blocks_used / blocks_total`, rounded                          |
| `images`           | Number of valid images                                         |
| `free_bytes`       | `blocks_free × block_size`                                     |

### `POST /api/v1/storage/format`

Initialise the storage: **every stored image is lost**. Needed once when the
flash still holds an older RadioFloppy format (`state: "old_format"`); a
completely erased flash is initialised automatically. Token required.

```json
{ "confirm": "ERASE ALL IMAGES" }
```

Response `200`: the storage object. Without the exact confirmation:
`400 CONFIRMATION_REQUIRED`. The disk in the drive stays loaded until
another disk is activated or the device restarts.

---

### `GET /api/v1/current`

The active disk.

```json
{ "inserted": true, "source": "flash", "image_id": 2, "image_changed_since": false,
  "name": "Nebulus", "size": 419840, "crc32": "40881c6a", "sides": 1,
  "cylinders": 82, "sectors": 10 }
```

| Field                 | Meaning                                                          |
| --------------------- | ---------------------------------------------------------------- |
| `inserted`            | `false`: no disk in the drive (then only `source: "none"`)       |
| `source`              | `flash` (my floppy images), `psram` or `none`                    |
| `image_id`            | Only for `flash`: the image it was loaded from                   |
| `image_changed_since` | Only for `flash`: that image was replaced or deleted since; the drive still plays the disk as it was when activated |
| `name`, `size`, `crc32` | The disk image                                                 |
| `sides`, `cylinders`, `sectors` | Geometry: 1 or 2 sides, 79–84 cylinders, 9–11 sectors per track |

---

### `PUT /api/v1/current`

Make a stored image the active disk. Token required.

```json
{ "image_id": 5 }
```

Response `200`: the new [`current`](#get-apiv1current) object. Takes about
0.4–0.9 s (the image is read through its blocks, its CRC checked, the
tracks prepared).

| Error                  | When                                             |
| ---------------------- | ------------------------------------------------ |
| `400 INVALID_REQUEST`  | `image_id` missing or not a number 1–65535       |
| `404 IMAGE_NOT_FOUND`  | No valid image with this id                      |
| `409 DRIVE_BUSY`       | The Atari kept using drive B:; nothing changed   |
| `500 FLASH_ERROR`      | The stored image could not be read or its CRC is wrong |

---

### `POST /api/v1/uploads`

Start an upload. Token required. See [Uploading images](#uploading-images).

Request fields:

| Field         | Type    | Required | Meaning                                                   |
| ------------- | ------- | -------- | --------------------------------------------------------- |
| `filename`    | string  | yes      | Original file name; becomes the title (TOSEC tags removed, max. 52 characters). Only used as a name. |
| `size`        | number  | yes      | Exact size of the file in bytes                           |
| `destination` | string  | yes      | `"psram"` (temporary) or `"flash"` (my floppy images)     |
| `replace`     | number  | no       | Only with `"flash"`: id of the image to **replace** (keeps its id and position). Leave out to add a new image at the end. |
| `activate`    | boolean | no       | `"flash"`: make it the active disk afterwards (default `false`). `"psram"`: always activated; `false` is refused. |
| `crc32`       | string  | no       | CRC-32 of the file as hex, e.g. `"42ce7eed"`. If given, the received data must match. |

Response `201 Created`: an [upload object](#get-apiv1uploadsid) in state
`ready`, with the `upload_url` to send the data to:

```json
{ "upload_id": "26", "state": "ready", "name": "Crystal Castles", "size": 368640,
  "received": 0, "destination": "flash", "activate": false,
  "upload_url": "/api/v1/uploads/26/data" }
```

The size, geometry, image to replace and free space are checked here
already, so a client learns about most problems before sending any data.
When replacing, the blocks of the old image count as free.

| Error                        | When                                               |
| ---------------------------- | -------------------------------------------------- |
| `400 INVALID_REQUEST`        | Missing/wrong fields, `psram` with `activate: false`, `replace` with `psram` |
| `404 IMAGE_NOT_FOUND`        | `replace` names no image                           |
| `409 UPLOAD_BUSY`            | Another upload is still in progress                |
| `413 IMAGE_TOO_LARGE`        | `size` above 1 572 864 bytes (1.5 MiB)             |
| `422 INVALID_IMAGE`          | `size` cannot be a floppy image                    |
| `422 UNSUPPORTED_GEOMETRY`   | `size` is a `.ST` geometry that is not supported   |
| `503 STORAGE_NOT_READY`      | `flash`, but the storage is not usable             |
| `507 NO_SPACE`               | Not enough free blocks                             |
| `507 INSUFFICIENT_MEMORY`    | Not enough memory for the upload                   |

---

### `PUT /api/v1/uploads/{id}/data`

Send the image bytes. Token required. Body: the complete file, nothing
else; `Content-Type: application/octet-stream`; `Content-Length` must equal
the announced `size`. Data can be sent only once per upload.

The request returns when everything is finished — received, checked, stored
and/or activated — typically 3–7 seconds. The response is the final
[upload object](#get-apiv1uploadsid):

```json
{ "upload_id": "26", "state": "done", "name": "Crystal Castles", "size": 368640,
  "received": 368640, "destination": "flash", "activate": false,
  "crc32": "42ce7eed", "image_id": 7, "active": false }
```

On failure the HTTP status matches the error, and the upload object has
`"state": "failed"` and an `error`:

```json
{ "upload_id": "27", "state": "failed", "name": "Crystal Castles", "size": 368640,
  "received": 100000, "destination": "flash", "activate": false,
  "error": { "code": "UPLOAD_INCOMPLETE", "message": "received 100000 of 368640 bytes" } }
```

| Error                      | When                                                        |
| -------------------------- | ----------------------------------------------------------- |
| `400 UPLOAD_INCOMPLETE`    | `Content-Length` ≠ `size`, or the connection dropped        |
| `404 UPLOAD_NOT_FOUND`     | Unknown or superseded upload id                             |
| `409 UPLOAD_STATE`         | Data was already sent for this upload                       |
| `409 DRIVE_BUSY`           | Stored (if `flash`) but not activated — see below           |
| `422 INVALID_IMAGE` / `UNSUPPORTED_GEOMETRY` | The content is not a supported `.ST` image |
| `422 CHECKSUM_MISMATCH`    | Data does not match the given `crc32`                       |
| `500 FLASH_ERROR`          | Writing or verifying failed (nothing stored; when replacing, the old image is lost) |
| `500 PREPARE_FAILED`       | The disk could not be prepared for the drive                |
| `507 NO_SPACE`             | Not enough free blocks any more                             |
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
| `destination`    | always             | `psram` or `flash`                              |
| `replace`        | when replacing     | The image being replaced                        |
| `activate`       | always             | Whether it will be / was activated              |
| `upload_url`     | state `ready`      | Where to `PUT` the data                         |
| `crc32`          | state `done`       | CRC-32 of the received image                    |
| `image_id`       | stored             | The stored image (also on `DRIVE_BUSY`)         |
| `active`         | state `done`       | It is now the active disk                       |
| `error`          | state `failed`     | `{ "code": ..., "message": ... }`               |

### `GET /api/v1/settings`

```json
{
  "hostname": "RadioFloppy",
  "drive_select": "DS1",
  "drive_select_active": "DS1",
  "buzzer": true,
  "last_image_id": 6,
  "restart_required": false,
  "wifi": { "ssid": "MyNetwork", "security": "wpa2", "connected": true,
            "ip": "192.168.178.62", "rssi": -38 },
  "firmware": { "version": "1.0.0", "idf": "v5.5.5" }
}
```

The WiFi password is never returned. `restart_required` is `true` when a
saved setting only takes effect after a restart (e.g. a new host name).

### `PUT /api/v1/settings`

Body (`Content-Type: application/json`), every field optional:

```json
{ "hostname": "Atari-B", "drive_select": "DS1", "buzzer": false }
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

`last_image_id` (read only) is the library image that is loaded at the next
start-up. It is stored 5 s after the last disk change (so stepping through
disks with the buttons writes the flash once) and only when it changed; a
temporary PSRAM image is not remembered. If that image no longer exists,
the start-up falls back to *Crystal Castles*, else the first image.

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

## Uploading images

An upload always takes two requests:

1. `POST /api/v1/uploads` with the options → `upload_url`
2. `PUT <upload_url>` with the file bytes → final result

```
ready ──PUT data──> receiving ──> processing ──> done
  │                     │              │
  │ (60 s, no data)     │ (dropped)    │ (invalid, flash error, drive busy)
  └──> failed <─────────┴──────────────┘
```

### Three kinds of upload

| You want to…                                   | Request body                                                       |
| ---------------------------------------------- | ------------------------------------------------------------------ |
| Play an image now, without storing it          | `{"filename": "Game.st", "size": 368640, "destination": "psram"}` |
| Add an image to my floppy images               | `{"filename": "Game.st", "size": 368640, "destination": "flash"}` |
| Replace image 5 and play the new version       | `{"filename": "Game.st", "size": 368640, "destination": "flash", "replace": 5, "activate": true}` |

Add `"activate": true` to the second form to also play a stored image at
once.

### Guarantees

- The data is first received completely into memory. An image that is
  incomplete, invalid or does not match its `crc32` never touches the
  storage and never becomes the active disk.
- A new image only goes into free blocks, is read back and checked, and
  only then appears in the list. An interrupted store (also a power cut)
  leaves no image behind, and its blocks are free again.
- Without `replace`, a stored image is **never** overwritten.
- With `replace`, the old image is given up first, so replacing also works
  when the storage is nearly full (its blocks are reused first). **If the
  upload then fails, or the power fails while writing, the old image is
  lost** — the image shows `incomplete`, never a half image.
- A PSRAM upload replaces the previous PSRAM image only once the new one is
  complete and prepared. There is only one PSRAM image.

### DRIVE_BUSY

A new active disk is switched in only while the Atari is not using drive
B:. The device waits up to 3 seconds for such a moment. If the Atari keeps
the drive busy longer (for example while loading a game), the request ends
with `409 DRIVE_BUSY` and the old disk stays in the drive:

- `flash` upload: the image *is* stored (`image_id`) — activate it later
  with `PUT /api/v1/current`.
- `psram` upload: nothing is kept — upload it again later.

## Errors

Every error response has the same shape:

```json
{ "error": { "code": "NO_SPACE", "message": "not enough free storage: 14 blocks needed, 9 free" } }
```

`code` is stable and meant for programs; `message` is for people and may
change. A failed upload carries the same `error` inside its upload object.

| HTTP | Code                   | What to do                                             |
| ---- | ---------------------- | ------------------------------------------------------ |
| 400  | `INVALID_REQUEST`      | Fix the request                                        |
| 400  | `CONFIRMATION_REQUIRED` | Send the exact confirmation text                      |
| 400  | `UPLOAD_INCOMPLETE`    | Start a new upload and send the complete file          |
| 401  | `UNAUTHORIZED`         | Send the correct token (only when one is configured)   |
| 415  | `UNSUPPORTED_MEDIA_TYPE` | Send `Content-Type: application/json` / `application/octet-stream` |
| 404  | `IMAGE_NOT_FOUND`      | Use an id from `GET /images`                           |
| 404  | `UPLOAD_NOT_FOUND`     | Start a new upload                                     |
| 409  | `UPLOAD_BUSY`          | Wait for the running upload (or its 60 s timeout)      |
| 409  | `UPLOAD_STATE`         | Start a new upload                                     |
| 409  | `DRIVE_BUSY`           | Retry when the Atari is idle (see above)               |
| 413  | `IMAGE_TOO_LARGE`      | Only images up to 1 572 864 bytes (1.5 MiB)            |
| 422  | `INVALID_IMAGE`        | The file is not a floppy image                         |
| 422  | `UNSUPPORTED_GEOMETRY` | Use an image with 79–84 tracks and 9–11 sectors        |
| 422  | `CHECKSUM_MISMATCH`    | The file was damaged in transit — send it again        |
| 422  | `INVALID_HOSTNAME`     | Use 1–32 letters, digits or `-`                        |
| 422  | `INVALID_DRIVE_SELECT` | Use `"DS0"` or `"DS1"`                                 |
| 500  | `FLASH_ERROR`          | Storage problem — retry; check the device log          |
| 500  | `PREPARE_FAILED`       | Internal problem — check the device log                |
| 503  | `STORAGE_NOT_READY`    | Storage not usable (see `GET /storage`, maybe initialise it) |
| 507  | `NO_SPACE`             | Delete an image, or replace one                        |
| 507  | `INSUFFICIENT_MEMORY`  | Retry later / after a restart                          |

## Client examples

### Shell (curl)

```sh
DEV=192.168.178.62
TOKEN=your-api-token

# upload FILE DESTINATION [extra JSON, e.g. ',"replace":5,"activate":true']
upload() {
    size=$(stat -c %s "$1")
    url=$(curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
          -d "{\"filename\":\"$(basename "$1")\",\"size\":$size,\"destination\":\"$2\"$3}" \
          http://$DEV/api/v1/uploads | sed -n 's/.*"upload_url":"\([^"]*\)".*/\1/p')
    [ -n "$url" ] || { echo "upload refused"; return 1; }
    curl -s -X PUT -H "Authorization: Bearer $TOKEN" \
         -H "Content-Type: application/octet-stream" \
         --data-binary @"$1" http://$DEV$url; echo
}

upload "Crystal Castles.st" psram                          # play now, not stored
upload "Crystal Castles.st" flash                          # add to my floppy images
upload "Crystal Castles.st" flash ',"replace":5,"activate":true'   # replace image 5, play it

# Play image 1 / move image 3 to the top / delete image 5
curl -s -X PUT -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
     -d '{"image_id":1}' http://$DEV/api/v1/current
curl -s -X PUT -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
     -d '{"sequence":1}' http://$DEV/api/v1/images/3
curl -s -X DELETE -H "Authorization: Bearer $TOKEN" http://$DEV/api/v1/images/5
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

def upload(path, destination="flash", **options):
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

print(upload("Crystal Castles.st", "flash", activate=True))
print(call("GET", "/api/v1/images")[1])
```

A complete test suite that exercises every endpoint and error is in
`tests/api/api_test.py`.

## Limits and behaviour

| Item                          | Value                                             |
| ----------------------------- | ------------------------------------------------- |
| Image size / geometry         | 79–84 cyl, 9–11 sectors, 1–2 sides (up to 946 176 bytes); storage limit 1.5 MiB |
| Stored images                 | as many as fit: 255 blocks (64 KiB each on 16 MiB), at least 1 block per image |
| PSRAM images                  | 1                                                 |
| Uploads at the same time      | 1                                                 |
| Upload without data expires   | after 60 s                                        |
| Network timeout while sending | 10 s without data → `UPLOAD_INCOMPLETE`           |
| Wait for drive to be idle     | up to 3 s → else `DRIVE_BUSY`                     |
| Disk change signal to the Atari | 0.7 s "no disk"                                 |
| Typical duration              | Activate an image: 360 KB ≈ 0.4 s, 880 KB ≈ 0.9 s; store 880 KB ≈ 7 s; PSRAM upload 720 KB ≈ 3 s |
| Disk name                     | title from `filename` (TOSEC tags removed, `(Disk n/m)` kept), max. 52 characters |

- The device handles one request at a time; a long upload delays other
  requests until it is done.
- The emulated drive keeps working while the API is used. Uploading or
  deleting never disturbs the disk the Atari is reading.
- Restarting the device removes the PSRAM image and inserts the library
  image that was active last (see `last_image_id`), else the image named
  *Crystal Castles*, else the first valid image in `sequence` order.

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
  `main/image_store.c` (image library: blocks, catalog, the image reader;
  see `docs/FLASH_LAYOUT.md`),
  `main/wifi_net.c`, `main/settings.c` (settings on the external flash),
  `main/setup_mode.c` (setup access point, DNS, setup page `web/setup.html`).
- WiFi, lwIP and the HTTP server run on core 1; the floppy interrupts and
  the flux stream on core 0. WiFi keeps its credentials in RAM and starts
  before the flux stream, so no internal-flash write stalls the caches while
  the drive runs.
- New disks are encoded into the inactive of two PSRAM track buffers,
  switched under the drive lock only while our drive is not selected (the
  switch releases WPROT and gates INDEX/RDATA for 0.7 s), and then verified
  again in the background (`DISK_BACKGROUND_VERIFY`).
- API, buttons and web page change disks through one module,
  `main/disk_switch.c`, serialised by one mutex.
