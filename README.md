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

The executable is at `build/Release/ScrubCut.exe` or `build/Debug/ScrubCut.exe`.

## Usage

Launch the app and drag-and-drop a video file onto the window.

### Keyboard shortcuts

| Action               | Hotkey                        |
|----------------------|-------------------------------|
| Play / Pause         | Space                         |
| Seek +/- 5s          | Left / Right                  |
| Seek +/- 1s          | Ctrl + Left / Right           |
| Seek +/- 30s         | Shift + Left / Right          |
| Frame step           | Alt + Left / Right  or  , / . |
| Speed up / down      | + / -                         |
| Jump to start / end  | Home / End                    |
| Mark In              | I                             |
| Mark Out             | O                             |
| Remove last segment  | Delete                        |
| Export segments      | Ctrl + E                      |
| Toggle help          | ?                             |

### Command-line flags

| Flag     | Description                              |
|----------|------------------------------------------|
| `-trace` | Enable performance tracing to `logs/`    |

## Tech stack

- **SDL3** -- windowing, audio, events
- **Dear ImGui** (docking) -- immediate-mode UI
- **FFmpeg** -- demuxing, decoding, encoding, filtering
- **OpenGL 3.3+** -- rendering
- **CMake + vcpkg** -- build system and dependency management

## License

[MIT](LICENSE)
