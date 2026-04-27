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

// Headless single-segment export:
//   ScrubCut -export-segment <startSec> <endSec> <inputPath> [outputBasePath]
// outputBasePath works the same as the UI's "output directory + base name":
// the exporter appends the mark name and source extension. If omitted, the
// default is the input file's directory + stem, matching the UI default.
static int RunExportSegment(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "usage: -export-segment <startSec> <endSec> <input> [outputBase]\n");
        return 1;
    }
    std::string input = argv[4];
    std::string outputBase = (argc >= 6) ? argv[5] : "";
    if (outputBase.empty()) {
        std::filesystem::path p(input);
        outputBase = (p.parent_path() / p.stem()).string();
    }

    ExportSettings s;
    s.outputPath = outputBase;
    TimeRange r;
    r.startSec = std::stod(argv[2]);
    r.endSec   = std::stod(argv[3]);
    r.name     = "001";
    r.colorIndex = 0;
    r.mode = ExportMode::SourceFormat;
    s.segments.push_back(r);

    Exporter exp;
    exp.Start(input, s);
    while (exp.IsRunning())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return exp.GetProgress().error ? 1 : 0;
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

    App app;

    if (!app.Init())
        return 1;

    app.Run();
    app.Shutdown();

    return 0;
}
