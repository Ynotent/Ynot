# Ynot: one music production ecosystem on macOS, Windows, iOS and Android

Record, edit, produce, mix and collaborate across your devices. A project started on an
iPhone continues on a studio Mac, a Windows laptop or an Android tablet. It's one product with
one account and one project format, not four separate apps.

> **Platform status:** the architecture covers all four platforms, but **no platform app is
> released or supported yet.** Stage 17 delivers the architecture and the shared core that
> every platform will use. See [docs/ROADMAP.md](docs/ROADMAP.md) for exactly what is verified.

## What's in this repo

| Path | Contents |
|---|---|
| `core/` | **`ynot_core`**, the portable C++20 library shared by every platform: project model, universal `.ynot` format, audio engine, compatibility and freeze, real-time collaboration, responsive layout rules, entitlements and AI gating. |
| `core/tests/` | Unit tests (31), dependency-free |
| `tools/ynot-cli/` | `ynot` CLI: create, inspect, compatibility-check and render projects |
| `apps/{macos,windows,ios,android}/` | Plans for each platform's native shell (not built yet) |
| `design/tokens.json` | Shared visual identity (colors, type, spacing, breakpoints) |
| `docs/` | Architecture, format spec, sync/collaboration, billing, platform plans, ADRs, roadmap |

## Build and test

Requires CMake 3.22 or later and a C++20 compiler (GCC 11+, Clang 14+, MSVC 2022, Xcode 15+).

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Try the cross-device compatibility check:

```sh
./build/ynot demo /tmp/Demo.ynot          # project with a VST3 effect and an AU instrument
./build/ynot check /tmp/Demo.ynot ios windows
./build/ynot render /tmp/Demo.ynot /tmp/demo.wav 48000
```

## Documentation

- [Architecture](docs/architecture/ARCHITECTURE.md): layers, shared versus platform code, key decisions
- [Platform plans](docs/architecture/PLATFORMS.md): CoreAudio/AU/VST3, ASIO/WASAPI/VST3, AVAudioSession/AUv3, AAudio
- [Project format](docs/architecture/PROJECT_FORMAT.md): the `.ynot` package and its compatibility guarantees
- [Sync and collaboration](docs/architecture/SYNC_AND_COLLABORATION.md)
- [Accounts and billing](docs/architecture/ACCOUNTS_AND_BILLING.md)
- [ADR-0001: shared C++ core](docs/adr/0001-shared-cpp-core.md) · [ADR-0002: UI and plugin framework (decision needed)](docs/adr/0002-ui-and-audio-framework.md)
- [Roadmap and support status](docs/ROADMAP.md)
