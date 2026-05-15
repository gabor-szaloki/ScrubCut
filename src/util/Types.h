#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ExportMode { SourceFormat, GIF };

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

struct ExportSettings {
    std::string outputPath;
    std::vector<TimeRange> segments;
    std::vector<FrameMark> frames;
    int gifWidth = 480;
    double gifFps = 15.0;
};
