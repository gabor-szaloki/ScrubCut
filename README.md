# ScrubCut

A lightweight video player for fast frame-accurate scrubbing, trimming, and exporting.

<video src="https://github.com/user-attachments/assets/4800c503-a0df-4697-b780-9de4a54129db" controls width="720"></video>

## Install

Prebuilt Windows and macOS installers are available on the [Releases page](https://github.com/gabor-szaloki/ScrubCut/releases).

## Features

### Scrub

- **Fast scrubbing** -- drag the timeline bar or use keyboard shortcuts to seek instantly
- **Frame stepping** -- step forward/backward one frame at a time

### Cut

- **Trimming** -- mark multiple in/out segments, export them as separate files
- **Frame grabbing** -- mark individual frames, export them as PNG stills
- **Export formats**
  - Segments: original format (stream copy, no re-encoding) or GIF. MKV/WebM sources are remuxed into MP4 so the cuts are frame-accurate at both ends — the bitstream is bit-identical, just a different container. GIF sources only support GIF exports.
  - Frames: PNG stills.

### Generic video player features

ScrubCut can serve as your basic go-to video player app — you can find most basic features, like:

- **Variable speed** -- 0.1x to 8x playback
- **Volume control** -- mute button and volume slider
- **Audio tracks** -- switch between multiple embedded audio tracks
- **Subtitles** -- show embedded subtitle tracks, or load an external subtitle file (SRT, ASS/SSA, WebVTT, SUB) via the menu or drag-and-drop; adjustable text size and timing offset
- **Input formats** -- MP4, MKV, MOV, AVI, WebM, WMV, FLV, MPG/MPEG, 3GP, TS, M4V, and animated GIF

## Setup

Requires **CMake** 3.21+, a **C++17** compiler (MSVC, GCC, or Clang), and **Git** (for cloning the repo and the vcpkg submodule).

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

## Build, install, package

The same four commands work on both platforms — each platform's tooling produces its native artifact.

| Step       | Command (from project root)                  | Windows result                                         | macOS result                                                |
|------------|----------------------------------------------|--------------------------------------------------------|-------------------------------------------------------------|
| Configure  | `cmake --preset release`                     | configures `build/release/`                            | configures `build/release/`                                 |
| Build      | `cmake --build --preset release`             | exe in `build/release/bin/`                            | binary in `build/release/bin/`                              |
| Install    | `cmake --install build/release`              | `C:/Program Files/ScrubCut/` (admin)                   | `/Applications/ScrubCut.app` (signed)                       |
| Package    | `cpack --config build/release/CPackConfig.cmake` | `build/release/ScrubCut-<version>-win64.exe`     | `build/release/ScrubCut-<version>-mac.dmg`                  |

Notes:

- For a debug build, swap `release` → `debug` in the configure and build commands.
- On Windows, install requires admin privileges and bundles vcpkg DLLs alongside the exe.
- On macOS, install creates an ad-hoc codesigned `.app` bundle. Gatekeeper will show a first-launch warning since there's no Developer ID signature.
- Packaging requires [NSIS](https://nsis.sourceforge.io/) on Windows (`winget install NSIS.NSIS`).
- The Windows installer registers ScrubCut as an "Open with" handler for common video extensions; the macOS DMG includes a drag-to-`/Applications` shortcut.

### macOS first-launch warning

The `.app` inside the DMG is ad-hoc codesigned (no Apple Developer ID, not notarized). When you launch ScrubCut from `/Applications` for the first time, macOS Gatekeeper will show **"ScrubCut Not Opened — Apple could not verify ScrubCut is free of malware..."**. Two ways to get past it (both are one-time per install):

- **Terminal** — strip the quarantine attribute, then launch normally:
  ```
  xattr -dr com.apple.quarantine /Applications/ScrubCut.app
  ```
- **System Settings** — dismiss the warning dialog, then open **System Settings → Privacy & Security**, scroll to the bottom: there'll be a *"ScrubCut was blocked… Open Anyway"* entry. Click it and confirm. macOS remembers the approval.

## Usage

Launch the app and drag-and-drop a video file onto the window, or press **Ctrl/Cmd + O** to open one via a file dialog.

### Keyboard shortcuts

Shortcuts are grouped the same way as the in-app help panel (press **?**).

#### General

| Action                      | Windows                              | macOS                                       |
|-----------------------------|--------------------------------------|---------------------------------------------|
| Open video                  | Ctrl + O                             | Cmd + O                                     |
| Fullscreen                  | F                                    | F                                           |
| Show / hide UI              | H                                    | H                                           |
| Exit fullscreen / show UI   | Esc                                  | Esc                                         |
| Toggle timeline             | Ctrl + T                             | Ctrl + T                                    |
| Toggle marks                | Ctrl + M                             | Ctrl + M                                    |
| Toggle help                 | Ctrl + H  or  ?                      | Ctrl + H  or  ?                             |
| Quit                        | Alt + F4                             | Cmd + Q                                     |

#### Scrub

| Action                      | Windows                              | macOS                                       |
|-----------------------------|--------------------------------------|---------------------------------------------|
| Play / Pause                | Space                                | Space                                       |
| Seek +/- 1s                 | Ctrl + Left / Right                  | Option + Left / Right                       |
| Seek +/- 5s                 | Left / Right                         | Left / Right                                |
| Seek +/- 30s                | Shift + Left / Right                 | Shift + Left / Right                        |
| Frame step                  | Alt + Left / Right  or  , / .        | Cmd + Option + Left / Right  or  , / .      |
| Jump to start / end         | Home / End                           | Cmd + Left / Right  or  Home / End          |
| Prev / next chapter         | J / K                                | J / K                                       |
| Speed up / down             | + / -                                | + / -                                       |
| Precision scrub             | Alt + drag timeline (0.1× sensitivity) | Option + drag timeline (0.1× sensitivity) |

#### Cut

| Action                      | Windows                              | macOS                                       |
|-----------------------------|--------------------------------------|---------------------------------------------|
| Mark In                     | I  or  [                             | I  or  [                                    |
| Mark Out                    | O  or  ]                             | O  or  ]                                    |
| Mark Frame                  | P                                    | P                                           |
| Remove last mark            | Delete  or  Backspace                | Delete  or  Backspace                       |
| Add mark on timeline        | Ctrl + click (frame) / drag (segment) | Ctrl + click (frame) / drag (segment)      |
| Export                      | Ctrl + E                             | Cmd + E                                     |

#### Media

| Action                      | Windows                              | macOS                                       |
|-----------------------------|--------------------------------------|---------------------------------------------|
| Volume up / down            | Up / Down                            | Up / Down                                   |
| Mute / unmute               | M                                    | M                                           |
| Next / prev audio track     | A / Shift + A                        | A / Shift + A                               |
| Next / prev subtitle track  | S / Shift + S                        | S / Shift + S                               |
| Subtitle delay -/+          | ; / '                                | ; / '                                       |

### Command-line flags

| Argument         | Description                                                     |
|------------------|-----------------------------------------------------------------|
| `<path>`         | Open the given video file at launch                             |
| `-trace`         | Write performance traces to `scrubcut_trace.csv` in the app data dir |
| `-log`           | Allocate a console for stdout/stderr (Windows only)             |
| `-resetlayout`   | Reset the docked panel layout to defaults                       |
| `-profileseek`   | Enable seek-performance profiling                               |
| `-export-segment <startSec> <endSec> <input> [outputBase]` | Headless stream-copy export of a single segment; no UI. Defaults `outputBase` to the input's directory and stem. Output is `<outputBase>_001.<input_ext>`. |

## Tech stack

- **SDL3** -- windowing, audio, events
- **Dear ImGui** (docking) -- immediate-mode UI
- **FFmpeg** -- demuxing, decoding, encoding, filtering
- **OpenGL 3.3+** -- rendering
- **CMake + vcpkg** -- build system and dependency management

## License

[MIT](LICENSE)
