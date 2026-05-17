# Text Search in Video — handoff

**Status:** shelved 2026-05-17 on branch `text-search`. Working end-to-end on the
fixture, but Tesseract is the wrong engine for the real use case (long screen
recordings + occasional natural footage). The next move is to switch the OCR
engine to platform-native APIs. See **Next step** below.

This doc is the source of truth for resuming this work in a future session.
The original draft / planning file at
`~/.claude/plans/text-search-in-video.md` is machine-local and may be stale;
trust this file.

## What this branch ships

- `Cmd+F` / `Ctrl+F` toggles a floating Search panel.
- Live fuzzy filter over OCR'd words from the open video.
- Click a row to seek; `F3` / `Shift+F3` cycle matches (also Prev/Next buttons in
  the panel, tooltips show the hotkeys).
- Amber match markers on the timeline bar (between segments and frame marks).
- Amber bounding boxes drawn on top of the video for matched words, only while
  the playhead sits inside an occurrence's `[startSec, endSec]`.
- Lazy indexing: starts on first `Cmd+F` per file, never on plain file open.
- Disk cache at `GetAppDataDir() / "index" / <fp>.txt`. Schema-versioned and
  invalidated on engine/config/source-file changes. LRU eviction at 50 entries.
- Per-word confidence filter (currently `>= 60`) and 16×16 grayscale fingerprint
  dedup between sampled frames.
- Source frames downscaled to max 1280 long-edge before OCR (`SWS_AREA`).
- Headless `-ocrscan` and scripted `-test-search` CLI flags for testing without
  a human at the keyboard. See **Autonomous testing** below.

The Search panel is intentionally **not** persisted (visibility or query) —
opens closed each session, same as Cmd+F in editors.

## Architecture

WaveformExtractor was the model: own thread + own Demuxer, atomic progress
counter, snapshot-based result reads. New files:

| Path | Role |
|---|---|
| `src/util/Types.h` | Adds `TextSpan` (word + normalized bbox) and `TextOccurrence` (time range + words). |
| `src/util/Levenshtein.h` | `Normalize`, `Distance`, `Similarity`, `ContainsFuzzy`. Used by the indexer's dedup + the UI's filter. |
| `src/util/AppPaths.h` | Adds `GetResourceDir()` so we can find `eng.traineddata` next to the binary. |
| `src/core/OcrIndexer.{h,cpp}` | Worker thread: seeks at 1 fps, downscales to ≤1280 long-edge via `sws_scale`, fingerprints, dedups, OCRs survivors, pushes per-word occurrences. |
| `src/core/OcrCache.{h,cpp}` | Text-format JSON-ish cache file. Schema `v4`. Bump the tag (`kSchemaTag` in `OcrCache.cpp`) any time the OCR pipeline output changes. |
| `src/App.{h,cpp}` | Search panel render, Cmd+F handler, F3 nav, timeline overlay, on-video bbox overlay, prefs cleanup, scripted-test harness. |
| `scripts/make_fixture.py` | Regenerates `test_assets/fixture_search.mp4` deterministically via Pillow + FFmpeg. |
| `vcpkg.json` + `CMakeLists.txt` | Add `tesseract` dep; fetch `eng.traineddata` (fast variant) at configure time; bundle to install dir on both platforms. |

Timeline overlay Z-order (in `App::Render`'s timeline bar section): background
→ waveform → segments → **text-search matches** → frame marks → playhead. The
on-video bbox overlay is drawn right after `ImGui::Image` inside the Viewport
window, clipped to the image rect so it can't bleed into letterbox bars.

## Why this is shelved: Tesseract is the wrong engine

The blocker is OCR quality + speed, not the surrounding code. Two real-world
videos exposed both failure modes:

1. **Natural footage (rock climbing, foliage, etc.)** — Tesseract's LSTM
   hallucinates "words" out of any high-frequency texture. On a 6:55 4K video
   it produced ~75 garbage matches per `a` query. The confidence filter at 60
   helped but doesn't eliminate the noise — hallucinated words frequently
   score 60–80.

2. **Screen recordings with small anti-aliased UI text (Unreal Editor
   Outliner)** — searching for "light" on a frame showing
   `DirectionalLight`, `SkyLight`, `SM_SkySphere` etc. returned 0 matches.
   Tesseract scores those words below the confidence threshold even when
   they're perfectly legible to a human.

3. **Speed** — at 1 fps sampling and 1280 long-edge, a 30-min screen recording
   takes >15 min to index on CPU. User's target use case is 30-min recordings.

Things tried, with results:

| Lever | Effect |
|---|---|
| Downscale to 1280 long-edge | Big speed win (~4× on 2720p source); minor accuracy improvement (less noise on textures) |
| Per-word confidence filter (≥60) | Kills most low-quality hallucinations; does not stop 60–80 score noise |
| Switch to `tessdata_fast` (integer LSTM) | ~2× faster, but tanks small-UI-text accuracy; net regression for screen recordings |
| Pixel-diff dedup (16×16 grayscale fingerprint, SAD threshold 1024) | Great for static screen content; useless on natural footage where every frame differs slightly |
| Higher confidence threshold (~80) | Drops hallucinations but also drops most real UI text |

We're at the engine's limit. Tesseract is built for scanned documents; it's
chronically miscalibrated for both modern UI rendering and natural imagery.

## Next step: platform-native OCR

Both macOS and Windows ship modern, GPU/NPU-accelerated OCR engines built for
screen content. They're dramatically faster (Neural Engine / DirectML
inference) and much more accurate on the exact content we care about. Plan:

