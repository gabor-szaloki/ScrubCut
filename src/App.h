#pragma once

#include "ui/UIManager.h"
#include "core/Player.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <string>

class App {
public:
    bool Init();
    void Run();
    void Shutdown();

    void OpenFile(const std::string& path);

private:
    void ProcessEvents();
    void Render();

    void CreateVideoTexture(int width, int height);
    void UploadFrame(const uint8_t* rgba, int width, int height);

    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    UIManager m_ui;
    bool m_running = false;

    Player m_player;

    // Display
    GLuint m_videoTexture = 0;
    int m_videoWidth = 0;
    int m_videoHeight = 0;

    // Seek slider state
    bool m_isSeeking = false;
    bool m_wasPlayingBeforeSeek = false;
    double m_seekTarget = 0.0;
    uint64_t m_lastSeekTime = 0;
};
