#pragma once

#include <string>
#include <vector>

struct TimeRange {
    double startSec = 0.0;
    double endSec = 0.0;
};

struct ExportSettings {
    enum class Mode { SourceFormat, GIF };
    Mode mode = Mode::SourceFormat;
    std::string outputPath;
    std::vector<TimeRange> segments;
    int gifWidth = 480;
    double gifFps = 15.0;
};