### Shared interface

```cpp
// src/core/OcrEngine.h
struct OcrWord {
    std::string text;
    float confidence;       // 0..1; Windows engine returns no score → set 1.0
    float bx, by, bw, bh;   // normalized 0..1 in source frame
};

class IOcrEngine {
public:
    virtual ~IOcrEngine() = default;
    virtual bool Init() = 0;
    virtual std::vector<OcrWord> Recognize(const uint8_t* rgba, int w, int h, int stride) = 0;
};
std::unique_ptr<IOcrEngine> CreateNativeOcrEngine();
```

`OcrIndexer::Worker` swaps the Tesseract init/recognize block for
`auto engine = CreateNativeOcrEngine(); engine->Init();` then
`auto words = engine->Recognize(rgba, W, H, W*4);`. Downscaling, dedup,
occurrence-building, and cache all stay. Bump `OcrCache.cpp::kSchemaTag` to
`v5`.

### macOS — Vision Framework (~150 LOC, `src/core/OcrEngineMac.mm`)

- API: `VNRecognizeTextRequest` (Vision.framework, 10.15+). Runs on the Neural
  Engine on M-series Macs.
- File is Objective-C++; CMake marks the one file with `LANGUAGE OBJCXX` and
  links `-framework Vision -framework CoreGraphics -framework Foundation`.
- Flow: wrap our RGBA buffer as a `CGImage` (`CGDataProviderCreateWithData` +
  `CGImageCreate`). Build a `VNImageRequestHandler`, run
  `VNRecognizeTextRequest` with `recognitionLevel = .accurate`,
  `usesLanguageCorrection = NO`. Iterate `VNRecognizedTextObservation`s;
  for each, take `topCandidates(1)`, then for each whitespace-separated word
  in the string compute its substring range and call
  `boundingBox(for: range)` to get a per-word `CGRect` in normalized coords.
  `observation.confidence` is 0..1.
- Synchronous `perform:` from our worker thread is fine — Vision parallelizes
  internally.

### Windows — Windows.Media.Ocr (~200 LOC, `src/core/OcrEngineWin.cpp`)

- API: `winrt::Windows::Media::Ocr::OcrEngine` (WinRT, Win10+). Uses DirectML.
- C++/WinRT headers ship with the Windows SDK. CMake links `windowsapp.lib`.
  No new vcpkg deps. Compile flags: `/await` and `/std:c++17` (already set).
- Flow: copy RGBA into a `SoftwareBitmap` (`BgraPremultiplied8`). Call
  `OcrEngine::TryCreateFromLanguage(L"en-US")->RecognizeAsync(bitmap).get()`.
  Iterate `OcrResult.Lines.Words`; each has `.Text` and `.BoundingRect`
  (pixel coords; we normalize).
- Caveat: Windows OCR doesn't expose a per-word confidence. Either trust it
  (its precision is high enough that hallucinations are rare in practice) or
  add a small heuristic: drop 1-char "words" with no letters, drop words
  whose bbox is implausibly small or has extreme aspect ratio.

### CMake wiring

```cmake
if(APPLE)
    target_sources(ScrubCut PRIVATE src/core/OcrEngineMac.mm)
    set_source_files_properties(src/core/OcrEngineMac.mm PROPERTIES LANGUAGE OBJCXX)
    target_link_libraries(ScrubCut PRIVATE
        "-framework Vision" "-framework CoreGraphics" "-framework Foundation")
elseif(WIN32)
    target_sources(ScrubCut PRIVATE src/core/OcrEngineWin.cpp)
    target_link_libraries(ScrubCut PRIVATE windowsapp)
endif()
```

