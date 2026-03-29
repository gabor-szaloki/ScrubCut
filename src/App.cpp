#include "App.h"
#include "util/Log.h"
#include "util/Trace.h"
#include "util/CommandLine.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <cmath>

bool App::Init() {
    LogFile::Get().Open();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    m_window = SDL_CreateWindow(
        "ScrubCut",
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );

    if (!m_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        LOG_ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(m_window, m_glContext);
    SDL_GL_SetSwapInterval(1);

    if (!m_ui.Init(m_window, m_glContext)) {
        LOG_ERROR("UIManager::Init failed");
        return false;
    }

    if (CommandLine::Get().HasFlag("-trace")) {
        TraceFile::Get().Open("logs/scrubcut_trace.csv");
        LOG_INFO("Tracing enabled -> logs/scrubcut_trace.csv");
    }
    LOG_INFO("ScrubCut initialized");
    m_running = true;
    return true;
}

void App::Run() {
    while (m_running) {
        TRACE_EVENT("Frame");
        ProcessEvents();

        {
            TRACE_EVENT("TryGetVideoFrame");
            const uint8_t* rgba = nullptr;
            int w = 0, h = 0;
            if (m_player.TryGetVideoFrame(&rgba, &w, &h)) {
                if (w != m_videoWidth || h != m_videoHeight) {
                    m_videoWidth = w;
                    m_videoHeight = h;
                    CreateVideoTexture(w, h);
                }
                UploadFrame(rgba, w, h);
            }
        }

        Render();
    }
}

void App::Shutdown() {
    m_player.Close();

    if (m_videoTexture) {
        glDeleteTextures(1, &m_videoTexture);
        m_videoTexture = 0;
    }

    m_ui.Shutdown();

    if (m_glContext) {
        SDL_GL_DestroyContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
    LOG_INFO("ScrubCut shutdown");
}

void App::OpenFile(const std::string& path) {
    m_player.Close();

    if (m_videoTexture) {
        glDeleteTextures(1, &m_videoTexture);
        m_videoTexture = 0;
    }
    m_videoWidth = 0;
    m_videoHeight = 0;

    if (!m_player.Open(path))
        return;

    m_videoWidth = m_player.GetVideoWidth();
    m_videoHeight = m_player.GetVideoHeight();
    CreateVideoTexture(m_videoWidth, m_videoHeight);

    std::string title = "ScrubCut - " + path;
    SDL_SetWindowTitle(m_window, title.c_str());

    m_player.Play();
}

void App::ProcessEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_QUIT) {
            m_running = false;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            m_running = false;
        }
        if (event.type == SDL_EVENT_DROP_FILE) {
            OpenFile(event.drop.data);
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !ImGui::GetIO().WantCaptureKeyboard) {
            SDL_Keymod mod = SDL_GetModState();
            bool shift = (mod & SDL_KMOD_SHIFT) != 0;
            bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
            bool alt = (mod & SDL_KMOD_ALT) != 0;

            switch (event.key.key) {
            case SDLK_SPACE:
                m_player.TogglePlayPause();
                break;
            case SDLK_LEFT:
                if (alt)        m_player.StepFrame(-1);
                else if (shift) m_player.SeekRelative(-30.0);
                else if (ctrl)  m_player.SeekRelative(-1.0);
                else            m_player.SeekRelative(-5.0);
                break;
            case SDLK_RIGHT:
                if (alt)        m_player.StepFrame(+1);
                else if (shift) m_player.SeekRelative(30.0);
                else if (ctrl)  m_player.SeekRelative(1.0);
                else            m_player.SeekRelative(5.0);
                break;
            case SDLK_COMMA:
                m_player.StepFrame(-1);
                break;
            case SDLK_PERIOD:
                m_player.StepFrame(+1);
                break;
            case SDLK_HOME:
                m_player.SeekTo(0.0);
                break;
            case SDLK_END:
                m_player.SeekTo(m_player.GetDuration());
                break;
            case SDLK_EQUALS: // + key (= without shift, + with shift)
            case SDLK_KP_PLUS: {
                double spd = m_player.GetSpeed();
                if (spd < 0.25) spd = 0.25;
                else if (spd < 0.5) spd = 0.5;
                else if (spd < 1.0) spd = 1.0;
                else if (spd < 2.0) spd = 2.0;
                else if (spd < 4.0) spd = 4.0;
                else spd = 4.0;
                m_player.SetSpeed(spd);
                break;
            }
            case SDLK_MINUS:
            case SDLK_KP_MINUS: {
                double spd = m_player.GetSpeed();
                if (spd > 4.0) spd = 4.0;
                else if (spd > 2.0) spd = 2.0;
                else if (spd > 1.0) spd = 1.0;
                else if (spd > 0.5) spd = 0.5;
                else if (spd > 0.25) spd = 0.25;
                else spd = 0.25;
                m_player.SetSpeed(spd);
                break;
            }
            }
        }
    }
}

