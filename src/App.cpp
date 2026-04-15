#include "App.h"
#include "util/Log.h"
#include "util/Trace.h"
#include "util/CommandLine.h"
#include "util/AppPaths.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <cmath>
#include <filesystem>

struct PlatformKeys {
    SDL_Keymod cmdMod;          // Cmd on Mac, Ctrl on Windows — for app commands (quit, export, open)
    SDL_Keymod seekFineMod;     // Option on Mac, Ctrl on Windows — for 1s seeking
    SDL_Keymod frameStepMod;    // Alt on both — for frame stepping (+ cmdMod on Mac)
    SDL_Keymod jumpMod;         // Cmd on Mac, none on Windows (uses Home/End) — for jump to start/end
    bool frameStepNeedsCmd;     // true on Mac (Cmd+Option), false on Windows (Alt alone)

    const char* cmdName;        // "Cmd" / "Ctrl"
    const char* seekFineName;   // "Option" / "Ctrl"
    const char* frameStepName;  // "Cmd + Option" / "Alt"
    const char* jumpName;       // "Cmd + Left / Right  or  Home / End" / "Home / End"
    const char* deleteName;     // "Backspace" / "Delete"
    const char* quitShortcut;   // "Cmd + Q" / "Alt+F4"
};

#ifdef __APPLE__
static constexpr PlatformKeys kKeys = {
    SDL_KMOD_GUI, SDL_KMOD_ALT, SDL_KMOD_ALT, SDL_KMOD_GUI, true,
    "Cmd", "Option", "Cmd + Option",
    "Cmd + Left / Right  or  Home / End",
    "Backspace", "Cmd + Q"
};
#else
static constexpr PlatformKeys kKeys = {
    SDL_KMOD_CTRL, SDL_KMOD_CTRL, SDL_KMOD_ALT, SDL_Keymod(0), false,
    "Ctrl", "Ctrl", "Alt",
    "Home / End",
    "Delete", "Alt+F4"
};
#endif

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

    const char* windowTitle = "ScrubCut";
#ifndef NDEBUG
    windowTitle = "ScrubCut - Debug";
#endif

    m_window = SDL_CreateWindow(
        windowTitle,
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
        auto tracePath = (GetAppDataDir() / "logs" / "scrubcut_trace.csv").string();
        TraceFile::Get().Open(tracePath.c_str());
        LOG_INFO("Tracing enabled -> %s", tracePath.c_str());
    }
    m_settings.Load(GetAppDataDir() / "settings.ini");
    if (CommandLine::Get().HasFlag("--resetlayout")) {
        m_ui.DeleteLayoutFile();
        std::filesystem::remove(GetAppDataDir() / "settings.ini");
    } else {
        // Restore saved window state
        m_settings.Load(GetAppDataDir() / "settings.ini");
        int ww = m_settings.GetInt("window_width", 0);
        int wh = m_settings.GetInt("window_height", 0);
        if (ww > 0 && wh > 0)
            SDL_SetWindowSize(m_window, ww, wh);
        int wx = m_settings.GetInt("window_x", SDL_WINDOWPOS_UNDEFINED);
        int wy = m_settings.GetInt("window_y", SDL_WINDOWPOS_UNDEFINED);
        if (wx != SDL_WINDOWPOS_UNDEFINED && wy != SDL_WINDOWPOS_UNDEFINED)
            SDL_SetWindowPosition(m_window, wx, wy);
        m_showTimeline = m_settings.GetBool("show_timeline", true);
        m_showSegments = m_settings.GetBool("show_segments", false);
    }

    LOG_INFO("ScrubCut initialized");
    m_running = true;

    // Open file passed on the command line (e.g. via file association on Windows)
    std::string fileArg = CommandLine::Get().GetFileArg();
    if (!fileArg.empty())
        OpenFile(fileArg);

    return true;
}

