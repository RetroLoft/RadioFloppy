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

## Web interface

Open `http://<device>/` in a browser (PC or phone). The page is served by
the device itself, uses only this API and needs no internet access:

- **Current Floppy** — the disk the Atari sees now, its origin (flash slot
  or temporary PSRAM image) and a warning when its slot changed since.
- **Add a floppy** — upload a `.ST` file: temporarily (PSRAM), into the
  first free slot, or into a chosen slot (asks before overwriting).
- **Flash slots** — all 20 slots with a **Load** button for valid images.
- **Admin access** — only shown when the device has a token; it is kept only in the open page.

The page polls `GET /current` every 3 s and `GET /slots` every 15 s (and
after changes), one request at a time, and pauses while it uploads or loads.
Source: `web/index.html`, embedded in the firmware.

## Front panel buttons

**Left** = previous disk, **right** = next disk, wrapping around. The list
is: the PSRAM image (if there is one), then every valid flash slot in slot
order. A press gives a click on the buzzer; the switch takes about 0.5 s
(360 KB) to 0.8 s (720 KB) and follows the same rules as `PUT /current`.

## Quick start

```sh
DEV=192.168.178.62            # the device address (see "Device configuration")
# What is the device doing?
curl -s http://$DEV/api/v1/status

# Which images are stored?
curl -s http://$DEV/api/v1/slots

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

- a **flash slot** — one of 20 permanent storage places, or
- **PSRAM** — one temporary image that is lost when the device restarts.

`GET /api/v1/current` tells you which one it is. After a restart the device
inserts the stored image named *Crystal Castles* (or else the first valid
slot); a PSRAM image is gone.

### Flash slots

The device has 20 slots, numbered **1 to 20**. Each holds at most one image
of up to 800 KiB, together with its name, size and CRC-32. A slot is:

| Status       | Meaning                                                        |
| ------------ | -------------------------------------------------------------- |
| `valid`      | Holds a complete, verified image                               |
| `empty`      | Never used — free                                              |
| `deleted`    | Freed — free (the old bytes are erased when it is reused)      |
| `incomplete` | An upload into it was interrupted — free, never used as a disk |
| `invalid`    | The catalog entry is damaged — treat as unusable               |

Only `valid` slots can be activated. `empty`, `deleted` and `incomplete`
slots are all free for new uploads.

### Supported images

Raw `.ST` images (sector dumps without header) with

| Property          | Supported                                      |
| ----------------- | ---------------------------------------------- |
| Cylinders         | 80–84                                          |
| Sectors per track | 9, 10 or 11 (512 bytes)                        |
| Sides             | 1 or 2                                         |
| File size         | up to 819 200 bytes (800 KiB, one flash slot)  |

Examples: 360 KiB (80/1/9), 400 KiB (80/1/10), 410 KiB (82/1/10, e.g.
Nebulus), 440 KiB (80/1/11), 720 KiB (80/2/9), 800 KiB (80/2/10).
Double-sided images with 10 sectors and more than 80 cylinders, or with
11 sectors, are larger than 800 KiB and are refused (`IMAGE_TOO_LARGE`).

The geometry follows from the file size; a valid boot sector (BPB) must
agree with it (its sectors per track and sides; the file system may use
fewer tracks than the file holds). Other geometries (8 sectors, 40 or 70
cylinders, …) are recognised but not supported (`UNSUPPORTED_GEOMETRY`).
`.MSA` and `.HFE` are not supported. The drive is **read-only** for the
Atari: it reports the disk as write-protected.

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
| `GET /api/v1/slots`             |       | List the 20 flash slots                     |
| `GET /api/v1/current`           |       | The active disk                             |
| `PUT /api/v1/current`           | (yes) | Make a stored slot the active disk          |
| `DELETE /api/v1/slots/{slot}`   | (yes) | Free a slot                                 |
| `POST /api/v1/uploads`          | (yes) | Start an upload                             |
| `PUT /api/v1/uploads/{id}/data` | (yes) | Send the image bytes of an upload           |
| `GET /api/v1/uploads/{id}`      |       | State of an upload                          |

---

### `GET /api/v1/status`

Overall state of the device.

```json
{
  "device": "RadioFloppy",
  "api": "v1",
  "wifi": { "connected": true, "ssid": "MyNetwork", "ip": "192.168.178.62", "rssi": -38 },
  "external_flash": {
    "detected": true, "jedec_id": "016018", "size": 16777216,
    "slot_store": "valid", "slots_used": 1, "slots_total": 20
  },
  "drive": { "select_line": "DS1", "armed": true, "selected": false, "motor": false, "cylinder": 0 },
  "current": { "inserted": true, "source": "flash", "slot": 1, "slot_changed_since": false,
               "name": "Crystal Castles", "size": 368640, "crc32": "42ce7eed", "sides": 1 },
  "psram_free": 4322748,
  "auth_required": false,
  "psram_image": { "name": "Retroloft test", "size": 737280, "crc32": "c413f8cc", "sides": 2 },
  "upload": { "upload_id": "25", "state": "done", "...": "..." }
}
```

| Field                       | Meaning                                                           |
| --------------------------- | ----------------------------------------------------------------- |
| `wifi.rssi`                 | Signal strength in dBm                                            |
| `external_flash.slot_store` | `valid`, `blank` (no images stored yet) or `invalid` (unknown data: storing is disabled) |
| `drive.select_line`         | Which drive-select line the emulator answers to (`DS1` = drive B:) |
| `drive.armed`               | The emulator has seen the Atari powered on and answers to it      |
| `drive.selected`            | The Atari is using our drive right now                            |
| `drive.motor`               | The floppy motor line is on                                       |
| `drive.cylinder`            | Current head position (0–79)                                      |
| `current`                   | Same object as [`GET /current`](#get-apiv1current)                |
| `psram_free`                | Free PSRAM in bytes                                               |
| `auth_required`             | A token is configured: changing calls need it                     |
| `psram_image`               | The kept PSRAM image, if any (selectable with the buttons)        |
| `upload`                    | The latest upload, if any (same object as [`GET /uploads/{id}`](#get-apiv1uploadsid)) |

---

### `GET /api/v1/slots`

All 20 slots, slot 1 first.

```json
{
  "slots": [
    { "slot": 1, "status": "valid", "name": "Crystal Castles", "size": 368640,
      "crc32": "42ce7eed", "format": "st", "active": true },
    { "slot": 2, "status": "deleted", "active": false },
    { "slot": 3, "status": "empty", "active": false }
  ]
}
```

| Field    | Meaning                                                              |
| -------- | -------------------------------------------------------------------- |
| `slot`   | Slot number 1–20                                                     |
| `status` | See [Flash slots](#flash-slots)                                      |
| `name`, `size`, `crc32`, `format` | Only for `valid` slots. `crc32`: CRC-32 as 8 hex digits (same as `zlib.crc32`, `crc32` command) |
| `active` | This slot is the active disk (and has not been changed since it was activated) |

---

### `GET /api/v1/current`

The active disk.

```json
{ "inserted": true, "source": "flash", "slot": 4, "slot_changed_since": false,
  "name": "Nebulus", "size": 419840, "crc32": "40881c6a", "sides": 1,
  "cylinders": 82, "sectors": 10 }
