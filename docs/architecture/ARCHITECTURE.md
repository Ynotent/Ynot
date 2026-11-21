# Ynot Architecture: one ecosystem on four platforms

Ynot is **one** music production product that runs on macOS, Windows, iOS/iPadOS and Android.
It is one codebase with one shared core and one project format, plus thin platform layers.
It is not four separate apps.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                          Ynot Cloud (platform-neutral)                       │
│  Accounts · Entitlements · Project op-log relay · Media blob store · AI jobs │
└───────────────▲───────────────▲────────────────▲───────────────▲─────────────┘
                │ HTTPS/WSS     │                │               │
┌───────────────┴──┐ ┌──────────┴───────┐ ┌──────┴─────────┐ ┌───┴────────────┐
│  macOS shell     │ │  Windows shell   │ │  iOS shell     │ │ Android shell  │
│  CoreAudio       │ │  ASIO / WASAPI   │ │ AVAudioSession │ │ AAudio (Oboe)  │
│  AU + AUv3 + VST3│ │  VST3            │ │ AUv3           │ │ AAP (flagged)  │
│  StoreKit / Web  │ │  Web checkout    │ │ StoreKit       │ │ Play Billing   │
│  mouse · keyboard│ │  mouse · kbd ·   │ │ touch · Pencil │ │ touch · kbd    │
│  MIDI            │ │  touch · MIDI    │ │ kbd · MIDI     │ │ MIDI           │
└────────▲─────────┘ └────────▲─────────┘ └──────▲─────────┘ └──────▲─────────┘
         │ implements PluginHost, AudioDevice, renders the UI per LayoutDecision │
┌────────┴────────────────────┴──────────────────┴──────────────────┴──────────┐
│                     ynot_core (portable C++20, this repo)                    │
│ model · project_format · engine · compatibility/freeze · collab · ui_layout  │
│ account/entitlements · AI gating · media (hashing, WAV) · platform profiles  │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Layers

### 1. Shared core: `core/` (done in Stage 17)
Portable C++20 with no OS, UI or framework dependencies. It compiles for all four targets.
Anything that has to *behave the same everywhere* goes here:

| Module | Responsibility |
|---|---|
| `model.hpp` | Project, tracks, audio/MIDI clips, automation, plugin instances, freezes. Time is kept in **ticks**, never samples, so projects don't depend on the device's sample rate. |
| `project_format.hpp` | The universal `.ynot` package, migrations, preserving unknown fields, content-addressed media. See [PROJECT_FORMAT.md](PROJECT_FORMAT.md). |
| `engine.hpp` | Block-based audio engine that doesn't allocate on the realtime thread. One render path serves live playback, bounce and freeze. Resamples media from its native rate. |
| `compatibility.hpp` | Per-device compatibility reports with warnings and resolution options. Also handles track freeze/unfreeze and "prepare for mobile". |
| `collab.hpp` | Operation log with hybrid logical clocks. Replicas converge deterministically, which gives real-time collaboration and offline multi-device sync. See [SYNC_AND_COLLABORATION.md](SYNC_AND_COLLABORATION.md). |
| `platform.hpp` | Capability profiles: the plugin formats, audio APIs, inputs and form factors of each platform, kept as data. |
| `audio_device.hpp` | The audio I/O interface each shell implements natively. `NullAudioDevice` is for tests and offline work. |
| `ui_layout.hpp` | Responsive layout decisions (Compact / Medium / Expanded / Studio), navigation pattern and touch-target sizes. All shells use them. |
| `account.hpp` | Merges purchases from several stores into one entitlement, picks the purchase channel per platform, and decides where each AI feature can run. |

### 2. Platform shells: `apps/<platform>/` (planned, see ROADMAP)
Each shell is a thin native layer that:
1. implements `AudioDevice` with the platform's optimized audio API;
2. implements `PluginHost` for the plugin formats that platform can load;
3. renders the UI from the shared design tokens and `decideLayout()`;
4. integrates the platform store, file system, background audio and permissions.

A shell should not contain project, mixing, sync or compatibility logic. If code would be
duplicated across two shells, it belongs in the core.

### 3. Cloud services: `services/` (planned)
These are platform-neutral HTTPS/WebSocket services. The op-log relay and the media store
don't need to understand audio. The entitlement service validates receipts from the App
Store, Google Play and web checkout server-side.

## Key cross-platform decisions

1. **Plugins are never assumed portable.** `platformLoadsFormat()` encodes the hard limits:
   - VST3 and AU binaries can't load on iOS.
   - Nothing desktop-native loads on Android.
   - AU is macOS-only.

   Even when a format is supported, the plugin must actually be installed on that device
   (`DeviceCatalog`).
2. **Freeze is the bridge.** A track that uses a plugin the target device lacks can be frozen
   on a device that has it. The freeze is a pre-fader render stored as content-addressed
   media. The plugin's opaque state still travels with the project, so the track can be
   unfrozen later back on the desktop. The track's fingerprint detects when a freeze is stale.
3. **Built-in instruments and effects are portable C++.** They sound identical on all four
   platforms and form the guaranteed-compatible baseline.
4. **One render path.** Live playback, offline bounce and freeze use the same `Engine`. A
   freeze made on a Mac therefore matches what the Mac played.
5. **Musical time and native-rate media.** iPhones typically run at 48 kHz and desktop
   interfaces at 44.1 or 96 kHz. Clips are positioned in ticks and resampled at playback.
   Production should replace the current linear interpolation with a windowed-sinc resampler
   (tracked in the ROADMAP).
6. **Collaboration works on the document, not the platform.** Ops target paths in the same
   JSON document the file format stores. Any client that can parse JSON, including a future
   web viewer, can join.

## UI strategy

See [ADR-0002](../adr/0002-ui-and-audio-framework.md). The core is framework-agnostic on
purpose, so the UI toolkit decision can't lock the product in. All shells share:
- **Design tokens** (`design/tokens.json`): color, type, spacing, motion and track colors.
  They keep one visual identity everywhere.
- **Layout decisions** (`ui_layout.hpp`): the same window size leads to the same workspace
  arrangement on every OS.
- **Platform-appropriate chrome**: menu bar on macOS, title-bar menus on Windows, bottom tabs
  on phones, sidebars on tablets. Touch targets follow each OS's guidelines (44 pt on Apple,
  48 dp on Android) and shrink when a precise pointer is attached.

## Input model

| Input | macOS | Windows | iOS/iPadOS | Android |
|---|---|---|---|---|
| Mouse / trackpad | primary | primary | iPad pointer | supported |
| Keyboard shortcuts | full | full | iPad hardware kbd | hardware kbd |
| Touch / multitouch | — | touchscreen laptops | primary | primary |
| Pencil | — | pen (basic) | Apple Pencil for editing | stylus (basic) |
| MIDI controllers | CoreMIDI | Windows MIDI Services / WinMM | CoreMIDI (USB/BLE) | android.media.midi |
| Pro audio interfaces | CoreAudio (aggregate) | ASIO, WASAPI exclusive | class-compliant USB | USB class-compliant (AAudio) |
