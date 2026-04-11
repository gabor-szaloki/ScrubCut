#pragma once

#include <string>
#include <filesystem>
#include <SDL3/SDL.h>

// Returns the directory for app data files (logs, imgui.ini, etc.).
// Windows: C:\Users\<user>\AppData\Roaming\ScrubCut\ScrubCut\
// macOS:   ~/Library/Application Support/ScrubCut/ScrubCut/
inline const std::filesystem::path& GetAppDataDir() {
    static std::filesystem::path dir = []() -> std::filesystem::path {
        const char* prefPath = SDL_GetPrefPath("ScrubCut", "ScrubCut");
        if (prefPath) return prefPath;
        return ".";
    }();
    return dir;
}
