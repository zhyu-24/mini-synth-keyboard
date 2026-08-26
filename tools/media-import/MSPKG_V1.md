# MSPKG v1 — Mini Synth Package

`*.mspkg` is the transport container for the nine-key ESP32-S3 device. Version 1 defines `SEQUENCE`; `AUDIO` reserves the same container but is not enabled until MP3 playback is physically validated.

All integer fields are little-endian. Strings are UTF-8. CRC uses IEEE CRC-32 as implemented by Python `zlib.crc32`.

## Container header — 32 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `char[4]` | magic `MSPK` |
| 4 | `uint8` | format major = 1 |
| 5 | `uint8` | format minor = 0 |
| 6 | `uint8` | content type: 1 = sequence, 2 = audio |
| 7 | `uint8` | flags; v1 = 0 |
| 8 | `uint32` | header bytes = 32 |
| 12 | `uint32` | metadata bytes |
| 16 | `uint32` | payload bytes |
| 20 | `uint32` | metadata CRC-32 |
| 24 | `uint32` | payload CRC-32 |
| 28 | `uint32` | minimum firmware ABI; v1 = 1 |

The header is followed by metadata TLVs and then payload bytes.

## Metadata TLV

Each record begins with `<uint16 tag, uint8 type, uint8 flags, uint32 length>`, followed by `length` value bytes.

Types: 1 UTF-8, 2 `uint32`, 3 `int32`, 4 raw bytes, 5 boolean byte.

Tags used by sequence v1:

| Tag | Meaning |
|---:|---|
| 1 | title |
| 2 | creator/source label |
| 3 | original file format |
| 4 | device profile ID |
| 5 | source SHA-256 raw 32 bytes |
| 16 | ticks per quarter |
| 17 | note event count |
| 18 | duration ticks |
| 19 | recommended playback transpose in semitones (`int32`) |
| 20 | original minimum MIDI pitch |
| 21 | original maximum MIDI pitch |
| 22 | peak active polyphony |
| 23 | seven-natural-key teaching compatibility (`bool`) |
| 24 | conversion summary UTF-8 |
| 25 | preferred timbre UTF-8: `SINE`, `8BIT`, `ORGAN`, `PIANO_SYNTH`, or future registered value |

The preferred timbre is a hint. The device may let the user override it without reconverting the song. `PIANO_SYNTH` means a synthesized approximation; it does not require piano sample assets.

Unknown tags must be skipped using their length.

## Sequence payload

### Payload header — 28 bytes

`<4s HH III HH I>`

| Field | Meaning |
|---|---|
| magic | `MSQ1` |
| ticks per quarter | v1 default 480 |
| note event record bytes | 12 |
| note event count | count after tied notes are merged |
| tempo record count | number of tempo changes |
| duration ticks | end of last note/rest timeline |
| time-signature count | number of time-signature changes |
| reserved | 0 |
| reserved | 0 |

### Tempo record — 8 bytes

`<uint32 tick, uint32 microseconds_per_quarter>`

### Time-signature record — 8 bytes

`<uint32 tick, uint8 numerator, uint8 denominator_power_of_two, uint8 midi_clocks, uint8 thirty_seconds_per_quarter>`

For 4/4, denominator power is 2 because `2^2 = 4`.

### Note event — 12 bytes

`<uint32 start_tick, uint32 duration_tick, uint8 midi_pitch, uint8 velocity, uint8 voice, uint8 flags>`

- MIDI pitch uses the standard 0–127 equal-tempered note number.
- Velocity v1 defaults to 100 when the source has no reliable dynamics.
- Voice is a compact zero-based source voice index.
- Flags v1 = 0; future versions may define articulation or teaching roles.
- Events are sorted by start tick, voice, and MIDI pitch.

## Conversion principles

- Source pitch is preserved in event records. Recommended transpose is metadata and can be accepted or overridden by the player.
- Tied notes are merged before packaging.
- Repeats must be expanded by the converter or rejected; v1 does not store notation repeats.
- MusicXML layout, lyrics, engraving, pedal, arbitrary ornaments, and unsupported notation do not enter sequence payload.
- A converter must report any timing quantization, pitch modification, voice deletion, or unsupported feature. It must not silently replace chromatic notes with natural notes.
- `MASTER_PEAK=18000` is a firmware output ceiling, not a package field and cannot be overridden by imported media.
