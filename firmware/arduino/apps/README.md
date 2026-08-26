# Mini Synth Keyboard applications

[简体中文](README.zh-CN.md)

## `mini_synth_v1`

Hardware-accepted playable firmware with the multi-app shell, 30-song file library, persistent Settings, global BLE game keyboard, and MUSB v1 storage management.

### Play and audio

- Seven active-low note keys play C-D-E-F-G-A-B and support chords. A dedicated 1 kHz input task supplies 5 ms non-blocking debounce; a dedicated 4 ms audio task continuously feeds I2S.
- Play has two octave modes selected in Settings:
  - `HOLD` (default): hold SW8 for C3, neither for C4, or SW9 for C5. Holding both remains the global Home/BLE gesture and does not select an octave.
  - `LATCH`: release a single SW8 press to move one octave down or a single SW9 press to move one octave up, clamped to C3-C5. The latched octave survives app switches during the current boot and resets to C4 after reboot. If both buttons are used for Home/BLE, no latch step is committed.
- Already sounding Play notes retain the confirmed 12 ms smoothstep glide when the octave changes. New attacks start directly at the selected octave.
- Play timbre is selected only in Settings; SW8/SW9 no longer implement Play double-click timbre cycling. Song keeps its independent SW8/SW9 timbre controls.
- Pure SINE remains on the confirmed `MASTER_PEAK=18000` and exact 21-pitch attenuation table: low C3-B3 all 0 dB; middle C4-B4 `{-1,-2,-3,-4,-5,-5,-6}` dB; high C5-B5 `{-8,-11,-14,-14,-15,-17,-18}` dB.
- For `8BIT`, `ORGAN`, and `PIANO` only, MIDI 60-71 receives +3 dB and MIDI 72+ receives +6 dB relative to the prior calibrated curve; pitches below MIDI 60 are unchanged. This applies consistently to Play and Song. Chord normalization uses the original calibrated envelope so the requested relative boost is retained, followed by the final safety clamp.
- Global volume is applied to Play and Song after mixing/normalization and before the final clamp and `MASTER_PEAK`. It ranges from 0% to 100% in 10% steps; 100% preserves the accepted output path.
- After 100 ms of valid digital silence at startup, NS4168 is enabled and remains enabled to avoid an amplifier-enable transient on every key press. Cold start does not wait for USB Serial.

### Home, Settings, and app shell

- Firmware still boots directly into Play. Holding SW8+SW9 for 1 second opens Home. C/D/E/F/G/A/B select Play/Song/Loop/Beat/Pet/Keyboard/Game, SW8 opens Settings, and SW9 resumes the last resumable app. Input is suppressed until all nine keys are released after every app transition.
- Settings is a functional persistent page backed by a dedicated `Preferences` NVS namespace, unrelated to BLE bonds. Its compact versioned record is validated before use; absent, corrupt, or incompatible data falls back to volume 100%, octave mode HOLD, and Play sound SINE.
- Settings controls: C/D select the previous/next row, E/F decrease/increase the selected value, and G resets that row to its default. The rows are `VOLUME`, `OCT MODE`, and `PLAY SOUND`. Holding SW8+SW9 returns Home as on every page.
- Loop, Beat, Pet, and Game remain safe `COMING SOON` placeholders and publish digital silence.

### Song catalog and controls

- Song exposes up to 30 valid MSPKG v1 `SEQUENCE` files (`.mspkg` or `.msp`) from the FFat root. Ordering is deterministic, case-insensitive filename order; the music-library manager uses `01-` through `30-` prefixes to preserve user order.
- File-song catalog entries retain only display metadata, source identity, duration/event count, and path. At steady state, only the currently selected file song owns an allocated `SongEvent[]`; all other files are fully validated during scan and immediately release their temporary event arrays. The four embedded recovery sequences remain immutable flash arrays.
- E/F stops playback, changes selection, and asynchronously requests loading from `loopTask`. The audio task never calls FFat and never blocks. The existing audio catalog gate is also used for selection loads: audio acknowledges a safe boundary before the old selected array is freed and the replacement is installed. OLED shows `LOAD` or `FAILED`; C and D cannot start playback until loading succeeds.
- Boot selects index 0 and loads it when valid file songs exist. If FFat is absent, damaged, unmounted, empty, or has no valid package, the four embedded songs appear as fallback.
- The Song page preserves status, title, timbre, time progress, and controls while adding a zero-padded index/total such as `01/30`.
- Song controls: C toggles play/pause, D restarts, E/F select previous/next without autoplay, SW8/SW9 cycle Song timbre, and holding SW8+SW9 returns Home.
- The loader mounts with `FFat.begin(false)` and never formats automatically. It verifies container sizes, metadata/payload CRC-32, source-hash duplicates, event count, pitch/timing order, and actual peak polyphony.

### BLE and USB invariants

- BLE remains a global service rather than a Keyboard-owned connection. Holding SW8+SW9 for 1 second returns Home; continuing to 3 seconds opens the existing 30-second switch-device window. The firmware preserves bonds, temporarily rejects only the peer explicitly left, and keeps the `B-` / `B30..B01` / `B+` badge behavior on every page.
- Keyboard remains the same two-byte NKRO BLE HID game keyboard: top SW1/SW2/SW3/SW8 = Q/W/E/R, bottom SW4/SW5/SW6/SW7 = A/S/D/F, SW9 = Left Alt, with Alt suppressing R, release-all on exit, reconnect all-up gating, and BLE API calls isolated to the low-priority BLE task.
- MUSB v1 commands and payloads are unchanged. Upload still uses a hidden temporary FFat file, validates before commit, and uses backup+rename rollback. Upload/delete refresh now rescans up to 30 songs and leaves a valid selected song loaded behind the same atomic audio gate. `INIT_STORAGE` remains the only explicitly confirmed format path and affects only the FFat music partition.

Required libraries:

- `MiniSynthBoard` (project library, or installed ZIP)
- `U8g2` by oliver
- `ESP_I2S`, `BLE`, `FFat`, and `Preferences` from Arduino-ESP32 3.3.11

The production application intentionally does not wait for USB Serial or emit large synchronous startup/key logs; those previously caused long cold starts and OLED latency under USB backpressure. Compile with the exact project FQBN in `config/board.ps1`, then verify cold-start time, OLED response, settings persistence, octave behavior, 30-song browsing/loading, audio balance, BLE switching, and MUSB refresh on hardware. Stop immediately for resets, USB disconnects, obvious distortion, odor, rapid heating, or unstable TP2 voltage.
