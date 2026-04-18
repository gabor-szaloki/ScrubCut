#pragma once

#include <SDL3/SDL.h>
#include <string>

class UIManager {
public:
    bool Init(SDL_Window* window, SDL_GLContext glContext);
    void Shutdown();

    void BeginFrame(bool fullscreen);
    void EndFrame();
    void ResetLayout();
    void DeleteLayoutFile();
    bool IsLayoutResetPending();

private:
    void SetupDockspace();
    bool m_layoutInitialized = false;
    bool m_layoutResetPending = false;
    bool m_fullscreen = false;
    std::string m_iniPath;
};