```

| Field                | Meaning                                                          |
| -------------------- | ---------------------------------------------------------------- |
| `inserted`           | `false`: no disk in the drive (then only `source: "none"`)       |
| `source`             | `flash`, `psram` or `none`                                       |
| `slot`               | Only for `flash`: the slot it was loaded from                    |
| `slot_changed_since` | Only for `flash`: that slot was overwritten or freed since; the drive still plays the disk as it was when activated |
| `name`, `size`, `crc32` | The disk image                                                |
| `sides`, `cylinders`, `sectors` | Geometry: 1 or 2 sides, 80–84 cylinders, 9–11 sectors per track |

---

### `PUT /api/v1/current`

Make a stored slot the active disk. Token required.

Request:

```json
{ "slot": 5 }
```

Response `200`: the new [`current`](#get-apiv1current) object. Takes about
3 seconds (the image is read, checked and prepared).

| Error                  | When                                             |
| ---------------------- | ------------------------------------------------ |
| `400 INVALID_SLOT`     | `slot` missing or not 1–20                       |
| `404 SLOT_EMPTY`       | The slot holds no valid image                    |
| `409 DRIVE_BUSY`       | The Atari kept using drive B:; nothing changed   |
| `500 FLASH_ERROR`      | The stored image could not be read or its CRC is wrong |

---

### `DELETE /api/v1/slots/{slot}`

Free a slot (1–20). Token required. The image is removed from the catalog;
its bytes are erased only when the slot is reused. If it is the active
disk, the drive keeps playing it until another disk is activated.

Response `200`:

```json
{ "slot": 2, "status": "deleted" }
```

| Error              | When                                          |
| ------------------ | --------------------------------------------- |
| `400 INVALID_SLOT` | Not a number 1–20                             |
| `404 SLOT_EMPTY`   | The slot is already free                      |
| `409 UPLOAD_BUSY`  | An upload into this slot is in progress       |

---

### `POST /api/v1/uploads`

Start an upload. Token required. See [Uploading images](#uploading-images).

Request fields:

| Field         | Type    | Required | Meaning                                                   |
| ------------- | ------- | -------- | --------------------------------------------------------- |
| `filename`    | string  | yes      | Original file name. Its base name without extension becomes the disk name (max. 39 characters). Only used as a name. |
| `size`        | number  | yes      | Exact size of the file in bytes                           |
| `destination` | string  | yes      | `"psram"` or `"flash"`                                    |
| `slot`        | number  | no       | Only with `"flash"`: 1–20 to write exactly that slot (**overwrites** it). Leave out to use the first free slot. |
| `activate`    | boolean | no       | `"flash"`: make it the active disk afterwards (default `false`). `"psram"`: always activated; `false` is refused. |
| `crc32`       | string  | no       | CRC-32 of the file as hex, e.g. `"42ce7eed"`. If given, the received data must match. |

Response `201 Created`: an [upload object](#get-apiv1uploadsid) in state
`ready`, with the `upload_url` to send the data to:

```json
{ "upload_id": "26", "state": "ready", "name": "Crystal Castles", "size": 368640,
  "received": 0, "destination": "flash", "activate": false,
  "upload_url": "/api/v1/uploads/26/data" }
