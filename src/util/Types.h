#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ExportMode { SourceFormat, GIF };

struct TimeRange {
    double startSec = 0.0;
    double endSec = 0.0;
    ExportMode mode = ExportMode::SourceFormat;
    std::string name;
    int colorIndex = 0;
    uint64_t addSeq = 0;  // monotonic add order, shared counter with FrameMark
};

struct FrameMark {
    double timeSec = 0.0;
    std::string name;
    int colorIndex = 0;
    uint64_t addSeq = 0;
};

struct ExportSettings {
    std::string outputPath;
    std::vector<TimeRange> segments;
    std::vector<FrameMark> frames;
    int gifWidth = 480;
    double gifFps = 15.0;
};