### What to drop

- `tesseract` from `vcpkg.json` (and transitive Leptonica) — saves ~25 MB.
- `eng.traineddata` fetch + bundling in `CMakeLists.txt`.
- `#include <tesseract/...>` from `OcrIndexer.cpp`.
- `OcrIndexer::SelfTest` (Tesseract-specific) — replace with an engine-init
  check or remove.

### Shipping order

1. Define `IOcrEngine` + refactor `OcrIndexer` to call through it. Keep a
   temporary `OcrEngineTesseract` impl so the build stays green and the
   fixture test still passes.
2. Add `OcrEngineMac.mm`. Verify on the rock-climbing video + the Unreal
   Outliner Slack recording. Both should produce a clean signal.
3. Add `OcrEngineWin.cpp` next time the user is on Windows.
4. Once both platforms work, delete `OcrEngineTesseract` + drop the dep.

## Autonomous testing pattern

ScrubCut's built-in F12 screenshot (renders the OpenGL framebuffer to PNG via
`stb_image_write`) is the testing primitive — **never** use macOS Screen
Recording. The user has explicitly asked we avoid that permission.

CLI flags (see `src/main.cpp` and `App::TickTestSearch`):

- `-ocrscan <video>` — headless, dumps every `OCC` and `WORD` to stdout. Used
  to verify the indexer end-to-end without any UI.
- `-test-search <video> [-test-query <word>]` — scripted UI walkthrough:
  opens the panel automatically (no Cmd+F keystroke), waits for the indexer,
  screenshots the empty-query state, injects the query, seeks into the first
  match, screenshots, F3-advances, screenshots. Saves PNGs to
  `$APPDATA/.../screenshots/`. Note the arg order: **file path must come
  before** the flags or `CommandLine::GetFileArg()` grabs the value of
  `-test-query` as the file.

The harness has one ImGui quirk that's load-bearing: after writing to
`m_searchQuery` directly, we must call `ImGui::ClearActiveID()` to release
the focused `InputText`'s internal scratch buffer; otherwise the next frame
clobbers our injected value. Same reason `m_searchPanelJustOpened = false`
in the test path so the test's `InputText` is never focused in the first
place.

## How to resume

```bash
# On the resume machine:
git fetch origin
git checkout text-search

# Build (vcpkg submodule needs init on a fresh clone)
git submodule update --init --recursive
cmake --preset release          # ~3 min first time (vcpkg builds tesseract + deps)
cmake --build --preset release

# Regenerate the test fixture (.mp4 is gitignored)
python3 -m pip install --user Pillow
python3 scripts/make_fixture.py

# Sanity-check the indexer end-to-end (should print 2 OCCs)
./build/release/bin/ScrubCut -ocrscan test_assets/fixture_search.mp4

# Drive the UI (saves screenshots under ~/Library/Application Support/...)
./build/release/bin/ScrubCut test_assets/fixture_search.mp4 \
    -test-search -test-query alpha
```

The cache (`$APPDATA/.../index/*.txt`) is keyed by schema tag + source
fingerprint, so bumping `kSchemaTag` after engine changes is the safe way to
invalidate stale entries without touching the file system.

## File map cheatsheet

- Indexer entry point: `OcrIndexer::Worker` in `src/core/OcrIndexer.cpp`.
- Cache schema constants: top of `src/core/OcrCache.cpp` (`kSchemaTag`,
  `kSampleFps`, `kFpDim`).
- Tunable constants in `OcrIndexer.cpp`: `kOcrMaxDim` (downscale target),
  `kMinWordConfidence` (Tesseract confidence floor), `kFpSameThreshold`
  (pixel-diff dedup tightness).
- Search panel render: `App::Render`, "Search panel (Cmd+F / Ctrl+F)" block.
- Cmd+F early-out and bare-F fullscreen handling: top of
  `App::ProcessEvents` keydown switch.
- Timeline overlay block: in `App::Render`'s timeline bar section, between
  segments and frame marks.
- On-video bbox overlay: directly after the `ImGui::Image` call in the
  Viewport window block.
- F3 / NavigateMatch and BuildSearchMatchIdx: helpers near the top of
  `App.cpp` (search for those names).