```

The size, geometry, slot number and free space are checked here already, so
a client learns about most problems before sending any data.

| Error                        | When                                               |
| ---------------------------- | -------------------------------------------------- |
| `400 INVALID_REQUEST`        | Missing/wrong fields, `psram` with `activate: false` |
| `400 INVALID_SLOT`           | `slot` not 1–20, or `slot` with `psram`            |
| `409 UPLOAD_BUSY`            | Another upload is still in progress                |
| `409 NO_FREE_SLOT`           | `flash` without `slot` and all 20 slots are in use |
| `413 IMAGE_TOO_LARGE`        | `size` above 819 200 bytes                         |
| `422 INVALID_IMAGE`          | `size` cannot be a floppy image                    |
| `422 UNSUPPORTED_GEOMETRY`   | `size` is a `.ST` geometry that is not supported   |
| `503 FLASH_ERROR`            | `flash`, but the storage is not usable             |
| `507 INSUFFICIENT_MEMORY`    | Not enough memory for the upload                   |

---

### `PUT /api/v1/uploads/{id}/data`

Send the image bytes. Token required. Body: the complete file, nothing
else; `Content-Type: application/octet-stream`; `Content-Length` must equal
the announced `size`. Data can be sent only once per upload.

The request returns when everything is finished — received, checked, stored
and/or activated — typically 3–6 seconds. The response is the final
[upload object](#get-apiv1uploadsid):

```json
{ "upload_id": "26", "state": "done", "name": "Crystal Castles", "size": 368640,
  "received": 368640, "destination": "flash", "activate": false,
  "crc32": "42ce7eed", "stored_in_slot": 2, "active": false }
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
| `409 NO_FREE_SLOT`         | All slots got used in the meantime                          |
| `409 DRIVE_BUSY`           | Stored (if `flash`) but not activated — see below           |
| `422 INVALID_IMAGE` / `UNSUPPORTED_GEOMETRY` | The content is not a supported `.ST` image (e.g. the boot sector contradicts the size) |
| `422 CHECKSUM_MISMATCH`    | Data does not match the given `crc32`                       |
| `500 FLASH_ERROR`          | Writing or verifying the slot failed                        |
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
| `name`           | always             | Disk name derived from `filename`               |
| `size`, `received` | always           | Announced and received byte count               |
| `destination`    | always             | `psram` or `flash`                              |
| `slot`           | if requested       | The requested slot                              |
| `activate`       | always             | Whether it will be / was activated              |
| `upload_url`     | state `ready`      | Where to `PUT` the data                         |
| `crc32`          | state `done`       | CRC-32 of the received image                    |
| `stored_in_slot` | written to flash   | The slot it was stored in (also on `DRIVE_BUSY`) |
| `active`         | state `done`       | It is now the active disk                       |
| `error`          | state `failed`     | `{ "code": ..., "message": ... }`               |

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
| Store an image in the first free slot          | `{"filename": "Game.st", "size": 368640, "destination": "flash"}` |
| Store it in slot 5 (overwriting) and play it   | `{"filename": "Game.st", "size": 368640, "destination": "flash", "slot": 5, "activate": true}` |

