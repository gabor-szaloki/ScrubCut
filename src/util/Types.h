#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

enum class ExportMode { SourceFormat, GIF };

// Crop rectangle in source-video coded-pixel coordinates (the same space the
// decoder/renderer and the FFmpeg `crop` filter operate in).
// w <= 0 || h <= 0 means "no crop / full frame" (the default).
struct CropRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool Active() const { return w > 0 && h > 0; }
    bool operator==(const CropRect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
};

// Clamp a crop into the source frame and snap x/y/w/h to even values
// (yuv420p chroma alignment + libx264 even-dimension requirements; applied
// to all export paths so the viewport preview stays pixel-exact WYSIWYG).
// Returns an inactive rect if the result would be degenerate (< 2x2) or if
// it covers the full frame (a no-op crop).
inline CropRect NormalizeCrop(CropRect c, int srcW, int srcH) {
    if (!c.Active() || srcW <= 0 || srcH <= 0) return {};
    c.x = std::clamp(c.x, 0, std::max(0, srcW - 2)) & ~1;
    c.y = std::clamp(c.y, 0, std::max(0, srcH - 2)) & ~1;
    c.w = std::min(c.w, srcW - c.x) & ~1;
    c.h = std::min(c.h, srcH - c.y) & ~1;
    if (c.w < 2 || c.h < 2) return {};
    if (c.x == 0 && c.y == 0 && c.w == srcW && c.h == srcH) return {};
    return c;
}

// Color characteristics of the decoded video, derived from the transfer
// function. SDR frames go straight to an 8-bit sRGB texture; HDR frames are
// kept in 10-bit BT.2020 and tone-mapped to SDR on the GPU at draw time.
enum class VideoColorMode { SDR, HDR_PQ, HDR_HLG };

// Source color primaries (gamut), used to pick the gamut->BT.709 conversion in
// the tone-map shader. Only the primaries the shader can convert from are
// distinguished; anything else (including unspecified) defaults to BT.2020, the
// near-universal HDR gamut. Integer values are passed to the shader's
// uPrimaries, so keep them in sync with the uPrimaries branch in
// src/ui/shaders/tonemap.frag.glsl.
enum class VideoColorPrimaries { BT709 = 0, BT2020 = 1, DisplayP3 = 2 };

// HDR->SDR tone-mapping operator, selectable from View > HDR. The order here is
// also the menu order. The integer values are passed straight to the
// tone-mapping shader, so keep them in sync with the uTonemapper branch in
// src/ui/shaders/tonemap.frag.glsl.
enum class Tonemapper { None = 0, Reinhard = 1, Uncharted2 = 2, ACES = 3, AgX = 4 };
inline constexpr int kTonemapperCount = 5;

inline const char* TonemapperName(Tonemapper t) {
    switch (t) {
        case Tonemapper::None:       return "No Tonemapping";
        case Tonemapper::Reinhard:   return "Reinhard";
        case Tonemapper::Uncharted2: return "Uncharted 2 (VLC)";
        case Tonemapper::ACES:       return "ACES Filmic";
        case Tonemapper::AgX:        return "AgX";
    }
    return "?";
}

struct TimeRange {
    double startSec = 0.0;
    double endSec = 0.0;
    ExportMode mode = ExportMode::SourceFormat;
    // Playback speed for export. 1.0 = source rate. >1 → faster, <1 → slower.
    // Implemented as a pure timestamp rescale on stream-copy export — no
    // re-encoding. Audio is forcibly dropped at non-1× speed because naive
    // timestamp rescale on AAC packets cuts samples / leaves gaps.
    double speed = 1.0;
    // User preference for keeping the audio track in the exported segment.
    // The actual decision also depends on mode + speed — see
    // EffectiveKeepAudio() below.
    bool keepAudio = true;
    std::string name;
    int colorIndex = 0;
    uint64_t addSeq = 0;  // monotonic add order, shared counter with FrameMark
    // Export crop. Inactive (default) = full frame. SourceFormat segments with
    // an active crop are re-encoded (libx264) instead of stream-copied.
    // Keep last: SegmentManager aggregate-initializes the leading members.
    CropRect crop;
};

// True if `range`'s output format / speed makes keeping audio impossible
// (GIF has no audio track; non-1× speed would need an audio re-encode pass
// we don't currently have). The UI greys out the Audio toggle when this
// returns true.
inline bool AudioForciblyDropped(const TimeRange& range) {
    if (range.mode == ExportMode::GIF) return true;
    if (range.speed < 0.9999 || range.speed > 1.0001) return true;
    return false;
}

// Effective audio decision for the exporter: keep only if the user wants to
// AND the format/speed allow it.
inline bool EffectiveKeepAudio(const TimeRange& range) {
    return range.keepAudio && !AudioForciblyDropped(range);
}

struct FrameMark {
    double timeSec = 0.0;
    std::string name;
    int colorIndex = 0;
    uint64_t addSeq = 0;
    // Export crop. Inactive (default) = full frame. Keep last (see TimeRange).
    CropRect crop;
};

struct Chapter {
    double startSec = 0.0;
    double endSec   = 0.0;
    std::string title;
};

// Read-only description of an audio stream in the open file. Populated in
// Player::Open the same way chapters are, and used to drive the Media menu.
struct AudioTrackInfo {
    int streamIndex = -1;
    std::string title;     // friendly label: metadata title, else "<lang> (codec, Nch)", else "Track N"
    std::string language;  // metadata "language" tag, e.g. "eng" ("" if absent)
    std::string codecName;
    int channels = 0;
};

// Read-only description of a subtitle track. Covers both embedded streams
// (external == false, streamIndex valid) and externally-opened subtitle files
// (external == true, path valid, streamIndex == -1).
struct SubtitleTrackInfo {
    int streamIndex = -1;
    std::string title;
    std::string language;
    bool external = false;
    bool textBased = true; // false for bitmap formats (PGS/VOBSUB/DVB) — shown but not rendered
    std::string path;      // external file path (external only)
};

// A single decoded subtitle cue. SubtitleExtractor produces a time-sorted
// list of these; rendering is a simple time lookup.
struct SubtitleEvent {
    double startSec = 0.0;
    double endSec   = 0.0;
    std::string text; // plain text, '\n'-separated lines, formatting tags stripped
};

struct ExportSettings {
    std::string outputPath;
    std::vector<TimeRange> segments;
    std::vector<FrameMark> frames;
    // Resolution multiplier applied to each GIF segment's (cropped) source
    // dimensions. 1.0 = source size.
    float gifScale = 1.0f;
    double gifFps = 15.0;
    // HDR->SDR operator used when re-encoding HDR sources (GIF/PNG). Mirrors the
    // user's current display selection so exports match what's on screen.
    Tonemapper tonemapper = Tonemapper::Uncharted2;
};
