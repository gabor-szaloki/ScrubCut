#include "App.h"
#include "util/Log.h"
#include "util/Trace.h"
#include "util/CommandLine.h"
#include "util/AppPaths.h"

#include <imgui.h>
#include <imgui_internal.h>
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

// Fixed cycling color palette for segments
static const ImVec4 kSegmentColors[] = {
    {0.30f, 0.55f, 0.85f, 1.0f},  // blue
    {0.90f, 0.55f, 0.20f, 1.0f},  // orange
    {0.35f, 0.75f, 0.40f, 1.0f},  // green
    {0.85f, 0.35f, 0.35f, 1.0f},  // red
    {0.65f, 0.45f, 0.80f, 1.0f},  // purple
    {0.30f, 0.75f, 0.80f, 1.0f},  // cyan
    {0.85f, 0.75f, 0.25f, 1.0f},  // yellow
    {0.85f, 0.50f, 0.65f, 1.0f},  // pink
};
static const int kSegmentColorCount = sizeof(kSegmentColors) / sizeof(kSegmentColors[0]);

static ImU32 GetSegmentColor(int colorIndex, float alpha = 1.0f) {
    const ImVec4& c = kSegmentColors[colorIndex % kSegmentColorCount];
    return IM_COL32(static_cast<int>(c.x * 255), static_cast<int>(c.y * 255),
                    static_cast<int>(c.z * 255), static_cast<int>(alpha * 255));
}

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

    // Delete layout/settings files before ImGui loads them
    bool resetLayout = CommandLine::Get().HasFlag("-resetlayout");
    if (resetLayout) {
        std::filesystem::remove(GetAppDataDir() / "imgui.ini");
        std::filesystem::remove(GetAppDataDir() / "settings.ini");
    }

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
    if (resetLayout) {
        // files already deleted, nothing to restore
    } else {
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
        m_autoHideCursor = m_settings.GetBool("auto_hide_cursor", true);
        m_autoHideUI = m_settings.GetBool("auto_hide_ui", true);
    }

    SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
    SDL_GetWindowSize(m_window, &m_windowedW, &m_windowedH);

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

    // Save windowed mode geometry (tracked while not fullscreen)
    if (m_window) {
        m_settings.SetInt("window_x", m_windowedX);
        m_settings.SetInt("window_y", m_windowedY);
        m_settings.SetInt("window_width", m_windowedW);
        m_settings.SetInt("window_height", m_windowedH);
        m_settings.SetBool("show_timeline", m_showTimeline);
        m_settings.SetBool("show_segments", m_showSegments);
        m_settings.SetBool("auto_hide_cursor", m_autoHideCursor);
        m_settings.SetBool("auto_hide_ui", m_autoHideUI);
        m_settings.Save();
    }
    SDL_ShowCursor();

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