Add `"activate": true` to the second form to also play a stored image at
once.

### Guarantees

- The data is first received completely into memory. An image that is
  incomplete, invalid or does not match its `crc32` never touches a slot
  and never becomes the active disk.
- A flash image is read back and checked before its slot becomes `valid`.
- Without `slot`, an occupied slot is **never** overwritten.
- With `slot`, the old image in that slot is removed first. **If the upload
  then fails, or the power fails while writing, the old image of that slot
  is lost** — the slot shows `incomplete`, never a half image.
- A PSRAM upload replaces the previous PSRAM image only once the new one is
  complete and prepared. There is only one PSRAM image.

### DRIVE_BUSY

A new active disk is switched in only while the Atari is not using drive
B:. The device waits up to 3 seconds for such a moment. If the Atari keeps
the drive busy longer (for example while loading a game), the request ends
with `409 DRIVE_BUSY` and the old disk stays in the drive:

- `flash` upload: the image *is* stored (`stored_in_slot`) — activate it
  later with `PUT /api/v1/current`.
- `psram` upload: nothing is kept — upload it again later.

## Errors

Every error response has the same shape:

```json
{ "error": { "code": "NO_FREE_SLOT", "message": "all 20 slots are in use" } }
```

`code` is stable and meant for programs; `message` is for people and may
change. A failed upload carries the same `error` inside its upload object.

