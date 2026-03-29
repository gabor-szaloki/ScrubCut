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

    App app;

    if (!app.Init())
        return 1;

    app.Run();
    app.Shutdown();

    return 0;
}
