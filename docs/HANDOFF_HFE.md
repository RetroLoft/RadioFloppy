# Handoff: HFE support (state at 2026-09-27)

For whoever continues this work (a new Claude session or a person). Read
this first; it replaces the chat history of the previous session.

## Where things stand

| Branch | Commit | State |
| --- | --- | --- |
| `main` | f46bc3d | Tested. HFE uploads are fully checked and stored as a stream, then **discarded** (`HFE_PLAYBACK 0` in `main/api.c`): no HFE image is kept until playback works. |
| `wip/hfe-player` | 47a1119 + this file | **Phase 2 (the HFE player), builds without warnings, never flashed or tested.** |

The board currently runs firmware built from `main`.

## HFE plan (phases, approved by the user)

1. **Parser / validator + streaming upload**: done (on `main`).
2. **Native timing player**: code written on `wip/hfe-player`, untested.
3. **HFEv3 opcodes in the player**: weak bits (RAND) still to do. Timing
   segments (BITRATE) are already handled by the phase 2 encoder.

After every phase: host tests plus the ST regression on the device
(`tests/api/api_test.py`, `tests/web/ui_test.py`). Then the user tests on
the Atari: HFEv1 first, then HFEv3, then copy-protected images.

## User requirements (keep these)

- The **ST route must stay completely unchanged**, including write support.
  The ST data encoder (`flux_encode`) and the 200 ms INDEX hardware loop
  are untouched; HFE uses separate encoders.
- **INDEX must sit on the real revolution boundary** of the flux stream and
  must never drift through an independent timer.
- Measure timing on real hardware; don't assume.
- HFE is always READ_ONLY and always stored compressed. HFEv2 is refused
  with a clear message; v1/v3 are detected from the header.
- Never accept an image that cannot be activated (track buffer fit is
  checked with the same packing code the player uses).
- An image becomes VALID in the catalog only after the complete upload,
  decompression, format check and CRC check.
- No STX/IPF/SCP now (STX research is parked). An SD card connector is
  planned for a later board; large images will go there.
- HFE test files: `/mnt/retroloft/retro/FLOPPIES/HFE` (1442 files, all
  HFEv1). 1436 pass, 6 are too large for the track buffer, none have an
  unsupported layout. No real HFEv3 files exist yet; the plan is to build
  HxC `hxcfe` and convert STX to HFEv3.

## How the pieces fit

- `main/hfe.c`: streaming reader (header, track table, one cylinder at a
  time in a 64 KiB region). It decodes into a **packed track buffer**: a
  slot table (`hfe_slot_t`, 168 slots) followed per track by cells (MSB
  first, 32-bit aligned), timing segments and weak areas. Checking a file
  runs the same packing in count-only mode.
- `main/hfe_import.c`: upload → CRC + early check + streaming deflate into
  reserved flash blocks (`image_store_writer_*`) → afterwards inflate from
  flash and check again → commit.
- `main/img_codec.c`: streaming deflate (worker task on core 1, ROM tdefl)
  and streaming inflate.
- **Phase 2 (this branch):**
  - `main/flux_stream.c`: `FLUX_MODE_HFE`, set with `flux_stream_set_mode`,
    which restarts both RMT channels in sync on an ST↔HFE disk change.
  - `hfe_encode`: plays the packed track at its own cell times and records
    the RMT tick of every revolution boundary in a ring buffer.
  - `hfe_index_encode`: puts the 3 ms pulse exactly on those ticks. While
    the next boundary isn't known yet it emits 10 µs filler steps, so it
    never runs further ahead than the data encoder. Late pulses are
    counted (`flux_hfe_stats`).
  - On a track change, the position in the new track follows from the time
    elapsed since the last boundary.
  - `main/disk_image.c`: `disk_prepare_hfe` / `build_hfe` inflate from flash
    straight into the inactive track buffer. `disk_tracks_kind` tells the
    encoders what the buffer holds. Start-up with an HFE disk is supported.
  - `main/disk_switch.c`: HFE images go through `disk_prepare_hfe`.
  - `main/api.c`: after an HFE upload, `activate` inserts the disk.

## Next steps

1. Flash `wip/hfe-player` with `HFE_PLAYBACK 1` in `main/api.c`. Check that
   an ST disk still works (ST regression tests), then upload and insert an
   HFE file.
2. **Measurement tool (not built yet):** capture INDEX and RDATA on the
   device itself and verify:
   - the INDEX period equals the track duration;
   - the first flux pulse after INDEX is at the same offset every revolution
     (no drift);
   - `late_pulses` stays 0;
   - the stream keeps running during an HFE upload (user requirement 3:
     compression and flash writes must not disturb emulation; PSRAM
     bandwidth contention is the risk).

   Idea: MCPWM capture on the INDEX pad, and on the INDEX edge briefly
   capture the first RDATA edges. Or loop RDATA back into the MFM decoder
   and count sector CRC errors.
3. Show the format (ST/HFE) in the API status / current and in the web UI.
4. Phase 3: weak bits (RAND) in `hfe_encode` (random cells per revolution
   inside the weak areas).
5. Only when this works: `HFE_PLAYBACK 1` on `main`, update the docs, then
   the user tests on the Atari.

## Known loose ends

- `tests/web/ui_test.py` is sometimes flaky: the drag-handle arrow step
  and the replace check each failed once and passed on the rerun.
- A power cut during an HFE upload was only tested on the host, not on the
  device.

## Working with this user and project

- Communication in Dutch; code, web UI and docs in English.
- Work phase by phase. After a finished step, report and **ask before
  starting the next phase**. "proceed" after a commit question means:
  commit only.
- Commit/push only when asked. "commit" has meant "commit and push".
  End commit messages with the Co-Authored-By line.
- Never do a full chip erase without permission. Never commit WiFi
  credentials or tokens (they live only in the git-ignored `sdkconfig`).
  Never pass client-provided paths to file functions. Never silently wipe
  or move images.
- Device: ESP32-S3 N16R8, API at `http://192.168.178.62` (DHCP, may
  change). Serial `/dev/ttyACM0`; opening the port resets the board.
  ESP-IDF 5.5.5 (`. ~/esp/esp-idf/export.sh`; `idf.py -p /dev/ttyACM0
  build flash`).
- Tests:
  - host: `tests/host/run.sh`;
  - device: `RADIOFLOPPY_HOST=<ip> python3 tests/api/api_test.py` and
    `tests/web/ui_test.py` (needs websocket-client and Chrome).
