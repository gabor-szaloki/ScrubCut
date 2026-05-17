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

// Returns the directory containing bundled read-only resources.
// Windows: install dir (where ScrubCut.exe lives).
// macOS:   ScrubCut.app/Contents/Resources/ when bundled, exe dir otherwise.
// During dev runs from the build tree, this is the build/bin dir; CMake
// copies resource files (e.g. eng.traineddata) next to the binary so the
// same path works in both bundled and dev contexts.
inline const std::filesystem::path& GetResourceDir() {
    static std::filesystem::path dir = []() -> std::filesystem::path {
        const char* basePath = SDL_GetBasePath();
        if (basePath) return basePath;
        return ".";
    }();
    return dir;
}
