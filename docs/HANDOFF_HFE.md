# Handoff: HFE support (state at 2026-09-28)

For whoever continues this work (a new Claude session or a person). Read
this first; it replaces the chat history of the previous session.

## Where things stand

| Branch | Commit | State |
| --- | --- | --- |
| `main` | latest | **`wip/hfe-player` merged (2026-09-29).** Phase 2 (the HFE player) with `HFE_PLAYBACK 1`: ST regression passes, HFEv1 and HFEv3 games load on the Atari. Timing not yet measured. |
| `wip/hfe-player` | same as `main` | Kept for reference; new work starts from `main`. |

The board runs firmware built from `main`. Work on board revision 1 is
paused; the next steps below still apply, next to the preparation for
board revision 2 (DS0/DS1 pull-ups; being manufactured).

## First device test of phase 2 (2026-09-28)

- ST regression on the new firmware: `api_test.py` 68 ok / 0 FAIL,
  `ui_test.py` 49 ok / 0 FAIL. Stored images unchanged.
- Start-up with an HFE disk inserted works: `HFE tracks: HFEv1, 80
  cylinders, 2 side(s), 250 kbit/s, 1958 KiB of track buffer, 2106 ms`.
- HFE uploads with `activate: true` take ~12 s for 2 MB (stored ~390 KB
  deflated). An upload while the Atari reads the drive gives
  `409 DRIVE_BUSY` (stored, not inserted), as designed.
- Test files: 75 HFEv1 files converted from STX (`..._stx.hfe`, the "P"
  set of the TOSEC Atari ST collection), in the user's OneDrive folder
  `retro/P` next to this repo.

| Image (id on the board) | Disk layout | Result on the Atari |
| --- | --- | --- |
| Pang [cr Bad Brew Crew] (185) | own loader, no BPB (boot sector filled with E5) | **works** |
| Pink Panther (187) | normal, 10 sectors, side 0 only, 82 cylinders | **works** |
| P-47 (184) | copy-protected: tracks 2–38 have no standard sectors (1–2 syncs + one long block, 100192–100208 cells); root directory is random bytes | boots to the intro screen, **hangs** after it |
| Pac-land (186) | normal GEMDOS, 11 sectors, side 0 only | **works** (file names correct in GEM) |
| Pang, later in the game | track 36 of this file lacks sector 2 (only 720 bytes of room where a 1024-byte sector belongs): the image is damaged | hangs at track 36, as any emulator would |
| **Klax (HFEv3)** (217) | flux dump (Greaseweazle SCP → HxC HFEv3), see below | **works** |

P-47 is the test case for the measurement tool: the image itself looks
complete (valid MFM on all tracks), so either the STX→HFEv1 conversion
lost something (e.g. weak bits) or the player's timing / track-change
position is off on these long custom tracks.

### HFEv3 from flux dumps (Klax)

Source: the Internet Archive item `Klax_Domark_AtariST_DiskImage`; the
7z holds SCP dumps, two STX conversions,
an HxC HFEv3 and an Aufit protection report. Local copy in OneDrive
`retro/HFEv3`. Protection report: tracks 0–4 have fuzzy sectors 11/12,
sector-within-sector, data over the index and no-flux areas.

What the HFEv3 contains, and how it is handled now (`main/hfe.c`):
- 2200–3600 `BITRATE` opcodes per track, values 70–75 around 72: the
  speed jitter of the dumping drive. A track with more changes than
  `HFE_MAX_SEGMENTS` (now 32) is **smoothed**: equal zones, each at the
  average cell time of its cells, the division remainder carried to the
  next zone, so the revolution keeps its duration (< 0.1 us off).
- `INDEX` repeated 16–24 cells before the track end (same pulse at the
  end of the revolution): ignored in the last 1/64 of the track; a repeat
  elsewhere is still refused.
- Tracks 80–82 are unformatted noise (thousands of `RAND`): weak areas
  beyond the limit are joined.
- The fuzzy sectors are **not** encoded as `RAND` in this file, so it
  does not test weak bits. A weak-bit test file must still be made (e.g.
  from the Aufit STX in the same archive, with HxC).
- Needs 2067 KiB of track buffer (2132 KiB available); stored 657 KB.
- Decision for later (SD card board): keep smoothing; stream tracks per
  cylinder from the card and raise `HFE_MAX_SEGMENTS` instead of playing
  every jitter step.

### Other changes in this round

- **Disk change while the drive is selected** (`drive_swap_media`): after
  0.3 s a disk is also changed while the Atari keeps drive B: selected
  (many games do; with the Atari off the select line floats low on board
  revision 1, which lacks the DS0/DS1 pull-ups). Refused only for a
  writable disk while WGATE is active or after a write since the drive
  was selected; then `DRIVE_BUSY` after 3 s. The 0.7 s disk change signal
  keeps INDEX/RDATA off, so no half sector of the new disk is read.
  Needs testing on the Atari (game holding the drive, Atari off,
  multi-disk game).
- Web UI: success messages disappear after 15 s; an upload refused by the
  device stops the progress bar; "Load temporarily" is disabled for HFE.

Cosmetic issues seen:
- Titles keep the converter suffix: "P-47 _stx" (TOSEC tags are removed,
  `_stx` and the space before it are not).
- The boot log prints the ST lines (`Geometry: 80/2/0/512`, `MFM: ...
  100000 bitcells/track, GAP3 84`, `RPM: 300 (200 ms, INDEX 3 ms)`) also
  for an HFE disk; `/current` reports `"sectors": 0` and no format.
- `flux_hfe_stats` (`late_pulses`) is not printed or exposed anywhere yet.

## HFE plan (phases, approved by the user)

1. **Parser / validator + streaming upload**: done (on `main`).
2. **Native timing player**: on `wip/hfe-player`; HFEv1 and HFEv3 games
   load on the Atari. Timing not yet measured.
3. **HFEv3 opcodes in the player**: weak bits (RAND) still to do (stored in
   the packed track, not yet played). Timing segments (BITRATE) work,
   including smoothed flux dumps.

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

1. ~~Flash `wip/hfe-player` with `HFE_PLAYBACK 1`, ST regression, upload
   and insert an HFE file.~~ Done 2026-09-28, see above.
2. **Measurement tool (not built yet)**, with P-47 as the failing case: capture INDEX and RDATA on the
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
  - host: `tests/host/run.sh` (extra real HFE files via `HFE_TEST_FILES`;
    on the Windows laptop run it from Git Bash with
    `PATH=/c/msys64/ucrt64/bin:$PATH`);
  - device: `RADIOFLOPPY_HOST=<ip> python3 tests/api/api_test.py` and
    `tests/web/ui_test.py` (needs websocket-client and Chrome; set
    `CHROME` to the Chrome executable where it isn't `google-chrome`).
  - `api_test.py` needs `images/CRYSTAL_CASTLES.ST` (git-ignored); it can be
    downloaded from the board (`GET /api/v1/images/1/data`, CRC 42ce7eed).
  - The tests leave the first original image active, not the one that was
    active before.
- Second machine: Windows laptop, same network. ESP-IDF 5.5.5 in
  `C:\Users\frank\esp\esp-idf` (`. C:\Users\frank\esp\esp-idf\export.ps1`),
  build dir outside OneDrive: `idf.py -B C:\Users\frank\esp\build-radiofloppy
  -p COM3 build flash`. Serial port COM3 (CH343). Its `sdkconfig` has no WiFi
  credentials; the board uses the ones saved on its external flash. Turn
  off the WireGuard `retroloft` tunnel there: it also routes
  192.168.178.0/24 and hides the board.
