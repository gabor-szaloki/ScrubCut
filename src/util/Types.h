#pragma once

#include <string>
#include <vector>

enum class ExportMode { SourceFormat, GIF };

struct TimeRange {
    double startSec = 0.0;
    double endSec = 0.0;
    ExportMode mode = ExportMode::SourceFormat;
    std::string name;
    int colorIndex = 0;
};

struct ExportSettings {
    std::string outputPath;
    std::vector<TimeRange> segments;
    int gifWidth = 480;
    double gifFps = 15.0;
};
