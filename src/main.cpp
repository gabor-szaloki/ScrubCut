#include "App.h"
#include "util/CommandLine.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifdef _WIN32
#include <windows.h>
#endif

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

    App app;

    if (!app.Init())
        return 1;

    app.Run();
    app.Shutdown();

    return 0;
}