void App::RequestOpenFile(const std::string& path) {
    if (m_segments.GetCount() > 0) {
        m_pendingOpenFilePath = path;
        m_showOpenFileConfirm = true;
    } else {
        OpenFile(path);
    }
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

        if (event.type == SDL_EVENT_MOUSE_MOTION ||
            event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
            event.type == SDL_EVENT_MOUSE_WHEEL) {
            m_lastUIActivityNS = SDL_GetTicksNS();
            if (m_autoHideUI)
                m_uiHidden = false;
        }

        if (!m_fullscreen && (event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED)) {
            SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
            SDL_GetWindowSize(m_window, &m_windowedW, &m_windowedH);
        }

        if (event.type == SDL_EVENT_QUIT) {
            m_running = false;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            m_running = false;
        }
        if (event.type == SDL_EVENT_DROP_FILE) {
            RequestOpenFile(event.drop.data);
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
            case SDLK_LEFTBRACKET:
                if (m_player.HasMedia()) {
                    int before = m_segments.GetCount();
                    m_segments.SetMarkIn(m_player.GetPlaybackTime());
                    if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                        m_showSegments = true;
                    m_lastUIActivityNS = SDL_GetTicksNS();
                    if (m_autoHideUI) m_uiHidden = false;
                }
                break;
            case SDLK_O:
                if (cmd) {
                    static const SDL_DialogFileFilter videoFilters[] = {
                        {"Video files", "mp4;mkv;avi;mov;wmv;flv;webm;mpg;mpeg;3gp;ts;m4v"},
                        {"All files", "*"},
                    };
                    SDL_ShowOpenFileDialog([](void* userdata, const char* const* filelist, int) {
                        if (filelist && filelist[0])
                            static_cast<App*>(userdata)->RequestOpenFile(filelist[0]);
                    }, this, m_window, videoFilters, 2, nullptr, false);
                    break;
                }
                [[fallthrough]];
            case SDLK_RIGHTBRACKET:
                if (m_player.HasMedia()) {
                    double now_t = m_player.GetPlaybackTime();
                    if (m_segments.HasPendingMarkIn()) {
                        int before = m_segments.GetCount();
                        m_segments.SetMarkOut(now_t);
                        if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                            m_showSegments = true;
                    } else if (m_segments.GetCount() > 0) {
                        int last = m_segments.GetCount() - 1;
                        m_segments.UpdateSegment(last, m_segments.GetSegments()[last].startSec, now_t);
                    }
                    m_lastUIActivityNS = SDL_GetTicksNS();
                    if (m_autoHideUI) m_uiHidden = false;
                }
                break;
            case SDLK_DELETE:
            case SDLK_BACKSPACE:
                if (m_segments.GetCount() > 0) {
                    m_segments.RemoveSegment(m_segments.GetCount() - 1);
                    m_lastUIActivityNS = SDL_GetTicksNS();
                    if (m_autoHideUI) m_uiHidden = false;
                }
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
            case SDLK_F:
                m_fullscreen = !m_fullscreen;
                SDL_SetWindowFullscreen(m_window, m_fullscreen);
                break;
            case SDLK_ESCAPE:
                if (m_fullscreen) {
                    m_fullscreen = false;
                    SDL_SetWindowFullscreen(m_window, false);
                }
                break;
            case SDLK_H:
                if (!m_autoHideUI) {
                    m_uiHidden = !m_uiHidden;
                    m_uiAlpha = m_uiHidden ? 0.0f : 1.0f;
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
    m_hoveredSegmentThisFrame = -1;
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

    m_ui.BeginFrame(m_fullscreen);

    ImGuiCond layoutCond = m_ui.IsLayoutResetPending() ? ImGuiCond_Always : ImGuiCond_FirstUseEver;

    // Reposition floating windows proportionally when viewport size changes
    // (skip during layout reset — let the default positions apply)
    {
        ImGuiViewport* mvp = ImGui::GetMainViewport();
        ImVec2 newSize = mvp->WorkSize;
        if (layoutCond != ImGuiCond_Always &&
            m_prevViewportSize.x > 0 && m_prevViewportSize.y > 0 &&
            (newSize.x != m_prevViewportSize.x || newSize.y != m_prevViewportSize.y)) {

            const char* floatingWindows[] = { "Timeline", "Segments", "Help" };
            float margin = 20.0f;
            for (const char* name : floatingWindows) {
                ImGuiWindow* win = ImGui::FindWindowByName(name);
                if (!win || win->DockId != 0) continue;

                // Compute center as ratio of old viewport
                float cx = (win->Pos.x + win->SizeFull.x * 0.5f - mvp->WorkPos.x) / m_prevViewportSize.x;
                float cy = (win->Pos.y + win->SizeFull.y * 0.5f - mvp->WorkPos.y) / m_prevViewportSize.y;

                // Apply ratio to new viewport to get new center
                float newCX = mvp->WorkPos.x + cx * newSize.x;
                float newCY = mvp->WorkPos.y + cy * newSize.y;

                // Derive top-left from center
                float newX = newCX - win->SizeFull.x * 0.5f;
                float newY = newCY - win->SizeFull.y * 0.5f;

                // Clamp with margin
                newX = std::max(mvp->WorkPos.x + margin,
                       std::min(newX, mvp->WorkPos.x + newSize.x - win->SizeFull.x - margin));
                newY = std::max(mvp->WorkPos.y + margin,
                       std::min(newY, mvp->WorkPos.y + newSize.y - win->SizeFull.y - margin));

                win->Pos = ImVec2(newX, newY);
            }
        }
        m_prevViewportSize = newSize;
    }

    // Keep UI visible while hovering any panel or menu (except the Viewport)
    bool hoveringUI = false;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        ImGuiWindow* hovered = ImGui::GetCurrentContext()->HoveredWindow;
        hoveringUI = hovered && strcmp(hovered->Name, "Viewport") != 0;
    }
    if (hoveringUI || ImGui::IsAnyItemHovered())
        m_lastUIActivityNS = SDL_GetTicksNS();

    // Auto-hide UI after 5 seconds of no mouse activity (with 0.3s fade)
    if (m_autoHideUI && m_lastUIActivityNS > 0) {
        uint64_t elapsed = SDL_GetTicksNS() - m_lastUIActivityNS;
        if (elapsed > 5300000000ULL) {
            m_uiHidden = true;
            m_uiAlpha = 0.0f;
        } else if (elapsed > 5000000000ULL) {
            float fadeProgress = static_cast<float>(elapsed - 5000000000ULL) / 300000000.0f;
            m_uiAlpha = 1.0f - fadeProgress;
        } else {
            m_uiAlpha = 1.0f;
        }
    } else if (!m_uiHidden) {
        m_uiAlpha = 1.0f;
    }

    // Auto-hide mouse cursor
    if (m_autoHideCursor && m_lastUIActivityNS > 0) {
        uint64_t elapsed = SDL_GetTicksNS() - m_lastUIActivityNS;
        if (elapsed > 5000000000ULL && !hoveringUI)
            SDL_HideCursor();
        else
            SDL_ShowCursor();
    } else {
        SDL_ShowCursor();
    }

    // Menu bar — in fullscreen: overlay with fade, subject to auto-hide
    bool showMenuBar = !m_fullscreen || !m_uiHidden;
    if (m_fullscreen && showMenuBar) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_uiAlpha);
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.2f, 0.2f, 0.2f, 0.85f * m_uiAlpha));
    }
    if (showMenuBar && ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", (std::string(kKeys.cmdName) + "+O").c_str())) {
                static const SDL_DialogFileFilter videoFilters[] = {
                    {"Video files", "mp4;mkv;avi;mov;wmv;flv;webm;mpg;mpeg;3gp;ts;m4v"},
                    {"All files", "*"},
                };
                SDL_ShowOpenFileDialog([](void* userdata, const char* const* filelist, int) {
                    if (filelist && filelist[0])
                        static_cast<App*>(userdata)->RequestOpenFile(filelist[0]);
                }, this, m_window, videoFilters, 2, nullptr, false);
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
            if (ImGui::MenuItem("Fullscreen", "F", m_fullscreen)) {
                m_fullscreen = !m_fullscreen;
                SDL_SetWindowFullscreen(m_window, m_fullscreen);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Timeline", (std::string(kKeys.cmdName) + "+T").c_str(), m_showTimeline))
                m_showTimeline = !m_showTimeline;
            if (ImGui::MenuItem("Segments", (std::string(kKeys.cmdName) + "+S").c_str(), m_showSegments)) {
                m_showSegments = !m_showSegments;
                if (!m_showSegments) m_segmentsClosedManually = true;
            }
            if (ImGui::MenuItem("Help", "?", m_showHelpPanel))
                m_showHelpPanel = !m_showHelpPanel;
            ImGui::Separator();
            if (ImGui::MenuItem("Auto-hide Mouse Cursor", nullptr, m_autoHideCursor))
                m_autoHideCursor = !m_autoHideCursor;
            if (ImGui::MenuItem("Auto-hide UI", nullptr, m_autoHideUI))
                m_autoHideUI = !m_autoHideUI;
            if (ImGui::MenuItem("Show/Hide UI", "H", false, !m_autoHideUI)) {
                m_uiHidden = !m_uiHidden;
                m_uiAlpha = m_uiHidden ? 0.0f : 1.0f;
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
    if (m_fullscreen && showMenuBar) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
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
    if (m_showTimeline && !m_uiHidden) {
    ImGui::SetNextWindowBgAlpha(0.85f * m_uiAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_uiAlpha);
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

        // Segment mark buttons — centered between transport and audio controls
        {
            float afterTransport = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x;
            bool muted_ = m_player.IsMuted();
            float muteW_ = ImGui::CalcTextSize(muted_ ? "Unmute" : "Mute").x + ImGui::GetStyle().FramePadding.x * 2;
            float audioStart = panelWidth - muteW_ - ImGui::GetStyle().ItemSpacing.x - 80.0f;
            float bracketW = ImGui::CalcTextSize(" [ ").x + ImGui::GetStyle().FramePadding.x * 2;
            float segBtnsW = bracketW * 2 + ImGui::GetStyle().ItemSpacing.x;
            float segStart = afterTransport + (audioStart - afterTransport - segBtnsW) * 0.5f;
            ImGui::SameLine(segStart);
        }
        if (ImGui::Button(" [ ")) {
            int before = m_segments.GetCount();
            m_segments.SetMarkIn(m_player.GetPlaybackTime());
            if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                m_showSegments = true;
            m_lastUIActivityNS = SDL_GetTicksNS();
            if (m_autoHideUI) m_uiHidden = false;
        }
        ImGui::SameLine();
        bool canMarkOut = m_segments.HasPendingMarkIn() || m_segments.GetCount() > 0;
        if (!canMarkOut) ImGui::BeginDisabled();
        if (ImGui::Button(" ] ")) {
            double now_t = m_player.GetPlaybackTime();
            if (m_segments.HasPendingMarkIn()) {
                int before = m_segments.GetCount();
                m_segments.SetMarkOut(now_t);
                if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                    m_showSegments = true;
            } else if (m_segments.GetCount() > 0) {
                int last = m_segments.GetCount() - 1;
                m_segments.UpdateSegment(last, m_segments.GetSegments()[last].startSec, now_t);
            }
            m_lastUIActivityNS = SDL_GetTicksNS();
            if (m_autoHideUI) m_uiHidden = false;
        }
        if (!canMarkOut) ImGui::EndDisabled();

        // Right: Mute + volume slider
        {
            bool muted = m_player.IsMuted();
            bool noAudio = hasMedia && !m_player.HasAudio();
            float muteW = ImGui::CalcTextSize(muted ? "Unmute" : "Mute").x + ImGui::GetStyle().FramePadding.x * 2;
            float sliderW = 80.0f;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float volumeAreaWidth = muteW + spacing + sliderW;
            ImGui::SameLine(panelWidth - volumeAreaWidth);
            if (noAudio) ImGui::BeginDisabled();
            if (ImGui::Button(muted ? "Unmute" : "Mute")) {
                m_player.SetMuted(!muted);
            }
            ImGui::SameLine();
            if (noAudio) {
                ImGui::Text("No Audio");
            } else {
                float volPct = m_player.GetVolume() * 100.0f;
                ImGui::SetNextItemWidth(sliderW);
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
        auto fadeCol = [this](int r, int g, int b, int a) -> ImU32 {
            return IM_COL32(r, g, b, static_cast<int>(a * m_uiAlpha));
        };

        // Background (matches ImGui's FrameBg color)
        ImVec4 frameBg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        drawList->AddRectFilled(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight),
                                IM_COL32(static_cast<int>(frameBg.x * 255), static_cast<int>(frameBg.y * 255),
                                         static_cast<int>(frameBg.z * 255), static_cast<int>(frameBg.w * 255 * m_uiAlpha)));

        // Segments
        const auto& segs = m_segments.GetSegments();
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        for (int i = 0; i < static_cast<int>(segs.size()); i++) {
            float x0 = barPos.x + static_cast<float>(segs[i].startSec / duration) * barWidth;
            float x1 = barPos.x + static_cast<float>(segs[i].endSec / duration) * barWidth;

            // Check if mouse is hovering this segment on the timeline bar
            bool barHovered = mousePos.x >= x0 && mousePos.x <= x1 &&
                              mousePos.y >= barPos.y && mousePos.y <= barPos.y + barHeight;
            if (barHovered)
                m_hoveredSegmentThisFrame = i;

            bool highlighted = (m_hoveredSegment == i);
            float fillAlpha = highlighted ? 0.7f : 0.45f;
            float borderAlpha = highlighted ? 1.0f : 0.8f;
            ImU32 fillCol = GetSegmentColor(segs[i].colorIndex, fillAlpha * m_uiAlpha);
            ImU32 borderCol = GetSegmentColor(segs[i].colorIndex, borderAlpha * m_uiAlpha);
            drawList->AddRectFilled(ImVec2(x0, barPos.y), ImVec2(x1, barPos.y + barHeight), fillCol);
            drawList->AddRect(ImVec2(x0, barPos.y), ImVec2(x1, barPos.y + barHeight), borderCol,
                              0.0f, 0, highlighted ? 3.0f : 1.0f);

            // Diagonal line pattern on highlighted segments
            if (highlighted) {
                ImU32 lineCol = GetSegmentColor(segs[i].colorIndex, 0.6f * m_uiAlpha);
                float spacing = 8.0f;
                drawList->PushClipRect(ImVec2(x0, barPos.y), ImVec2(x1, barPos.y + barHeight), true);
                for (float offset = -barHeight; offset < (x1 - x0) + barHeight; offset += spacing) {
                    drawList->AddLine(
                        ImVec2(x0 + offset, barPos.y + barHeight),
                        ImVec2(x0 + offset + barHeight, barPos.y),
                        lineCol, 2.0f);
                }
                drawList->PopClipRect();
            }
        }

        // Pending mark-in indicator
        if (m_segments.HasPendingMarkIn() && duration > 0.0) {
            float mx = barPos.x + static_cast<float>(m_segments.GetPendingMarkIn() / duration) * barWidth;
            for (float y = barPos.y; y < barPos.y + barHeight; y += 6.0f) {
                float yEnd = y + 3.0f;
                if (yEnd > barPos.y + barHeight) yEnd = barPos.y + barHeight;
                drawList->AddLine(ImVec2(mx, y), ImVec2(mx, yEnd),
                                  fadeCol(255, 200, 50, 180), 2.0f);
            }
        }

        // Playhead (follows mouse immediately during any seek)
        if (duration > 0.0) {
            float px = barPos.x + static_cast<float>(currentTime / duration) * barWidth;
            drawList->AddLine(ImVec2(px, barPos.y), ImVec2(px, barPos.y + barHeight),
                              fadeCol(255, 255, 255, 220), 2.0f);
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
    ImGui::PopStyleVar();
    } // m_showTimeline

    // Segments panel
    if (m_showSegments && !m_uiHidden) {
    ImGui::SetNextWindowBgAlpha(0.85f * m_uiAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_uiAlpha);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 350 - 40, vp->WorkPos.y + 40), layoutCond);
    ImGui::SetNextWindowSize(ImVec2(350, 250), layoutCond);
    bool segmentsWasOpen = m_showSegments;
    ImGui::Begin("Segments", &m_showSegments);

    // Pending mark-in indicator
    if (m_segments.HasPendingMarkIn()) {
        double markIn = m_segments.GetPendingMarkIn();
        int mMin = static_cast<int>(markIn) / 60;
        int mSec = static_cast<int>(markIn) % 60;
        int mMs  = static_cast<int>(markIn * 1000) % 1000;
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Mark In: %02d:%02d.%03d", mMin, mSec, mMs);
    }

    // Scrollable segment list
    float bottomHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 4;
    if (ImGui::BeginChild("##seglist", ImVec2(0, -bottomHeight), ImGuiChildFlags_None)) {
        if (m_segments.GetCount() > 0) {
            const auto& segs = m_segments.GetSegments();
            int removeIdx = -1;
            for (int i = 0; i < static_cast<int>(segs.size()); i++) {
                ImGui::PushID(i);
                const ImVec4& segColor = kSegmentColors[segs[i].colorIndex % kSegmentColorCount];

                float cardPad = 4.0f;
                ImVec2 cardStart = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(cardStart.x + cardPad, cardStart.y + cardPad));
                ImGui::BeginGroup();

                // Row 1: color square, name field, delete button
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 sq = ImGui::GetCursorScreenPos();
                float sqSize = ImGui::GetTextLineHeight();
                dl->AddRectFilled(sq, ImVec2(sq.x + sqSize, sq.y + sqSize),
                    GetSegmentColor(segs[i].colorIndex));
                ImGui::Dummy(ImVec2(sqSize, sqSize));
                ImGui::SameLine();

                char nameBuf[64];
                snprintf(nameBuf, sizeof(nameBuf), "%s", segs[i].name.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30);
                if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
                    m_segments.SetSegmentName(i, nameBuf);
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
                if (ImGui::SmallButton("X")) {
                    removeIdx = i;
                }
                ImGui::PopStyleColor(3);

                // Row 2: start and end times as individual seek buttons
                int sMin = static_cast<int>(segs[i].startSec) / 60;
                int sSec = static_cast<int>(segs[i].startSec) % 60;
                int sMs  = static_cast<int>(segs[i].startSec * 1000) % 1000;
                int eMin = static_cast<int>(segs[i].endSec) / 60;
                int eSec = static_cast<int>(segs[i].endSec) % 60;
                int eMs  = static_cast<int>(segs[i].endSec * 1000) % 1000;
                char startBuf[32], endBuf[32];
                snprintf(startBuf, sizeof(startBuf), "%02d:%02d.%03d##start", sMin, sSec, sMs);
                snprintf(endBuf, sizeof(endBuf), "%02d:%02d.%03d##end", eMin, eSec, eMs);
                double playhead = m_seekTarget;
                bool canSetStart = playhead < segs[i].endSec;
                bool canSetEnd = playhead > segs[i].startSec;
                if (!canSetStart) ImGui::BeginDisabled();
                if (ImGui::SmallButton("[##setstart")) {
                    m_segments.UpdateSegment(i, playhead, segs[i].endSec);
                }
                if (!canSetStart) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton(startBuf))
                    m_player.SeekTo(segs[i].startSec);
                ImGui::SameLine();
                ImGui::Text("->");
                ImGui::SameLine();
                if (ImGui::SmallButton(endBuf))
                    m_player.SeekTo(segs[i].endSec);
                ImGui::SameLine();
                if (!canSetEnd) ImGui::BeginDisabled();
                if (ImGui::SmallButton("]##setend")) {
                    m_segments.UpdateSegment(i, segs[i].startSec, playhead);
                }
                if (!canSetEnd) ImGui::EndDisabled();
                double dur = segs[i].endSec - segs[i].startSec;
                ImGui::SameLine();
                ImGui::TextDisabled("(%.1fs)", dur);
                ImGui::SameLine();
                const char* fmtLabel = (segs[i].mode == ExportMode::GIF) ? "GIF##fmt" : "MP4##fmt";
                if (ImGui::SmallButton(fmtLabel)) {
                    ExportMode newMode = (segs[i].mode == ExportMode::GIF)
                        ? ExportMode::SourceFormat : ExportMode::GIF;
                    m_segments.SetSegmentMode(i, newMode);
                }

                ImGui::EndGroup();

                // Card bounds with padding
                ImVec2 cardEnd = ImVec2(
                    ImGui::GetItemRectMax().x + cardPad,
                    ImGui::GetItemRectMax().y + cardPad);
                // Advance cursor past the bottom padding
                ImGui::SetCursorScreenPos(ImVec2(cardStart.x, cardEnd.y));
                ImGui::Dummy(ImVec2(0, 0));

                // Detect hover on this card
                bool cardHovered = ImGui::IsMouseHoveringRect(cardStart, cardEnd);
                if (cardHovered)
                    m_hoveredSegmentThisFrame = i;

                // Draw highlight if this segment is hovered (from panel or timeline)
                if (m_hoveredSegment == i) {
                    ImDrawList* dl2 = ImGui::GetWindowDrawList();
                    dl2->AddRectFilled(cardStart, cardEnd,
                        GetSegmentColor(segs[i].colorIndex, 0.15f), 3.0f);
                    dl2->AddRect(cardStart, cardEnd,
                        GetSegmentColor(segs[i].colorIndex, 0.5f), 3.0f);
                }

                if (i < static_cast<int>(segs.size()) - 1)
                    ImGui::Separator();

                ImGui::PopID();
            }
            if (removeIdx >= 0) {
                m_segments.RemoveSegment(removeIdx);
            }
        }
    }
    ImGui::EndChild();

    // Bottom bar: Export + Clear All (always visible)
    ImGui::Separator();
    bool hasSegments = m_segments.GetCount() > 0;
    bool canExport = hasSegments && !m_exporter.IsRunning();
    if (!canExport) ImGui::BeginDisabled();
    float btnWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.7f;
    if (ImGui::Button("Export Segments", ImVec2(btnWidth, 0))) {
        m_showExportDialog = true;
        auto dir = std::filesystem::path(m_currentFilePath).parent_path().string();
        auto stem = std::filesystem::path(m_currentFilePath).stem().string();
        snprintf(m_exportDir, sizeof(m_exportDir), "%s", dir.c_str());
        snprintf(m_exportName, sizeof(m_exportName), "%s", stem.c_str());
        m_pendingExport = ExportSettings{};
    }
    if (!canExport) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!hasSegments) ImGui::BeginDisabled();
    if (ImGui::Button("Clear All", ImVec2(-1, 0))) {
        m_segments.Clear();
    }
    if (!hasSegments) ImGui::EndDisabled();

    ImGui::End();
    if (segmentsWasOpen && !m_showSegments) m_segmentsClosedManually = true;
    ImGui::PopStyleVar();
    } // m_showSegments

    // Help panel (toggled with ?)
    if (m_showHelpPanel && !m_uiHidden) {
        ImGui::SetNextWindowBgAlpha(0.85f * m_uiAlpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_uiAlpha);
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
            row("Mark In",              "I  or  [");
            row("Mark Out",             "O  or  ]");
            row("Remove last segment",  kKeys.deleteName);
            row("Export segments",      (std::string(kKeys.cmdName) + " + E").c_str());
            row("Toggle timeline",     (std::string(kKeys.cmdName) + " + T").c_str());
            row("Toggle segments",     (std::string(kKeys.cmdName) + " + S").c_str());
            row("Quit",                 kKeys.quitShortcut);
            row("Toggle help",          "?");

            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // Export dialog
    if (m_showExportDialog) {
        m_exporter.ResetProgress();
        m_exportChecked.assign(m_segments.GetCount(), true);
        ImGui::OpenPopup("Export Segments");
        m_showExportDialog = false;
    }

    if (ImGui::BeginPopupModal("Export Segments", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& progress = m_exporter.GetProgress();

        if (!progress.running && !progress.finished) {
            // --- Settings form ---
            ImGui::Text("Output Directory:");
            ImGui::SetNextItemWidth(500 - ImGui::CalcTextSize("Browse").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
            ImGui::InputText("##dir", m_exportDir, sizeof(m_exportDir));
            ImGui::SameLine();
            if (ImGui::Button("Browse")) {
                SDL_ShowOpenFolderDialog([](void* userdata, const char* const* filelist, int) {
                    if (filelist && filelist[0]) {
                        char* dst = static_cast<char*>(userdata);
                        snprintf(dst, 512, "%s", filelist[0]);
                    }
                }, m_exportDir, m_window, m_exportDir, false);
            }

            ImGui::Text("Filename Base:");
            ImGui::SetNextItemWidth(500);
            ImGui::InputText("##name", m_exportName, sizeof(m_exportName));

            // GIF settings (apply to all GIF segments)
            const auto& segs = m_segments.GetSegments();

            // Segment list with checkboxes in scrollable table
            ImGui::Spacing();
            ImGui::Text("Segments to export:");
            int segCount = static_cast<int>(segs.size());
            float rowH = ImGui::GetTextLineHeightWithSpacing() + 4;
            ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
            ImVec2 tableSize(0, 0);
            if (segCount > 10) {
                tableFlags |= ImGuiTableFlags_ScrollY;
                tableSize.y = 10 * rowH + 30;
            }
            if (ImGui::BeginTable("##export_segs", 5, tableFlags, tableSize)) {
                ImGui::TableSetupColumn("##chk", ImGuiTableColumnFlags_WidthFixed, 24);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed, 160);
                ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("Fmt", ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableHeadersRow();

                for (int i = 0; i < segCount; i++) {
                    ImGui::PushID(i);
                    ImGui::TableNextRow();

                    // Checkbox
                    ImGui::TableNextColumn();
                    bool checked = (i < static_cast<int>(m_exportChecked.size())) && m_exportChecked[i];
                    if (ImGui::Checkbox("##chk", &checked))
                        m_exportChecked[i] = checked;

                    // Editable name
                    ImGui::TableNextColumn();
                    char nameBuf[64];
                    snprintf(nameBuf, sizeof(nameBuf), "%s", segs[i].name.c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
                        m_segments.SetSegmentName(i, nameBuf);

                    // Time range
                    ImGui::TableNextColumn();
                    int sMin = static_cast<int>(segs[i].startSec) / 60;
                    int sSec = static_cast<int>(segs[i].startSec) % 60;
                    int sMs  = static_cast<int>(segs[i].startSec * 1000) % 1000;
                    int eMin = static_cast<int>(segs[i].endSec) / 60;
                    int eSec = static_cast<int>(segs[i].endSec) % 60;
                    int eMs  = static_cast<int>(segs[i].endSec * 1000) % 1000;
                    ImGui::Text("%02d:%02d.%03d-%02d:%02d.%03d", sMin, sSec, sMs, eMin, eSec, eMs);

                    // Duration
                    ImGui::TableNextColumn();
                    double dur = segs[i].endSec - segs[i].startSec;
                    ImGui::Text("%.1fs", dur);

                    // Format (editable toggle)
                    ImGui::TableNextColumn();
                    const char* fmtLabel = (segs[i].mode == ExportMode::GIF) ? "GIF" : "MP4";
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 0.6f));
                    if (ImGui::SmallButton(fmtLabel)) {
                        ExportMode newMode = (segs[i].mode == ExportMode::GIF)
                            ? ExportMode::SourceFormat : ExportMode::GIF;
                        m_segments.SetSegmentMode(i, newMode);
                    }
                    ImGui::PopStyleColor();

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            // GIF settings (shown if any segment is set to GIF)
            bool hasGif = false;
            for (const auto& s : segs)
                if (s.mode == ExportMode::GIF) { hasGif = true; break; }
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

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Validate: check for empty names and any checked
            bool anyChecked = false;
            bool hasEmptyName = false;
            for (int i = 0; i < segCount; i++) {
                if (i < static_cast<int>(m_exportChecked.size()) && m_exportChecked[i]) {
                    anyChecked = true;
                    if (segs[i].name.empty()) hasEmptyName = true;
                }
            }
            if (hasEmptyName)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "All segments must have a name.");

            if (!anyChecked || hasEmptyName) ImGui::BeginDisabled();
            float expBtnWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.7f;
            if (ImGui::Button("Export", ImVec2(expBtnWidth, 0))) {
                m_pendingExport.segments.clear();
                for (int i = 0; i < segCount; i++)
                    if (i < static_cast<int>(m_exportChecked.size()) && m_exportChecked[i])
                        m_pendingExport.segments.push_back(segs[i]);
                m_pendingExport.outputPath = (std::filesystem::path(m_exportDir) / m_exportName).string();

                // Check for existing files
                std::string inputExt = std::filesystem::path(m_currentFilePath).extension().string();
                std::filesystem::path base(m_pendingExport.outputPath);
                std::string stem = base.stem().string();
                std::string dir = base.parent_path().string();
                m_conflictingFiles.clear();
                for (int i = 0; i < static_cast<int>(m_pendingExport.segments.size()); i++) {
                    const auto& seg = m_pendingExport.segments[i];
                    std::string ext = (seg.mode == ExportMode::GIF) ? ".gif" : inputExt;
                    std::string suffix = "_" + seg.name;
                    std::filesystem::path outPath = std::filesystem::path(dir) / (stem + suffix + ext);
                    if (std::filesystem::exists(outPath))
                        m_conflictingFiles.push_back(outPath.filename().string());
                }

                if (m_conflictingFiles.empty()) {
                    m_exporter.Start(m_currentFilePath, m_pendingExport);
                } else {
                    m_showOverwriteConfirm = true;
                }
            }
            if (!anyChecked || hasEmptyName) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
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
            ImGui::Dummy(ImVec2(500, 0)); // maintain minimum width
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Export complete!");
            ImGui::Spacing();

            std::filesystem::path basePath(m_pendingExport.outputPath);
            std::string dir = basePath.parent_path().string();
            std::string stem = basePath.stem().string();
            ImGui::Text("Output folder:");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  %s", dir.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open")) {
#ifdef __APPLE__
                std::string cmd = "open \"" + dir + "\"";
#else
                std::string cmd = "explorer \"" + dir + "\"";
#endif
                system(cmd.c_str());
            }
            ImGui::Spacing();
            ImGui::Text("Exported files:");
            for (int i = 0; i < static_cast<int>(m_pendingExport.segments.size()); i++) {
                const auto& seg = m_pendingExport.segments[i];
                std::string ext = (seg.mode == ExportMode::GIF) ? ".gif" : basePath.extension().string();
                if (ext.empty()) ext = ".mp4";
                std::string suffix = seg.name.empty() ? "_" + std::to_string(i + 1) : "_" + seg.name;
                std::string filename = stem + suffix + ext;
                ImGui::BulletText("%s", filename.c_str());
            }
            ImGui::Spacing();

            if (ImGui::Button("Close", ImVec2(-1, 0))) {
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

        // Overwrite confirmation sub-popup
        if (m_showOverwriteConfirm) {
            ImGui::OpenPopup("Overwrite Files?");
            m_showOverwriteConfirm = false;
        }
        if (ImGui::BeginPopupModal("Overwrite Files?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Dummy(ImVec2(400, 0));
            ImGui::Text("The following files already exist:");
            ImGui::Spacing();
            for (const auto& f : m_conflictingFiles)
                ImGui::BulletText("%s", f.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
            if (ImGui::Button("Overwrite", ImVec2(btnW, 0))) {
                m_exporter.Start(m_currentFilePath, m_pendingExport);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Skip Existing", ImVec2(btnW, 0))) {
                // Remove conflicting segments from pending export
                std::string inputExt = std::filesystem::path(m_currentFilePath).extension().string();
                std::filesystem::path base(m_pendingExport.outputPath);
                std::string stem = base.stem().string();
                std::string dir = base.parent_path().string();
                std::vector<TimeRange> filtered;
                for (int i = 0; i < static_cast<int>(m_pendingExport.segments.size()); i++) {
                    const auto& seg = m_pendingExport.segments[i];
                    std::string ext = (seg.mode == ExportMode::GIF) ? ".gif" : inputExt;
                    std::filesystem::path outPath = std::filesystem::path(dir) / (stem + "_" + seg.name + ext);
                    if (!std::filesystem::exists(outPath))
                        filtered.push_back(seg);
                }
                m_pendingExport.segments = filtered;
                if (!filtered.empty())
                    m_exporter.Start(m_currentFilePath, m_pendingExport);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(btnW, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndPopup();
    }

    // Open file confirmation (segments will be lost)
    if (m_showOpenFileConfirm) {
        ImGui::OpenPopup("Open New File?");
        m_showOpenFileConfirm = false;
    }
    if (ImGui::BeginPopupModal("Open New File?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Opening a new file will clear all segments.");
        ImGui::Text("Are you sure you want to proceed?");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Proceed", ImVec2(btnW, 0))) {
            OpenFile(m_pendingOpenFilePath);
            m_pendingOpenFilePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
            m_pendingOpenFilePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    m_ui.EndFrame();

    m_hoveredSegment = m_hoveredSegmentThisFrame;

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
