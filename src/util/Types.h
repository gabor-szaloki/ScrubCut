#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ExportMode { SourceFormat, GIF };

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
    int gifWidth = 480;
    double gifFps = 15.0;
    // HDR->SDR operator used when re-encoding HDR sources (GIF/PNG). Mirrors the
    // user's current display selection so exports match what's on screen.
    Tonemapper tonemapper = Tonemapper::Uncharted2;
};