| HTTP | Code                   | What to do                                             |
| ---- | ---------------------- | ------------------------------------------------------ |
| 400  | `INVALID_REQUEST`      | Fix the request                                        |
| 400  | `INVALID_SLOT`         | Use a slot number 1–20                                 |
| 400  | `UPLOAD_INCOMPLETE`    | Start a new upload and send the complete file          |
| 401  | `UNAUTHORIZED`         | Send the correct token (only when one is configured)   |
| 415  | `UNSUPPORTED_MEDIA_TYPE` | Send `Content-Type: application/json` / `application/octet-stream` |
| 404  | `SLOT_EMPTY`           | Choose a slot with a valid image                       |
| 404  | `UPLOAD_NOT_FOUND`     | Start a new upload                                     |
| 409  | `UPLOAD_BUSY`          | Wait for the running upload (or its 60 s timeout)      |
| 409  | `UPLOAD_STATE`         | Start a new upload                                     |
| 409  | `NO_FREE_SLOT`         | Delete a slot, or give an explicit `slot` to overwrite |
| 409  | `DRIVE_BUSY`           | Retry when the Atari is idle (see above)               |
| 413  | `IMAGE_TOO_LARGE`      | Only images up to 819 200 bytes                        |
| 422  | `INVALID_IMAGE`        | The file is not a floppy image                         |
| 422  | `UNSUPPORTED_GEOMETRY` | Use an image with 80–84 tracks and 9–11 sectors        |
| 422  | `CHECKSUM_MISMATCH`    | The file was damaged in transit — send it again        |
| 500  | `FLASH_ERROR`          | Storage problem — retry; check the device log          |
| 500  | `PREPARE_FAILED`       | Internal problem — check the device log                |
| 503  | `FLASH_ERROR`          | Storage not available                                  |
| 507  | `INSUFFICIENT_MEMORY`  | Retry later / after a restart                          |

## Client examples

### Shell (curl)

```sh
DEV=192.168.178.62
TOKEN=your-api-token

# upload FILE DESTINATION [extra JSON, e.g. ',"slot":5,"activate":true']
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
upload "Crystal Castles.st" flash                          # store in first free slot
upload "Crystal Castles.st" flash ',"slot":5,"activate":true'   # slot 5, play it

# Play slot 1 / free slot 5
curl -s -X PUT -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
     -d '{"slot":1}' http://$DEV/api/v1/current
curl -s -X DELETE -H "Authorization: Bearer $TOKEN" http://$DEV/api/v1/slots/5
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
print(call("GET", "/api/v1/slots")[1])
```

A complete test suite that exercises every endpoint and error is in
`tests/api/api_test.py`.

## Limits and behaviour

| Item                          | Value                                             |
| ----------------------------- | ------------------------------------------------- |
| Image size / geometry         | up to 819 200 bytes; 80–84 cyl, 9–11 sectors, 1–2 sides |
| Flash slots                   | 20                                                |
| PSRAM images                  | 1                                                 |
| Uploads at the same time      | 1                                                 |
| Upload without data expires   | after 60 s                                        |
| Network timeout while sending | 10 s without data → `UPLOAD_INCOMPLETE`           |
| Wait for drive to be idle     | up to 3 s → else `DRIVE_BUSY`                     |
| Disk change signal to the Atari | 0.7 s "no disk"                                 |
| Typical duration              | Activate a slot: 360 KB ≈ 0.4 s, 720 KB ≈ 0.8 s; PSRAM upload 720 KB ≈ 3 s |
| Disk name                     | from `filename`, max. 39 printable characters     |

- The device handles one request at a time; a long upload delays other
  requests until it is done.
- The emulated drive keeps working while the API is used. Uploading or
  deleting never disturbs the disk the Atari is reading.
- Restarting the device removes the PSRAM image and inserts the stored
  image named *Crystal Castles*, else the first valid slot.

## Device configuration

WiFi and the API token are set in the firmware configuration and are stored
only in the local `sdkconfig` file, which is not part of the repository:

```sh
idf.py menuconfig        # → RadioFloppy: WiFi SSID, WiFi password, Hostname, API token
idf.py build flash
```

- No SSID: WiFi and the API are off.
- No token (default): the API is open on the local network.
- The device prints its address on the serial console at start-up:
  `WiFi: connected to "…", IP 192.168.178.62, API http://192.168.178.62/api/v1/status`.

If you set a token, choose a long random one, e.g. `python3 -c "import secrets; print(secrets.token_hex(16))"`.

## Implementation notes

For firmware developers; clients do not need this.

- Code: `main/api.c` (endpoints, uploads), `main/st_image.c` (image
  checks), `main/disk_image.c` (active disk, two PSRAM track buffers),
  `main/slot_store.c` (flash slots, see `docs/FLASH_LAYOUT.md`),
  `main/wifi_net.c`.
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
