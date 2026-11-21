# Sync, handoff and real-time collaboration

There are three user-facing features and one mechanism behind them all:

| Feature | What the user sees |
|---|---|
| **Cloud sync** | Projects, recordings and settings are on every signed-in device. |
| **Handoff** | Start on iPhone, keep going on the studio Mac. The project opens where you left off. |
| **Real-time collaboration** | Several people on any mix of platforms edit one project live. |

The shared mechanism is the **operation log** in `core/include/ynot/collab.hpp`.

## Operation log

- Every edit becomes an `Operation` (`set`, `insert` or `remove`). It targets a path in the
  project's JSON document, e.g. `tracks[id=t1].gainDb`.
- Each op is stamped with a **hybrid logical clock** (wall time, counter, device id). Clocks
  stay causally ordered even when a device's system clock is wrong.
- Each replica applies the set of ops it knows in timestamp order. The same ops give the same
  document on every device, in any delivery order, including duplicates and late arrivals.
  The unit tests cover shuffled delivery across four simulated platforms.
- Conflict rules:
  - The latest write wins per field.
  - Remove wins over a concurrent edit.
  - Inserts never conflict, because ids are 128-bit random.

## Media

Audio never goes through the op log.
- A recording is first written locally.
- It is then uploaded as an immutable blob keyed by SHA-256.
- Only after that is it referenced by an `insert` op.

A collaborator may receive the clip op before the audio has arrived. The engine then reports
"not available on this device yet (still syncing?)" instead of failing, and plays the clip as
soon as the blob is downloaded and verified.

## Cloud services (planned, M3/M6)

| Service | Role |
|---|---|
| **Relay** (WebSocket) | Authenticates the session, assigns a server sequence number, fans ops out to collaborators and persists them. Clients send catch-up requests with `opsSince(lastKey)`. |
| **Compaction** | Once every collaborator has acknowledged a prefix, the server writes a snapshot `project.json`. Clients then `compact()`. |
| **Media store** | Object storage with presigned URLs, resumable uploads and per-project access control. |
| **Presence** | Who's online, their playhead, selection and soft-lock on the track being recorded. Presence is not part of the document. |

## Security

- TLS 1.3 for every connection.
- Short-lived OAuth access tokens, stored in Keychain on Apple, DPAPI/Credential Locker on
  Windows and Android Keystore on Android.
- Server-side encryption at rest for media and snapshots. Per-project ACLs (owner, editor, viewer).
- The relay validates every op:
  - path depth;
  - size limits;
  - protected keys (`id`, `format`, `formatVersion` are rejected; the core enforces this too).
- Media hashes are verified on download. Package paths are validated on load.

## Settings sync

Settings are split into two groups:
- **Roaming**: theme, key commands, metronome, default tempo, recent projects. They are stored
  in a per-account JSON document with the same `Replica` mechanism.
- **Device-only**: audio interface, buffer size, latency offsets, plugin scan results.

## Offline

- Every client is fully functional offline.
- Ops queue locally and merge on reconnect using the same rules.
- Media uploads resume where they stopped.
