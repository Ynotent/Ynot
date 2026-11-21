# ADR-0001: A shared, framework-agnostic C++ core

**Status:** Accepted (Stage 17)

## Context
Ynot has to run on macOS, Windows, iOS and Android with one coordinated codebase. Realtime
audio has to run without garbage collection or locks, so the audio engine needs native code.
The project model, file format, compatibility rules, sync and entitlement logic must behave
*identically* on all four platforms. Otherwise projects would drift apart between devices.

## Decision
- All product logic lives in `ynot_core`: portable C++20 with no OS, UI or third-party framework
  dependencies. Every platform shell links it.
  - Swift/ObjC++ link it directly.
  - Android uses JNI.
  - Windows links it natively.
- Platform specifics live behind narrow interfaces:
  - `AudioDevice` (audio I/O);
  - `PluginHost` (plugin formats);
  - the UI.
- Platform differences are recorded as data (`PlatformProfile`), not as `#ifdef`s spread
  through the logic.

## Consequences
- Every core behavior is unit-tested once on a Linux CI host, and the same code is
  cross-compiled for all targets.
- Shells stay thin. If logic appears in two shells, it moves to the core.
- Swapping the UI or audio framework (ADR-0002) doesn't touch the core.
