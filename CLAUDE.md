# ScrubCut

Cross-platform desktop video player for fast frame-accurate scrubbing, trimming, and frame export. C++17 / CMake / SDL3 / Dear ImGui (docking) / FFmpeg / SDL_GPU (D3D12/Metal/Vulkan), deps via vcpkg (submodule). Windows is the primary target; macOS supported. No Linux build.

## Build, install, package

Same four commands on both platforms, run from the repo root:

```
cmake --preset release
cmake --build --preset release
cmake --install build/release          # Windows needs admin
cpack --config build/release/CPackConfig.cmake
```

Swap `release` → `debug` for a debug build. Windows packaging needs [NSIS](https://nsis.sourceforge.io/). Binaries land in `build/release/bin/`.

`scrubcut_version.h` is regenerated each build (`cmake/GenerateVersion.cmake`) with the CMake project version, git short hash, and dirty flag — only rewritten when content would change, so it doesn't force re-links.

## Layout

```
src/App.{h,cpp}     # main app — menus, viewport rendering, timeline, event loop
src/core/           # playback pipeline (Player, Demuxer, decoders, queues, clock, …)
src/ui/UIManager    # ImGui + SDL3 + SDLGPU3 backends, fonts, DPI, dockspace
src/ui/shaders/     # HLSL, compiled at build time (shadercross; glslang+spirv-cross on macOS) and embedded per-platform
src/export/         # background segment/frame export
src/util/           # AppPaths, Settings (INI), Log, Trace, Types (shared structs), FFmpegUtils
platform/{windows,macos}/  # icons, manifest, plist template, NSIS bits
cmake/              # GenerateVersion.cmake, EmbedShaders.cmake (shader blob embedding)
third_party/stb/    # vendored stb_image_write
```

## Architectural patterns worth knowing up front

These are invariants that grep won't tell you in five minutes:

- **`Player` owns the entire timing-sensitive world**: two `Demuxer` instances (main + a cache demuxer used by `PopulateCacheAroundCurrent` so backward-step scanning never disturbs main playback state), the decoders, the audio output, the clock, the packet/frame queues, and three worker threads (demux, video decode, audio decode).
- **Pipeline park/unpark** (`Player::ParkPipeline` / `UnparkPipeline`): workers halt at top-of-loop gates while queues get interrupted, so seek / flush / decoder-swap can mutate state without races. This is the safe sync point for any mid-playback reconfiguration.
- **Background extractor pattern**: long-running file scans (e.g. `WaveformExtractor`) run on their own thread with their own `Demuxer`, write results monotonically into a buffer, and expose progress as an atomic counter. Destructor and next `Start()` both `Stop()` cleanly. Reuse this shape for any new "scan the whole file once" feature.
- **File-derived read-only metadata** (chapters today, audio/subtitle stream lists in the future): populated in `Player::Open` from `AVFormatContext`, cleared in `Close`. The `Chapter` struct in `util/Types.h` is the model.
- **Two settings files** in `GetAppDataDir()` (see `util/AppPaths.h`): `layout.ini` for window/panel geometry, `preferences.ini` for user prefs and recent files. `util/Settings.h` is a tiny `key=value` reader/writer.
- **Timeline overlays** in `App::Render` all share one mapping: `x = barPos.x + (timeSec / duration) * barWidth`. Pick the right draw order between background → waveform → segments → frame marks → playhead so user marks stay on top.
- **Cross-platform shortcuts**: the `kKeys` struct in `App.cpp` exposes platform-aware modifiers (`cmdMod`, `winMod`, etc.) and display names (`cmdName`, `altKeyName`). Use these instead of hard-coding Cmd vs Ctrl.
- **Rendering & HDR**: everything draws into an FP16 offscreen scene target (extended-sRGB encoded, 1.0 = SDR white); a plain blit (SDR) or a composite pass (HDR) presents it to the swapchain. HDR output is content-gated in `App::UpdateHDROutput` — it engages only while an HDR video is open on an HDR-mode display (scRGB preferred, HDR10 PQ fallback). `ui/VideoTonemap` maps HDR frames for display and export: tone-mapping for SDR output, absolute-nits passthrough for HDR.

## Conventions

- Windows is primary — list Windows first in if/elseif branches, README tables, etc.
- macOS `.app` is **ad-hoc codesigned, not notarized** — Gatekeeper warning on first launch is expected and documented in README.
- Bump version by editing `project(ScrubCut VERSION X.Y.Z)` in `CMakeLists.txt`. The generated version header and macOS Info.plist both pull from this.
- No automated tests. Smoke-test manually via the app.

## When working in this repo

- Start with `README.md` for the user-facing pitch.
- For most features the relevant code is in `src/App.cpp` plus the relevant `src/core/*` file.
- Patterns in this codebase are consistent — when adding a new feature, find the closest existing analog (chapters for read-only file metadata, `WaveformExtractor` for background scans, etc.) and copy its shape rather than inventing a new one.
