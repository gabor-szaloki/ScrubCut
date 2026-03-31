#pragma once

#include <SDL3/SDL.h>

class UIManager {
public:
    bool Init(SDL_Window* window, SDL_GLContext glContext);
    void Shutdown();

    void BeginFrame();
    void EndFrame();

private:
    void SetupDockspace();
    bool m_layoutInitialized = false;
};