void App::Run() {
    while (m_running) {
        TRACE_EVENT("Frame");
        ProcessEvents();
        m_player.PollSeekComplete();

        // Fetch frame before render (for async playback)
        // and after render (for sync seeks triggered by UI during Render)
        auto fetchFrame = [&]() {
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
        };

        {
            TRACE_EVENT("TryGetVideoFrame");
            fetchFrame();
        }

        Render();

        // Pick up frames from seeks triggered during Render (timeline/slider drags)
        fetchFrame();
    }
}

void App::Shutdown() {
    m_player.Close();

    // Save window state before destroying
    if (m_window) {
        int wx, wy, ww, wh;
        SDL_GetWindowPosition(m_window, &wx, &wy);
        SDL_GetWindowSize(m_window, &ww, &wh);
        m_settings.SetInt("window_x", wx);
        m_settings.SetInt("window_y", wy);
        m_settings.SetInt("window_width", ww);
        m_settings.SetInt("window_height", wh);
        m_settings.SetBool("show_timeline", m_showTimeline);
        m_settings.SetBool("show_segments", m_showSegments);
        m_settings.Save();
    }

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

    m_currentFilePath = path;
    m_segments.Clear();
    m_segmentsClosedManually = false;

    m_videoWidth = m_player.GetVideoWidth();
    m_videoHeight = m_player.GetVideoHeight();
    CreateVideoTexture(m_videoWidth, m_videoHeight);

    std::string title = "ScrubCut - " + path;
#ifndef NDEBUG
    title += " - Debug";
#endif
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
            bool shift     = (mod & SDL_KMOD_SHIFT)      != 0;
            bool cmd       = (mod & kKeys.cmdMod)        != 0;
            bool seekFine  = (mod & kKeys.seekFineMod)   != 0 && !cmd;
            bool frameStep = (mod & kKeys.frameStepMod)  != 0 && (!kKeys.frameStepNeedsCmd || cmd);
            bool jump      = kKeys.jumpMod && (mod & kKeys.jumpMod) != 0 && !frameStep;

            switch (event.key.key) {
            case SDLK_SPACE:
                m_player.TogglePlayPause();
                break;
            case SDLK_LEFT:
                if (jump)           m_player.SeekTo(0.0);
                else if (frameStep) m_player.StepFrame(-1);
                else if (shift)     m_player.SeekRelative(-30.0);
                else if (seekFine)  m_player.SeekRelative(-1.0);
                else                m_player.SeekRelative(-5.0);
                break;
            case SDLK_RIGHT:
                if (jump)           m_player.SeekTo(m_player.GetDuration());
                else if (frameStep) m_player.StepFrame(+1);
                else if (shift)     m_player.SeekRelative(30.0);
                else if (seekFine)  m_player.SeekRelative(1.0);
                else                m_player.SeekRelative(5.0);
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
            case SDLK_I:
                if (m_player.HasMedia()) {
                    int before = m_segments.GetCount();
                    m_segments.SetMarkIn(m_player.GetPlaybackTime());
                    if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                        m_showSegments = true;
                }
                break;
            case SDLK_O:
                if (m_player.HasMedia()) {
                    int before = m_segments.GetCount();
                    m_segments.SetMarkOut(m_player.GetPlaybackTime());
                    if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                        m_showSegments = true;
                }
                break;
            case SDLK_DELETE:
            case SDLK_BACKSPACE:
                if (m_segments.GetCount() > 0)
                    m_segments.RemoveSegment(m_segments.GetCount() - 1);
                break;
            case SDLK_E:
                if (cmd && m_segments.GetCount() > 0 && !m_exporter.IsRunning()) {
                    m_showExportDialog = true;
                    auto dir = std::filesystem::path(m_currentFilePath).parent_path().string();
                    auto stem = std::filesystem::path(m_currentFilePath).stem().string();
                    snprintf(m_exportDir, sizeof(m_exportDir), "%s", dir.c_str());
                    snprintf(m_exportName, sizeof(m_exportName), "%s", stem.c_str());
                    m_pendingExport = ExportSettings{};
                }
                break;
            case SDLK_T:
                if (cmd) m_showTimeline = !m_showTimeline;
                break;
            case SDLK_S:
                if (cmd) {
                    m_showSegments = !m_showSegments;
                    if (!m_showSegments) m_segmentsClosedManually = true;
                }
                break;
            case SDLK_Q:
                if (cmd) m_running = false;
                break;
            case SDLK_SLASH:
                // ? key (slash + shift)
                if (shift) m_showHelpPanel = !m_showHelpPanel;
                break;
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

    // Keep seek target in sync with player when not actively seeking
    // and no async seek is in progress. This prevents the playhead from
    // snapping back to an old position while the seek thread is still working.
    if (!m_isTimelineSeeking && !m_player.IsSeekBusy() && m_player.HasMedia()) {
        m_seekTarget = m_player.GetPlaybackTime();
    }

    m_ui.BeginFrame();

    ImGuiCond layoutCond = m_ui.IsLayoutResetPending() ? ImGuiCond_Always : ImGuiCond_FirstUseEver;

    // Menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", (std::string(kKeys.cmdName) + "+O").c_str())) {
                // TODO: file dialog
            }
            if (ImGui::MenuItem("Export Segments...", (std::string(kKeys.cmdName) + "+E").c_str(),
                                false, m_segments.GetCount() > 0 && !m_exporter.IsRunning())) {
                m_showExportDialog = true;
                // Pre-fill defaults
                auto dir = std::filesystem::path(m_currentFilePath).parent_path().string();
                auto stem = std::filesystem::path(m_currentFilePath).stem().string();
                snprintf(m_exportDir, sizeof(m_exportDir), "%s", dir.c_str());
                snprintf(m_exportName, sizeof(m_exportName), "%s", stem.c_str());
                m_pendingExport = ExportSettings{};
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", kKeys.quitShortcut)) {
                m_running = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Timeline", (std::string(kKeys.cmdName) + "+T").c_str(), m_showTimeline))
                m_showTimeline = !m_showTimeline;
            if (ImGui::MenuItem("Segments", (std::string(kKeys.cmdName) + "+S").c_str(), m_showSegments)) {
                m_showSegments = !m_showSegments;
                if (!m_showSegments) m_segmentsClosedManually = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                m_ui.ResetLayout();
                m_showTimeline = true;
                m_showSegments = false;
                m_showHelpPanel = false;
                m_segmentsClosedManually = false;
                SDL_SetWindowSize(m_window, 1280, 720);
                SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                std::filesystem::remove(GetAppDataDir() / "settings.ini");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Help", "?")) {
                m_showHelpPanel = !m_showHelpPanel;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();

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

    // Timeline panel (unified controls + timeline bar)
    if (m_showTimeline) {
    ImGui::SetNextWindowBgAlpha(0.85f);
    float tlPad = 40.0f;
    float tlWidth = std::min(vp->WorkSize.x - tlPad * 2, 1000.0f);
    float tlHeight = 110.0f;
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + (vp->WorkSize.x - tlWidth) * 0.5f, vp->WorkPos.y + vp->WorkSize.y - tlHeight - tlPad),
        layoutCond);
    ImGui::SetNextWindowSize(ImVec2(tlWidth, tlHeight), layoutCond);
    ImGui::Begin("Timeline", &m_showTimeline);
    {
        bool hasMedia = m_player.HasMedia();
        if (!hasMedia) ImGui::BeginDisabled();

        double currentTime = hasMedia ? m_seekTarget : 0.0;
        double duration = hasMedia ? m_player.GetDuration() : 1.0;

        // --- Row 1: [left: speed] [center: transport] [right: volume] ---
        float panelWidth = ImGui::GetContentRegionAvail().x;

        // Left: speed controls
        static const double speeds[] = { 0.25, 0.5, 1.0, 2.0, 4.0 };
        static const char* speedLabels[] = { "0.25x", "0.5x", "1x", "2x", "4x" };
        double curSpeed = m_player.GetSpeed();
        for (int i = 0; i < 5; i++) {
            if (i > 0) ImGui::SameLine();
            bool selected = (std::abs(curSpeed - speeds[i]) < 0.01);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
            if (ImGui::Button(speedLabels[i])) {
                m_player.SetSpeed(speeds[i]);
            }
            if (selected) ImGui::PopStyleColor();
        }

        // Center: transport buttons
        float transportWidth = 11 * (ImGui::CalcTextSize("<<30").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x);
        float transportStart = (panelWidth - transportWidth) * 0.5f;
        ImGui::SameLine(transportStart > 0 ? transportStart : 0);

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

        // Right: Mute + volume slider
        {
            float volumeAreaWidth = 140.0f;
            ImGui::SameLine(panelWidth - volumeAreaWidth);
            bool noAudio = hasMedia && !m_player.HasAudio();
            if (noAudio) ImGui::BeginDisabled();
            bool muted = m_player.IsMuted();
            if (ImGui::Button(muted ? "Unmute" : "Mute")) {
                m_player.SetMuted(!muted);
            }
            ImGui::SameLine();
            if (noAudio) {
                ImGui::Text("No Audio");
            } else {
                float volPct = m_player.GetVolume() * 100.0f;
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::SliderFloat("##vol", &volPct, 0.0f, 100.0f, "%.0f%%")) {
                    m_player.SetVolume(volPct / 100.0f);
                    if (volPct > 0.0f && m_player.IsMuted()) m_player.SetMuted(false);
                }
            }
            if (noAudio) ImGui::EndDisabled();
        }

        // --- Row 2: Timeline bar ---
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        float barWidth = ImGui::GetContentRegionAvail().x;
        float barHeight = 32.0f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight),
                                IM_COL32(40, 40, 40, 255));

        // Segments
        const auto& segs = m_segments.GetSegments();
        for (int i = 0; i < static_cast<int>(segs.size()); i++) {
            float x0 = barPos.x + static_cast<float>(segs[i].startSec / duration) * barWidth;
            float x1 = barPos.x + static_cast<float>(segs[i].endSec / duration) * barWidth;
            // Color by export mode
            ImU32 fillCol = (segs[i].mode == ExportMode::GIF)
                ? IM_COL32(180, 120, 220, 120) : IM_COL32(80, 140, 220, 120);
            ImU32 borderCol = (segs[i].mode == ExportMode::GIF)
                ? IM_COL32(200, 150, 255, 200) : IM_COL32(100, 170, 255, 200);
            drawList->AddRectFilled(ImVec2(x0, barPos.y), ImVec2(x1, barPos.y + barHeight), fillCol);
            drawList->AddRect(ImVec2(x0, barPos.y), ImVec2(x1, barPos.y + barHeight), borderCol);
        }

        // Pending mark-in indicator
        if (m_segments.HasPendingMarkIn() && duration > 0.0) {
            float mx = barPos.x + static_cast<float>(m_segments.GetPendingMarkIn() / duration) * barWidth;
            for (float y = barPos.y; y < barPos.y + barHeight; y += 6.0f) {
                float yEnd = y + 3.0f;
                if (yEnd > barPos.y + barHeight) yEnd = barPos.y + barHeight;
                drawList->AddLine(ImVec2(mx, y), ImVec2(mx, yEnd),
                                  IM_COL32(255, 200, 50, 180), 2.0f);
            }
        }

        // Playhead (follows mouse immediately during any seek)
        if (duration > 0.0) {
            float px = barPos.x + static_cast<float>(currentTime / duration) * barWidth;
            drawList->AddLine(ImVec2(px, barPos.y), ImVec2(px, barPos.y + barHeight),
                              IM_COL32(255, 255, 255, 220), 2.0f);
        }

        // --- Interaction: segment edge handles first, then bar click-to-seek ---
        // Handles are rendered first so they take priority over the bar click
        bool handleActive = false;
        for (int i = 0; i < static_cast<int>(segs.size()); i++) {
            float x0 = barPos.x + static_cast<float>(segs[i].startSec / duration) * barWidth;
            float x1 = barPos.x + static_cast<float>(segs[i].endSec / duration) * barWidth;
            float handleW = 8.0f;

            // Left handle
            ImGui::SetCursorScreenPos(ImVec2(x0 - handleW * 0.5f, barPos.y));
            char idBuf[32];
            snprintf(idBuf, sizeof(idBuf), "##seg_l_%d", i);
            ImGui::InvisibleButton(idBuf, ImVec2(handleW, barHeight));
            if (ImGui::IsItemActive()) {
                float mouseX = ImGui::GetIO().MousePos.x - barPos.x;
                double newStart = static_cast<double>(mouseX / barWidth) * duration;
                newStart = std::max(0.0, std::min(newStart, segs[i].endSec - 0.01));
                m_segments.UpdateSegment(i, newStart, segs[i].endSec);
                m_seekTarget = newStart;
                handleActive = true;

                if (!m_isTimelineSeeking) {
                    m_wasPlayingBeforeTimelineSeek = m_player.IsPlaying();
                    if (m_wasPlayingBeforeTimelineSeek) m_player.Pause();
                    m_isTimelineSeeking = true;
                }
                uint64_t now = SDL_GetTicksNS();
                if (now - m_lastSeekTime > 150000000ULL) {
                    m_player.SeekTo(newStart);
                    m_lastSeekTime = now;
                }
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (!ImGui::IsItemActive() && ImGui::IsItemDeactivated() && m_isTimelineSeeking) {
                m_player.SeekTo(m_seekTarget, m_wasPlayingBeforeTimelineSeek);
                m_isTimelineSeeking = false;
            }

            // Right handle
            ImGui::SetCursorScreenPos(ImVec2(x1 - handleW * 0.5f, barPos.y));
            snprintf(idBuf, sizeof(idBuf), "##seg_r_%d", i);
            ImGui::InvisibleButton(idBuf, ImVec2(handleW, barHeight));
            if (ImGui::IsItemActive()) {
                float mouseX = ImGui::GetIO().MousePos.x - barPos.x;
                double newEnd = static_cast<double>(mouseX / barWidth) * duration;
                newEnd = std::max(segs[i].startSec + 0.01, std::min(newEnd, duration));
                m_segments.UpdateSegment(i, segs[i].startSec, newEnd);
                m_seekTarget = newEnd;
                handleActive = true;

                if (!m_isTimelineSeeking) {
                    m_wasPlayingBeforeTimelineSeek = m_player.IsPlaying();
                    if (m_wasPlayingBeforeTimelineSeek) m_player.Pause();
                    m_isTimelineSeeking = true;
                }
                uint64_t now = SDL_GetTicksNS();
                if (now - m_lastSeekTime > 150000000ULL) {
                    m_player.SeekTo(newEnd);
                    m_lastSeekTime = now;
                }
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (!ImGui::IsItemActive() && ImGui::IsItemDeactivated() && m_isTimelineSeeking) {
                m_player.SeekTo(m_seekTarget, m_wasPlayingBeforeTimelineSeek);
                m_isTimelineSeeking = false;
            }
        }

        // Click-to-seek on timeline bar (only if no handle is active)
        ImGui::SetCursorScreenPos(barPos);
        ImGui::InvisibleButton("##timeline_bar", ImVec2(barWidth, barHeight));
        if (ImGui::IsItemActive() && !handleActive) {
            float mouseX = ImGui::GetIO().MousePos.x - barPos.x;
            double clickTime = static_cast<double>(mouseX / barWidth) * duration;
            clickTime = std::max(0.0, std::min(clickTime, duration));
            m_seekTarget = clickTime;

            if (!m_isTimelineSeeking) {
                m_wasPlayingBeforeTimelineSeek = m_player.IsPlaying();
                if (m_wasPlayingBeforeTimelineSeek) m_player.Pause();
                m_isTimelineSeeking = true;
            }

            uint64_t now = SDL_GetTicksNS();
            if (now - m_lastSeekTime > 150000000ULL) {
                m_player.SeekTo(m_seekTarget);
                m_lastSeekTime = now;
            }
        }
        if (m_isTimelineSeeking && !ImGui::IsItemActive()) {
            m_player.SeekTo(m_seekTarget, m_wasPlayingBeforeTimelineSeek);
            m_isTimelineSeeking = false;
        }

        // --- Row 3: [left: time] [right: resolution | fps] ---
        // Advance cursor past the timeline bar
        ImGui::SetCursorScreenPos(ImVec2(barPos.x, barPos.y + barHeight + 2.0f));
        ImGui::Dummy(ImVec2(0, 0)); // reset ImGui cursor tracking

        int curMin2 = static_cast<int>(currentTime) / 60;
        int curSec2 = static_cast<int>(currentTime) % 60;
        int curMs2  = static_cast<int>(currentTime * 1000) % 1000;
        int durMin2 = static_cast<int>(duration) / 60;
        int durSec2 = static_cast<int>(duration) % 60;
        ImGui::Text("%02d:%02d.%03d / %02d:%02d", curMin2, curSec2, curMs2, durMin2, durSec2);

        // Format file size
        int64_t fileSize = m_player.GetFileSize();
        char sizeBuf[32];
        if (fileSize >= 1024LL * 1024 * 1024)
            snprintf(sizeBuf, sizeof(sizeBuf), "%.1f GB", fileSize / (1024.0 * 1024.0 * 1024.0));
        else if (fileSize >= 1024LL * 1024)
            snprintf(sizeBuf, sizeof(sizeBuf), "%.1f MB", fileSize / (1024.0 * 1024.0));
        else
            snprintf(sizeBuf, sizeof(sizeBuf), "%lld KB", static_cast<long long>(fileSize / 1024));

        // Format bitrate
        int64_t bitrate = m_player.GetBitRate();
        char brateBuf[32];
        if (bitrate >= 1000000)
            snprintf(brateBuf, sizeof(brateBuf), "%.1f Mbps", bitrate / 1000000.0);
        else if (bitrate > 0)
            snprintf(brateBuf, sizeof(brateBuf), "%lld kbps", static_cast<long long>(bitrate / 1000));
        else
            brateBuf[0] = '\0';

        char infoRight[128];
        snprintf(infoRight, sizeof(infoRight), "%dx%d  |  %.1f fps  |  %s  |  %s  |  %s",
                 m_videoWidth, m_videoHeight, m_player.GetFrameRate(),
                 m_player.GetVideoCodecName(), brateBuf, sizeBuf);
        float infoRightWidth = ImGui::CalcTextSize(infoRight).x;
        ImGui::SameLine(panelWidth - infoRightWidth);
        ImGui::Text("%s", infoRight);

        if (!hasMedia) ImGui::EndDisabled();
    }
    ImGui::End();
    } // m_showTimeline

    // Segments panel
    if (m_showSegments) {
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 350 - 40, vp->WorkPos.y + 40), layoutCond);
    ImGui::SetNextWindowSize(ImVec2(350, 150), layoutCond);
    bool segmentsWasOpen = m_showSegments;
    ImGui::Begin("Segments", &m_showSegments);
    if (m_player.HasMedia()) {
        double displayTime = m_seekTarget;

        // --- Mark buttons ---
        if (ImGui::Button("Mark In")) {
            m_segments.SetMarkIn(displayTime);
        }
        ImGui::SameLine();
        if (ImGui::Button("Mark Out")) {
            m_segments.SetMarkOut(displayTime);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Marks")) {
            m_segments.ClearPendingMark();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear All")) {
            m_segments.Clear();
        }
        if (m_segments.HasPendingMarkIn()) {
            double markIn = m_segments.GetPendingMarkIn();
            int mMin = static_cast<int>(markIn) / 60;
            int mSec = static_cast<int>(markIn) % 60;
            int mMs  = static_cast<int>(markIn * 1000) % 1000;
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "  Mark In: %02d:%02d.%03d", mMin, mSec, mMs);
        }

        ImGui::Separator();

    if (m_segments.GetCount() > 0) {
        const auto& segs = m_segments.GetSegments();
        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(segs.size()); i++) {
            int sMin = static_cast<int>(segs[i].startSec) / 60;
            int sSec = static_cast<int>(segs[i].startSec) % 60;
            int sMs  = static_cast<int>(segs[i].startSec * 1000) % 1000;
            int eMin = static_cast<int>(segs[i].endSec) / 60;
            int eSec = static_cast<int>(segs[i].endSec) % 60;
            int eMs  = static_cast<int>(segs[i].endSec * 1000) % 1000;
            double dur = segs[i].endSec - segs[i].startSec;

            char label[128];
            snprintf(label, sizeof(label), "[%d] %02d:%02d.%03d - %02d:%02d.%03d (%.1fs)",
                     i + 1, sMin, sSec, sMs, eMin, eSec, eMs, dur);

            // Limit selectable width so it doesn't overlap buttons
            float buttonsWidth = 90.0f;
            float selectableWidth = ImGui::GetContentRegionAvail().x - buttonsWidth;
            if (ImGui::Selectable(label, false, 0, ImVec2(selectableWidth, 0))) {
                m_player.SeekTo(segs[i].startSec);
            }

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttonsWidth + 10.0f);
            const char* modeLabel = (segs[i].mode == ExportMode::GIF) ? "GIF" : "MP4";
            ImU32 modeCol = (segs[i].mode == ExportMode::GIF)
                ? IM_COL32(200, 150, 255, 255) : IM_COL32(100, 170, 255, 255);
            ImGui::PushStyleColor(ImGuiCol_Button, modeCol);
            char modeBuf[32];
            snprintf(modeBuf, sizeof(modeBuf), "%s##mode_%d", modeLabel, i);
            if (ImGui::SmallButton(modeBuf)) {
                ExportMode newMode = (segs[i].mode == ExportMode::GIF)
                    ? ExportMode::SourceFormat : ExportMode::GIF;
                m_segments.SetSegmentMode(i, newMode);
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();
            char xBuf[16];
            snprintf(xBuf, sizeof(xBuf), "X##seg_%d", i);
            if (ImGui::SmallButton(xBuf)) {
                removeIdx = i;
            }
        }
        if (removeIdx >= 0) {
            m_segments.RemoveSegment(removeIdx);
        }
    }
    } // end if (m_player.HasMedia())
    ImGui::End();
    if (segmentsWasOpen && !m_showSegments) m_segmentsClosedManually = true;
    } // m_showSegments

    // Help panel (toggled with ?)
    if (m_showHelpPanel) {
        ImGui::Begin("Help", &m_showHelpPanel);
        if (ImGui::BeginTable("##shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Hotkey", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            auto row = [](const char* action, const char* hotkey) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", action);
                ImGui::TableNextColumn(); ImGui::Text("%s", hotkey);
            };

            row("Play / Pause",         "Space");
            row("Seek +/- 1s",          (std::string(kKeys.seekFineName) + " + Left / Right").c_str());
            row("Seek +/- 5s",          "Left / Right");
            row("Seek +/- 30s",         "Shift + Left / Right");
            row("Frame step",           (std::string(kKeys.frameStepName) + " + Left / Right  or  , / .").c_str());
            row("Speed up / down",      "+ / -");
            row("Jump to start / end",  kKeys.jumpName);
            row("Mark In",              "I");
            row("Mark Out",             "O");
            row("Remove last segment",  kKeys.deleteName);
            row("Export segments",      (std::string(kKeys.cmdName) + " + E").c_str());
            row("Toggle timeline",     (std::string(kKeys.cmdName) + " + T").c_str());
            row("Toggle segments",     (std::string(kKeys.cmdName) + " + S").c_str());
            row("Quit",                 kKeys.quitShortcut);
            row("Toggle help",          "?");

            ImGui::EndTable();
        }
        ImGui::End();
    }

    // Export dialog
    if (m_showExportDialog) {
        m_exporter.ResetProgress();
        ImGui::OpenPopup("Export Segments");
        m_showExportDialog = false;
    }

    if (ImGui::BeginPopupModal("Export Segments", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& progress = m_exporter.GetProgress();

        if (!progress.running && !progress.finished) {
            // --- Settings form ---
            ImGui::Text("Output Directory:");
            ImGui::SetNextItemWidth(400);
            ImGui::InputText("##dir", m_exportDir, sizeof(m_exportDir));

            ImGui::Text("Filename Base:");
            ImGui::SetNextItemWidth(400);
            ImGui::InputText("##name", m_exportName, sizeof(m_exportName));

            // GIF settings (apply to all GIF segments)
            bool hasGif = false;
            const auto& segs = m_segments.GetSegments();
            for (const auto& s : segs) {
                if (s.mode == ExportMode::GIF) { hasGif = true; break; }
            }
            if (hasGif) {
                ImGui::Spacing();
                ImGui::Text("GIF Settings:");
                ImGui::Indent();
                ImGui::SliderInt("Width", &m_pendingExport.gifWidth, 120, 1920);
                float fps = static_cast<float>(m_pendingExport.gifFps);
                if (ImGui::SliderFloat("FPS", &fps, 5.0f, 30.0f, "%.0f")) {
                    m_pendingExport.gifFps = static_cast<double>(fps);
                }
                ImGui::Unindent();
            }

            // Segment list
            ImGui::Spacing();
            ImGui::Text("Segments to export:");
            for (int i = 0; i < static_cast<int>(segs.size()); i++) {
                int sMin = static_cast<int>(segs[i].startSec) / 60;
                int sSec = static_cast<int>(segs[i].startSec) % 60;
                int sMs  = static_cast<int>(segs[i].startSec * 1000) % 1000;
                int eMin = static_cast<int>(segs[i].endSec) / 60;
                int eSec = static_cast<int>(segs[i].endSec) % 60;
                int eMs  = static_cast<int>(segs[i].endSec * 1000) % 1000;
                const char* fmt = (segs[i].mode == ExportMode::GIF) ? "GIF" : "MP4";
                ImGui::BulletText("[%d] %02d:%02d.%03d - %02d:%02d.%03d  (%s)",
                                  i + 1, sMin, sSec, sMs, eMin, eSec, eMs, fmt);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Export", ImVec2(120, 0))) {
                m_pendingExport.segments = m_segments.GetSegments();
                m_pendingExport.outputPath = (std::filesystem::path(m_exportDir) / m_exportName).string();
                m_exporter.Start(m_currentFilePath, m_pendingExport);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
        } else if (progress.running) {
            // --- Progress display ---
            int cur = progress.currentSegment;
            int total = progress.totalSegments;
            ImGui::Text("Exporting segment %d of %d...", cur, total);
            ImGui::ProgressBar(progress.fraction, ImVec2(400, 0));

            if (ImGui::Button("Cancel Export", ImVec2(120, 0))) {
                m_exporter.Cancel();
                ImGui::CloseCurrentPopup();
            }
        } else if (progress.finished) {
            // --- Done ---
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Export complete!");
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
        } else if (progress.error) {
            // --- Error ---
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Export failed:");
            ImGui::TextWrapped("%s", progress.GetError().c_str());
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }

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
