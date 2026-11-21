# Roadmap & platform support status

All four platforms are part of the architecture from the start. They are delivered in
milestones, and **a platform is only called "supported" after native builds pass CI *and* the
real-device test matrix for that milestone has been completed and signed off.**

## Current status (Stage 17, plus M1 in progress)

Based on CI run #1 (commit `0fea6cc`, 2026-11-21) on GitHub-hosted runners:

| Platform | Status | Evidence |
|---|---|---|
| Windows | **native-shell-builds** | MSVC x64 (CI run #1): core builds and 52/52 tests pass; the JUCE app (WASAPI + VST3 hosting) builds, its 9 adapter tests pass, and the headless smoke test passes (including recording through the audio callback). |
| Android | **core-compiles** | Core cross-compiles with the NDK for arm64-v8a and x86_64. No Android app shell yet (M5). |
| macOS | **native-shell-builds** (M1 in progress) | CI run #9 (`macos-15`, Xcode 16.4, Apple Silicon): core builds universal (arm64 + x86_64) and 57/57 tests pass; the JUCE app builds, its adapter tests pass, and the headless smoke test passes **with live CoreAudio output**, audio + MIDI recording, monitoring and save/reopen. **First real device (2026-11-21):** on an M2 MacBook Pro (macOS 14.3.1, Command Line Tools only, Apple Clang 15), `tools/macos-test.sh` passed configure, build, 57 core tests, app adapter tests and the smoke test against the Mac's real CoreAudio devices. Hands-on testing found the first real-world issue: with the Mac's default output set to a virtual device (Pro Tools Audio Bridge, BlackHole...), Ynot played silently, and it didn't remember a device choice. Fixed: the device choice is now saved per machine, and the status line warns about virtual outputs. Hands-on results on the same M2 (2026-11-21), all confirmed working by the tester:
  - playback;
  - device choice remembered across launches;
  - live MIDI from the on-screen piano and computer keyboard;
  - MIDI recording with recording feedback;
  - Return to start;
  - MPC-style 62% swing quantize while recording;
  - punch recording replacing only the punched bar;
  - the 6/8 click.

  Not yet checked on hardware: recording alignment against the click (checklist step 5),
  monitoring latency (6), buffer sizes (3), plugin scan (10), save/reopen (11). |
| iOS / iPadOS | **native-shell-builds** (unsigned, simulator only) | CI run #6: core cross-compiles for arm64 devices, and the full JUCE app builds for the iOS simulator (Xcode 16.4, unsigned). It has never been signed, installed or run on an iPhone/iPad. |
| Linux (dev host only) | n/a | Core with ASan/UBSan and Clang `-Werror`, app build and smoke test pass in CI and locally. |

**macOS runners:** all four macOS-based jobs in run #1 were cancelled before any step ran. The
cause was our workflow: it requested the `macos-14` image, which GitHub retired on
2026-11-02. The workflow now uses `macos-15`. (The repository is public, so GitHub-hosted macOS
runners are free; billing was not the issue.)

None of these statuses means the app is usable on that platform. Windows is the furthest along,
but it has not been run with a real audio interface, plugin or user.

Status levels match `PlatformStatus` in `core/include/ynot/platform.hpp`:
`planned → core-compiles → native-shell-builds → real-device-verified`.

## Milestones

Each milestone ends with a gate, and nothing in a later milestone may claim the earlier gate
was passed if it wasn't.

### M0: Architecture and shared core ✅ (this stage)
- Architecture, platform capability model, universal project format v1.
- Audio engine; compatibility, freeze and render; collaboration op-log.
- Responsive layout rules, entitlements, AI gating.
- **Gate:** core tests are green on Linux ✅, Windows ✅ and Android (compile) ✅; macOS and iOS
  CI ⏳ (blocked by macOS runner availability).

### M1: macOS alpha (first native shell), in progress
Done so far (built and tested on the Linux dev host only):
- ADR-0002 resolved: JUCE 8.0.4.
- `JuceAudioDevice` (one `AudioDevice` for CoreAudio, ASIO/WASAPI, iOS Audio, Oboe).
- `JucePluginHost` (VST3/AU/AUv3) with machine-independent plugin ids and portable base64 state.
- `Session`: live engine swap with no locks on the audio thread; freeze and "Prepare for
  iPhone & Android" from the UI.
- Responsive main window driven by `decideLayout()` and `design/tokens.json`.

- **Audio recording:**
  - Lock-free capture on the audio thread.
  - A drain thread streams takes to disk, so long takes never sit in memory.
  - Dropped audio is reported if the disk can't keep up.
  - Latency compensation uses the driver's reported latency.
  - Punch-in replaces only the overlapped parts of existing clips.
  - Takes become content-addressed media.
  - Per-track arm and input selection. Input choices stay on the device and are never saved
    into the project.
  - Metronome on live output only, never in bounces or freezes.
  - Count-in (1 bar): the transport pre-rolls with a click and only the audio from the record
    point onward is kept.
  - Input monitoring: armed tracks are heard through their own inserts, fader and pan, with the
    transport stopped too. The status bar shows the round-trip latency the driver reports.
    Monitoring is a device setting and is off by default to avoid feedback through laptop and
    phone speakers.
  - Microphone permission handling for Android.

- **Arrangement view and editing:**
  - Ruler with bars and beats; playhead that follows playback.
  - Audio waveforms and MIDI note previews in every clip.
  - Move clips, including onto another track of the same kind; trim either edge; split by
    double-click, the S key or the menu; delete.
  - Snap to a grid that adapts to the zoom level.
  - Zoom with Cmd/Ctrl+wheel or trackpad pinch; scroll with Shift+wheel or horizontal wheel.
  - Undo/redo for every edit, including recordings and freezes.
  - On phones, each lane sits under its track header at full width, file actions are in a
    "More" menu, Freeze is in the track menu (tap the name), and the compatibility report is a
    summary button.
  - The edit operations live in the core (`edit.hpp`), so every platform edits identically.
    They are non-destructive: trims never change media or delete MIDI notes.
  - Verified on the Linux dev host by driving the real app with X11 mouse and keyboard events:
    move, undo, double-click split, edge trim and delete, then checking the saved project
    file. Touch gestures (long-press menu, two-finger pinch and pan) are implemented but
    can't be exercised without a touch device, so they are untested until M4/M5 hardware runs.

- **MIDI:**
  - Armed instrument tracks play live from every MIDI input (USB, Bluetooth, virtual ports)
    and from an on-screen keyboard, which matters on phones and tablets without a controller.
  - Events are placed sample-accurately via `juce::MidiMessageCollector`.
  - Recording an armed instrument track creates a MIDI clip. The core's `MidiTakeRecorder`
    captures on the audio thread without allocating, skips the count-in, and closes notes
    still held at stop. Re-struck notes and stray note-offs are handled, and overflow is
    reported.
  - Audio and MIDI tracks can record in the same take.
  - On phones the keyboard has finger-sized keys, and Count-in/Monitor move into "More" so the
    toolbar stays at two rows.

- **Quantize, swing and time signatures** (core `quantize.hpp`, identical on every platform):
  - Grids from 1/1 to 1/32 plus 1/4T, 1/8T and 1/16T triplets.
  - Drum-machine swing (50–75% on the second step of each pair); strength 25–100%.
  - Presets: straight, triplet, MPC-style sampler swing (54/58/62/66/71/75% on 16ths, 58/66% on
    8ths), analog drum-machine shuffle (light/medium/heavy) and vintage drum-machine feels.
    Preset names describe a style; the swing values are approximations, not emulations of any
    product's timing.
  - Applies while recording MIDI, or to an existing clip (undoable).
  - Time signatures 4/4, 3/4, 2/4, 5/4, 7/4, 6/8, 7/8, 9/8 and 12/8 drive the ruler, grid,
    click (beat = denominator note, accent on the downbeat), count-in and quantize bars.
    Tempo stays in quarter-note BPM.
- **Punch recording:**
  - Set the punch range by Shift+dragging in the ruler, from the playhead, or from a clip.
  - Record pre-rolls at least one bar and captures only between the in and out points.
  - Audio replaces just that part of the old take (latency-compensated at the out point); MIDI
    replaces only the notes inside the range.
  - Found while testing: a pending playhead seek could override the pre-roll position, so
    engine rebuilds now discard stale seeks.

- **Out-of-process plugin scanning:**
  - Each plugin file is inspected by a helper process: Ynot's own executable started with
    `--scan-one`.
  - A plugin that crashes or hangs (60 s timeout) only kills the helper. It's reported in the
    scan summary and skipped on later scans, and Ynot keeps running.
  - Scans run on a background thread with progress shown on the button.
  - Results are cached per machine, so launches don't rescan.
  - The smoke test exercises the success, crash and hang paths with real child processes.

- **Out-of-process plugin processing (sandboxing):**
  - Third-party plugins run in helper processes (`--plugin-sandbox`). Audio and MIDI pass
    through shared memory, and a named semaphore wakes the helper, so idle helpers use no CPU.
  - Processing happens in the same block, so no latency is added.
  - The audio thread waits at most 60% of a block for a result. A late plugin is skipped for
    that block (effects pass through, instruments go silent) without stalling the mix.
  - A crashed helper is detected, the plugin is bypassed, and the user is told. Plugins >
    Restart crashed plugins brings it back. Helpers exit if Ynot itself dies.
  - Settings: Plugins > "Run plugins safely (separate processes)", on by default.
  - Plugins are now kept alive across edits. Previously every edit, even toggling mute,
    reloaded every plugin.
  - Verified with real helper processes: identical output to in-process, parameters and state
    round-trip, crash detected and bypassed, and a too-slow plugin skipped without stalling
    (about 0.16 ms per call).
  - **Not yet verified with real third-party AU/VST3 plugins:** no CI machine has any
    installed. That's for the tester's Mac (checklist step 10 onwards).

Still to do:
- Plugin editor windows. For sandboxed plugins they will open in the helper's own window. Real-device plugin scan with the tester's AU/VST3 collection
  (checklist step 10).
- Loop/cycle recording with take lanes; count-in length choice (currently 1 bar).
- MIDI recording from controllers onto instrument tracks.
- Recording latency calibration tool (loopback test). Driver-reported latency is often wrong
  by a few milliseconds.
- MIDI: piano-roll editor; quantize on record (implemented in the core, no UI yet); sustain
  pedal/CC/pitch-bend recording (notes only today); MIDI device hot-plug (devices are enabled
  at audio start).
- Automation lane editing; automation lane editing; clip gain/fades; multi-select;
  copy/paste.
- Remove unreferenced media from the package after undo (orphaned takes are kept on disk today).
- Features: record, edit, arrange, mix, freeze, open and save `.ynot`.
- Replace linear resampling with windowed-sinc. Add a recording latency calibration tool.
- **Real-device gate:**
  - Apple Silicon (M1 or later) and one Intel Mac.
  - 3 audio interfaces (USB 2, USB-C, Thunderbolt) and the built-in audio.
  - 64-sample buffer stable for 1 h with 32 tracks.
  - Load and freeze tests with 20 popular AU/VST3 plugins.

### M2: Windows alpha
- Windows shell with ASIO, WASAPI exclusive and shared `AudioDevice`, and VST3 `PluginHost`.
- **Real-device gate:**
  - Intel and AMD laptop or desktop.
  - 3 ASIO interfaces plus one WASAPI-only device; ASIO4ALL not required.
  - A touchscreen laptop for the Medium layout.
  - The same plugin and stability suite as M1.
  - A project round trip between Mac and Windows with byte-identical `project.json`.

### M3: Cloud sync and handoff
- Accounts, media blob store, project op-log relay (single user, multiple devices), roaming settings.
- **Gate:**
  - Start a project on the Mac, continue on Windows, then back again with no data loss.
  - Offline edits on both sides merge correctly.
  - Security review and penetration test of the services.

### M4: iOS / iPadOS
- iOS shell: AVAudioSession, AUv3 hosting, Compact (iPhone) and Expanded (iPad) layouts,
  Pencil, pointer.
- "Prepare for iPhone": freeze desktop-only tracks on the Mac before handoff.
- **Real-device gate:**
  - iPhone SE (3rd gen), a current iPhone, a base iPad, and an iPad Pro with keyboard and trackpad.
  - Wired, USB-C interface and Bluetooth routes.
  - Interruption tests (calls, Siri, alarms).
  - Desktop-to-iPhone handoff of a project with VST3/AU tracks shows correct warnings and plays
    the freezes.

### M5: Android
- Android shell: Oboe/AAudio `AudioDevice`, built-in plugins, Compact and Expanded layouts.
- AAP evaluation stays behind a feature flag.
- **Real-device gate:**
  - At least 8 devices across Pixel, Samsung (phone and tablet), and one mid-range and one
    low-end device.
  - Measured round-trip latency recorded for each device.
  - A USB class-compliant interface.
  - Handoff tests with all three other platforms.

### M6: Real-time collaboration
- Relay fan-out, presence, soft-locks during recording, permissions.
- **Gate:** a live session with four collaborators, one on each platform, for 30 minutes under
  packet loss and reconnects. All replicas end byte-identical.

### M7: AI features
- On-device: chord and key detection, vocal tuning.
- On-device or cloud: stem separation, audio-to-MIDI, mix and mastering assistants.
- Cloud: text-to-MIDI ideas.
- **Gate:** each feature is verified on every platform where `aiAvailability()` says it runs.

### M8: Subscriptions on every store
- StoreKit 2, Play Billing and web checkout, with the server-side entitlement service.
- **Gate:**
  - A purchase on each channel unlocks all four platforms.
  - Refund, grace-period and expiry flows work on each store's sandbox.
  - Legal review of in-app purchase rules for each launch region.

## Known gaps to fix before any release
- Linear-interpolation resampler (quality) → windowed-sinc in M1.
- Constant tempo only → tempo map in format v2.
- No buses or sends yet → format v2.
- `Replica::rematerialize()` is O(n) on out-of-order ops. That's fine for sessions; add
  periodic snapshots in M6.
- JSON number parsing uses `strtod`. Shells must not change the C locale (`LC_NUMERIC`), or a
  locale-independent parser has to be substituted.
