#include "App.h"
#include "util/CommandLine.h"
#include "export/Exporter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <chrono>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// Parse an "-crop x:y:w:h" flag into `crop`. Returns false on malformed input.
static bool ParseCropFlag(CropRect& crop) {
    std::string v = CommandLine::Get().GetValue("-crop", "");
    if (v.empty()) return true;
    int x, y, w, h;
    if (sscanf(v.c_str(), "%d:%d:%d:%d", &x, &y, &w, &h) != 4) {
        fprintf(stderr, "bad -crop value '%s' (expected x:y:w:h)\n", v.c_str());
        return false;
    }
    crop = {x, y, w, h};
    return true;
}

static int RunExport(const std::string& input, const ExportSettings& s) {
    Exporter exp;
    exp.Start(input, s);
    while (exp.IsRunning())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (exp.GetProgress().error) {
        fprintf(stderr, "export failed: %s\n", exp.GetProgress().GetError().c_str());
        return 1;
    }
    return 0;
}

// Headless single-segment export:
//   ScrubCut -export-segment <startSec> <endSec> <inputPath> [outputBasePath]
//            [-crop x:y:w:h] [-gif] [-gif-scale <f>] [-gif-fps <f>]
//            [-speed <f>] [-no-audio]
// outputBasePath works the same as the UI's "output directory + base name":
// the exporter appends the mark name and output extension. If omitted, the
// default is the input file's directory + stem, matching the UI default.
static int RunExportSegment(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "usage: -export-segment <startSec> <endSec> <input> [outputBase]"
            " [-crop x:y:w:h] [-gif] [-gif-scale f] [-gif-fps f] [-speed f] [-no-audio]\n");
        return 1;
    }
    std::string input = argv[4];
    std::string outputBase = (argc >= 6 && argv[5][0] != '-') ? argv[5] : "";
    if (outputBase.empty()) {
        std::filesystem::path p(input);
        outputBase = (p.parent_path() / p.stem()).string();
    }

    ExportSettings s;
    s.outputPath = outputBase;
    s.gifScale = static_cast<float>(atof(CommandLine::Get().GetValue("-gif-scale", "1.0").c_str()));
    double gifFps = atof(CommandLine::Get().GetValue("-gif-fps", "0").c_str());
    if (gifFps > 0.0) s.gifFps = gifFps;
    TimeRange r;
    r.startSec = std::stod(argv[2]);
    r.endSec   = std::stod(argv[3]);
    r.name     = "001";
    r.colorIndex = 0;
    r.mode = CommandLine::Get().HasFlag("-gif") ? ExportMode::GIF : ExportMode::SourceFormat;
    double speed = atof(CommandLine::Get().GetValue("-speed", "0").c_str());
    if (speed > 0.0) r.speed = speed;
    r.keepAudio = !CommandLine::Get().HasFlag("-no-audio");
    if (!ParseCropFlag(r.crop)) return 1;
    s.segments.push_back(r);

    return RunExport(input, s);
}

// Headless single-frame PNG export:
//   ScrubCut -export-frame <timeSec> <inputPath> [outputBasePath] [-crop x:y:w:h]
static int RunExportFrame(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "usage: -export-frame <timeSec> <input> [outputBase] [-crop x:y:w:h]\n");
        return 1;
    }
    std::string input = argv[3];
    std::string outputBase = (argc >= 5 && argv[4][0] != '-') ? argv[4] : "";
    if (outputBase.empty()) {
        std::filesystem::path p(input);
        outputBase = (p.parent_path() / p.stem()).string();
    }

    ExportSettings s;
    s.outputPath = outputBase;
    FrameMark f;
    f.timeSec = std::stod(argv[2]);
    f.name = "001";
    if (!ParseCropFlag(f.crop)) return 1;
    s.frames.push_back(f);

    return RunExport(input, s);
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"ScrubCut Main");
#endif
    CommandLine::Get().Init(argc, argv);

#ifdef _WIN32
    // The executable uses the Windows subsystem (no console by default).
    // Allocate a console for stdout/stderr output when -log is passed.
    if (CommandLine::Get().HasFlag("-log")) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif

    if (CommandLine::Get().HasFlag("-export-segment"))
        return RunExportSegment(argc, argv);
    if (CommandLine::Get().HasFlag("-export-frame"))
        return RunExportFrame(argc, argv);

    App app;

    if (!app.Init())
        return 1;

    app.Run();
    app.Shutdown();

    return 0;
}