void App::Render() {
    int w, h;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    m_ui.BeginFrame();

    // Menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                // TODO: file dialog
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) {
                m_running = false;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Viewport panel
    ImGui::Begin("Viewport");
    if (m_videoTexture && m_player.HasMedia()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float aspectRatio = static_cast<float>(m_videoWidth) / static_cast<float>(m_videoHeight);
        float displayW = avail.x;
        float displayH = avail.x / aspectRatio;
        if (displayH > avail.y) {
            displayH = avail.y;
            displayW = avail.y * aspectRatio;
        }
        ImVec2 cursor = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(
            cursor.x + (avail.x - displayW) * 0.5f,
            cursor.y + (avail.y - displayH) * 0.5f
        ));
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(m_videoTexture)),
                      ImVec2(displayW, displayH));
    } else {
        ImGui::TextWrapped("Drag and drop a video file to open it.");
    }
    ImGui::End();

    // Controls panel
    ImGui::Begin("Controls");
    if (m_player.HasMedia()) {
        double currentTime = m_isSeeking ? m_seekTarget : m_player.GetPlaybackTime();
        double duration = m_player.GetDuration();

        // --- Row 1: Transport + Seek buttons ---
        if (ImGui::Button("|<")) { m_player.SeekTo(0.0); }
        ImGui::SameLine();
        if (ImGui::Button("<<30")) { m_player.SeekRelative(-30.0); }
        ImGui::SameLine();
        if (ImGui::Button("<<5")) { m_player.SeekRelative(-5.0); }
        ImGui::SameLine();
        if (ImGui::Button("<<1")) { m_player.SeekRelative(-1.0); }
        ImGui::SameLine();
        if (ImGui::Button(" < ")) { m_player.StepFrame(-1); }
        ImGui::SameLine();
        if (ImGui::Button(m_player.IsPlaying() ? " Pause " : " Play  ")) {
            m_player.TogglePlayPause();
        }
        ImGui::SameLine();
        if (ImGui::Button(" > ")) { m_player.StepFrame(+1); }
        ImGui::SameLine();
        if (ImGui::Button("1>>")) { m_player.SeekRelative(1.0); }
        ImGui::SameLine();
        if (ImGui::Button("5>>")) { m_player.SeekRelative(5.0); }
        ImGui::SameLine();
        if (ImGui::Button("30>>")) { m_player.SeekRelative(30.0); }
        ImGui::SameLine();
        if (ImGui::Button(">|")) { m_player.SeekTo(duration); }

        // --- Row 2: Seek bar ---
        float seekPos = (duration > 0.0) ? static_cast<float>(currentTime / duration) : 0.0f;

        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##seek", &seekPos, 0.0f, 1.0f, "")) {
            double targetTime = static_cast<double>(seekPos) * duration;
            m_seekTarget = targetTime;

            if (!m_isSeeking) {
                m_wasPlayingBeforeSeek = m_player.IsPlaying();
                if (m_wasPlayingBeforeSeek) m_player.Pause();
                m_isSeeking = true;
            }

            uint64_t now = SDL_GetTicksNS();
            if (now - m_lastSeekTime > 150000000ULL) {
                m_player.SeekTo(m_seekTarget);
                m_lastSeekTime = now;
            }
        }
        if (m_isSeeking && !ImGui::IsItemActive()) {
            m_player.SeekTo(m_seekTarget);
            if (m_wasPlayingBeforeSeek) m_player.Play();
            m_isSeeking = false;
        }

        // --- Row 3: Time + Speed ---
        int curMin = static_cast<int>(currentTime) / 60;
        int curSec = static_cast<int>(currentTime) % 60;
        int curMs  = static_cast<int>(currentTime * 1000) % 1000;
        int durMin = static_cast<int>(duration) / 60;
        int durSec = static_cast<int>(duration) % 60;

        ImGui::Text("%02d:%02d.%03d / %02d:%02d  |  %.1f fps  |  %dx%d",
                     curMin, curSec, curMs, durMin, durSec,
                     m_player.GetFrameRate(),
                     m_videoWidth, m_videoHeight);

        if (m_player.HasAudio()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "[Audio]");
        }

        // --- Row 4: Speed controls ---
        ImGui::Text("Speed:");
        ImGui::SameLine();
        static const double speeds[] = { 0.25, 0.5, 1.0, 2.0, 4.0 };
        static const char* speedLabels[] = { "0.25x", "0.5x", "1x", "2x", "4x" };
        double curSpeed = m_player.GetSpeed();
        for (int i = 0; i < 5; i++) {
            ImGui::SameLine();
            bool selected = (std::abs(curSpeed - speeds[i]) < 0.01);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
            if (ImGui::Button(speedLabels[i])) {
                m_player.SetSpeed(speeds[i]);
            }
            if (selected) ImGui::PopStyleColor();
        }

        // --- Row 5: Keyboard shortcuts ---
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "Space: Play/Pause | Arrows: +/-5s | Ctrl+Arrows: +/-1s | Shift+Arrows: +/-30s");
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "Alt+Arrows or ,/.: Frame step | +/-: Speed | Home/End: Start/End");
    } else {
        ImGui::Text("No video loaded. Drag and drop a video file.");
    }
    ImGui::End();

    // Timeline panel
    ImGui::Begin("Timeline");
    if (m_player.HasMedia()) {
        ImGui::Text("Timeline (Phase 5)");
    }
    ImGui::End();

    m_ui.EndFrame();

    SDL_GL_SwapWindow(m_window);
}

void App::CreateVideoTexture(int width, int height) {
    if (m_videoTexture) {
        glDeleteTextures(1, &m_videoTexture);
        m_videoTexture = 0;
    }

    glGenTextures(1, &m_videoTexture);
    glBindTexture(GL_TEXTURE_2D, m_videoTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void App::UploadFrame(const uint8_t* rgba, int width, int height) {
    if (!m_videoTexture) return;
    glBindTexture(GL_TEXTURE_2D, m_videoTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
}
