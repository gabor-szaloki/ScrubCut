# ScrubCut

A lightweight video player for fast frame-accurate scrubbing, trimming, and exporting.

## Features

- **Fast scrubbing** -- drag the timeline bar or use keyboard shortcuts to seek instantly
- **Frame stepping** -- step forward/backward one frame at a time
- **Trimming** -- mark multiple in/out segments, export them as separate files
- **Export formats** -- original format (stream copy, no re-encoding) or GIF
- **Variable speed** -- 0.25x to 4x playback
- **Volume control** -- mute button and volume slider

## Prerequisites

- **CMake** 3.21+
- **C++17** compiler (MSVC, GCC, or Clang)
- **Git** (for cloning and vcpkg submodule)

## Setup

Clone the repository with the vcpkg submodule:

```bash
git clone --recurse-submodules https://github.com/gabor-szaloki/ScrubCut.git
cd ScrubCut
```

If you already cloned without submodules:

```bash
git submodule update --init
```

Bootstrap vcpkg:

```bash
# Windows
vcpkg\bootstrap-vcpkg.bat

# Linux / macOS
./vcpkg/bootstrap-vcpkg.sh
```

## Build

Configure (downloads and builds dependencies via vcpkg on first run):

```bash
cmake --preset default
```

Build:

```bash
# Release
cmake --build --preset release

# Debug
cmake --build --preset debug
```

The executable is at `build/release/bin/ScrubCut[.exe]` (or `build/debug/bin/` for the debug build). On macOS this is a bare binary — see [Packaging](#packaging) to produce a `.app` bundle.

## Packaging

### Windows installer (NSIS)

Install [NSIS](https://nsis.sourceforge.io/) (`winget install NSIS.NSIS`), then from the configured build dir:

```bash
cpack -G NSIS --config build/release/CPackConfig.cmake -B build/release
```

This produces `build/release/ScrubCut-<version>-win64.exe`, which installs ScrubCut, bundled DLLs, a desktop shortcut, and registers the app as an "Open with" handler for common video extensions.

### macOS .app bundle

The `.app` is produced by the **install** step, not a plain build:

```bash
cmake --install build/release --prefix <output-dir>
```

`<output-dir>/ScrubCut.app` can be dragged to `/Applications`. The bundle is ad-hoc codesigned (runs locally without a developer certificate), so Gatekeeper will show a first-launch warning. No DMG is produced.

## Usage

Launch the app and drag-and-drop a video file onto the window, or press **Ctrl/Cmd + O** to open one via a file dialog.

### Keyboard shortcuts

Where two modifiers are shown separated by `/`, the first is Windows and the second is macOS.

| Action                      | Hotkey                                      |
|-----------------------------|---------------------------------------------|
| Play / Pause                | Space                                       |
| Seek +/- 5s                 | Left / Right                                |
| Seek +/- 1s                 | Ctrl/Option + Left / Right                  |
| Seek +/- 30s                | Shift + Left / Right                        |
| Frame step                  | Alt/Cmd+Option + Left / Right  or  , / .    |
| Speed up / down             | + / -                                       |
| Jump to start / end         | Home / End  (also Cmd + Left / Right on macOS) |
| Mark In                     | I  or  [                                    |
| Mark Out                    | O  or  ]                                    |
| Remove last segment         | Delete / Backspace                          |
| Open file                   | Ctrl/Cmd + O                                |
| Export segments             | Ctrl/Cmd + E                                |
| Toggle timeline panel       | Ctrl/Cmd + T                                |
| Toggle segments panel       | Ctrl/Cmd + S                                |
| Toggle fullscreen           | F                                           |
| Toggle UI visibility        | H                                           |
| Exit fullscreen / show UI   | Esc                                         |
| Quit                        | Alt+F4 / Cmd + Q                            |
| Toggle help                 | ?                                           |

### Command-line flags

| Argument         | Description                                                     |
|------------------|-----------------------------------------------------------------|
| `<path>`         | Open the given video file at launch                             |
| `-trace`         | Write performance traces to `scrubcut_trace.csv` in the app data dir |
| `-log`           | Allocate a console for stdout/stderr (Windows only)             |
| `-resetlayout`   | Reset the docked panel layout to defaults                       |
| `-profileseek`   | Enable seek-performance profiling                               |

## Tech stack

- **SDL3** -- windowing, audio, events
- **Dear ImGui** (docking) -- immediate-mode UI
- **FFmpeg** -- demuxing, decoding, encoding, filtering
- **OpenGL 3.3+** -- rendering
- **CMake + vcpkg** -- build system and dependency management

## License

[MIT](LICENSE)
