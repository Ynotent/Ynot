# The `.ynot` universal project format (v1)

One format is used on all four platforms. A project saved on any device opens on any other
without losing recordings, arrangement, MIDI, automation or supported settings.

## Package layout

```
MySong.ynot/                    (a "package" document on macOS/iOS; a folder elsewhere)
  project.json                  UTF-8 JSON, keys in stable order
  media/<sha256>.wav            immutable 32-bit float WAV, named by content hash
```

- For transfer outside Ynot Cloud (AirDrop, email, USB) the package is zipped to `MySong.ynotz`.
- `project.json` is written atomically (temp file, then rename). A crash or a sync client can
  never leave a half-written project.
- Media is content-addressed:
  - Identical audio is stored once.
  - Cloud sync uploads each take once.
  - Every load verifies the hash, so a partial download is detected and never played as noise.
- Media paths are validated so they can't escape the package (`../` and absolute paths are
  rejected).

## What's stored

| Data | Representation | Notes |
|---|---|---|
| Tempo, time signature | `tempoBpm`, `timeSignature` | constant tempo in v1; tempo map planned for v2 |
| Positions & lengths | ticks, 960 per quarter | independent of sample rate |
| Audio recordings | `media[]` + `audioClips[]` | media keeps its native rate; offset in source frames |
| MIDI | `midiClips[].notes` as `[tick, length, pitch, velocity]` | compact for large MIDI files |
| Automation | `automation[]`, `target` + `[tick, value]` points | `track.gainDb`, `track.pan`, `plugin:<id>:<param>` |
| Mix | gain, pan, mute, solo, color, order | `order` is a fractional key so collaborators can insert between tracks |
| Plugins | `format`, `uid`, `version`, opaque `state` (base64), `params`, `seenOn` | the state chunk travels untouched even through devices that can't load the plugin |
| Freeze | `freeze{mediaId, sourceFingerprint, renderedOn, renderedAt}`, `frozen` | fingerprint = SHA-256 of the sound-defining parts of the track |

## Compatibility guarantees

1. **Forward compatibility.** Fields this version doesn't recognize are kept in `extras` and
   written back unchanged. An older app on one device never deletes data that a newer app on
   another device wrote.
2. **Backward compatibility.** `formatVersion` drives step-by-step migrations (`migrate()` in
   `project_format.cpp`). Every migration ships with a test fixture.
3. **Newer-major refusal.** If a project's format version is newer than the app supports, the
   app tells the user to update instead of guessing.
4. **No silent loss for plugins.** A plugin that can't load is reported (see `compatibility.hpp`).
   It is never removed from the project.

## Settings that are *not* in the project

Device-specific settings stay on the device and are never synced inside the project:
- audio device and buffer size;
- input routing and latency offsets;
- plugin scan cache.

Roaming user preferences (theme, key commands, default tempo, metronome sound) sync through
the account settings document, not the project. See SYNC_AND_COLLABORATION.md.

## Roadmap for the format

- v2: tempo and time-signature maps, buses/sends, take lanes and comping, markers.
- Compressed media (FLAC) alongside WAV, decoded by the core on every platform.
- Interchange: export to Standard MIDI File, stems and AAF/OMF are planned exporters. They are
  not part of the native format.
