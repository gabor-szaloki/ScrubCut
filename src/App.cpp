#include "App.h"
#include "util/Log.h"
#include "util/Trace.h"
#include "util/CommandLine.h"
#include "util/AppPaths.h"
#include "scrubcut_version.h"

#include "stb/stb_image_write.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOSERVICE
#define NOMCX
#define NOIME
#include <windows.h>
#endif

struct PlatformKeys {
    SDL_Keymod cmdMod;          // Cmd on Mac, Ctrl on Windows — for app commands (quit, export, open)
    SDL_Keymod winMod;          // Ctrl on both — for window toggles (avoids system hotkey clashes)
    SDL_Keymod seekFineMod;     // Option on Mac, Ctrl on Windows — for 1s seeking
    SDL_Keymod frameStepMod;    // Alt on both — for frame stepping (+ cmdMod on Mac)
    SDL_Keymod jumpMod;         // Cmd on Mac, none on Windows (uses Home/End) — for jump to start/end
    bool frameStepNeedsCmd;     // true on Mac (Cmd+Option), false on Windows (Alt alone)

    const char* cmdName;        // "Cmd" / "Ctrl"
    const char* winModName;     // "Ctrl" / "Ctrl"
    const char* seekFineName;   // "Option" / "Ctrl"
    const char* frameStepName;  // "Cmd + Option" / "Alt"
    const char* jumpName;       // "Cmd + Left / Right  or  Home / End" / "Home / End"
    const char* deleteName;     // "Backspace" / "Delete"
    const char* quitShortcut;   // "Cmd + Q" / "Alt+F4"
    const char* altKeyName;     // "Option" / "Alt"  — the SDL_KMOD_ALT-bearing key
};

#ifdef __APPLE__
static constexpr PlatformKeys kKeys = {
    SDL_KMOD_GUI, SDL_KMOD_CTRL, SDL_KMOD_ALT, SDL_KMOD_ALT, SDL_KMOD_GUI, true,
    "Cmd", "Ctrl", "Option", "Cmd + Option",
    "Cmd + Left / Right  or  Home / End",
    "Backspace", "Cmd + Q", "Option"
};
#else
static constexpr PlatformKeys kKeys = {
    SDL_KMOD_CTRL, SDL_KMOD_CTRL, SDL_KMOD_CTRL, SDL_KMOD_ALT, SDL_Keymod(0), false,
    "Ctrl", "Ctrl", "Ctrl", "Alt",
    "Home / End",
    "Delete", "Alt+F4", "Alt"
};
#endif

// Open a folder in the system file browser without spawning a shell.
// system("explorer ...") launches cmd.exe first and blocks the caller until
// it exits, which is hundreds of ms to seconds on Windows. SDL_OpenURL calls
// ShellExecuteW directly and returns immediately.
static void OpenFolderInShell(const std::filesystem::path& dir) {
    std::string path = dir.generic_string();
    if (!path.empty() && path.front() == '/') path.erase(0, 1);  // unix leading slash is part of the URI
    std::string uri = "file:///" + path;
    SDL_OpenURL(uri.c_str());
}

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

// Tooltip helper: description on line 1, grayed-out shortcut on line 2 (optional).
// Globally gated by App::m_showTooltips, mirrored here so the helper stays free-standing.
static bool g_tooltipsEnabled = true;
static void TooltipFor(const char* desc, const char* shortcut = nullptr) {
    if (!g_tooltipsEnabled) return;
    if (!ImGui::BeginItemTooltip()) return;
    ImGui::TextUnformatted(desc);
    if (shortcut && shortcut[0])
        ImGui::TextDisabled("%s", shortcut);
    ImGui::EndTooltip();
}

static ImU32 GetSegmentColor(int colorIndex, float alpha = 1.0f) {
    const ImVec4& c = kSegmentColors[colorIndex % kSegmentColorCount];
    return IM_COL32(static_cast<int>(c.x * 255), static_cast<int>(c.y * 255),
                    static_cast<int>(c.z * 255), static_cast<int>(alpha * 255));
}

// Discrete playback-speed multipliers, matching the timeline's speed buttons.
// Used both for export-segment speed and (separately) the in-app player speed.
static constexpr double kPlaybackSpeeds[] = { 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };
static constexpr const char* kPlaybackSpeedLabels[] = { ".1x", ".25x", ".5x", "1x", "2x", "4x", "8x" };
static constexpr int kPlaybackSpeedCount =
    sizeof(kPlaybackSpeeds) / sizeof(kPlaybackSpeeds[0]);

// Width helpers for fixed-size buttons. Sized to the widest possible label
// so the button doesn't visually jump when the user toggles state.
static float FixedAudioButtonWidth() {
    return ImGui::CalcTextSize("No audio").x + ImGui::GetStyle().FramePadding.x * 2;
}
static float FixedSpeedButtonWidth() {
    return ImGui::CalcTextSize(".25x").x + ImGui::GetStyle().FramePadding.x * 2;
}

// Fixed-width button. `small=true` mirrors SmallButton's geometry (zero
// vertical padding + AlignTextBaseLine) so it sits on the same baseline as
// neighboring SmallButtons in a row (e.g. the MP4/GIF Fmt toggle). `small=
// false` uses a regular Button.
static bool FixedWidthButton(const char* label, float width, bool small) {
    if (!small) return ImGui::Button(label, ImVec2(width, 0));

    ImGuiContext& g = *ImGui::GetCurrentContext();
    float backupPadY = g.Style.FramePadding.y;
    g.Style.FramePadding.y = 0.0f;
    bool pressed = ImGui::ButtonEx(label, ImVec2(width, 0),
                                   ImGuiButtonFlags_AlignTextBaseLine);
    g.Style.FramePadding.y = backupPadY;
    return pressed;
}

// Audio toggle for a TimeRange — pairs with the Fmt/MP4-GIF toggle in look
// and behaviour. The button shows the current effective state. When the
// format / speed force audio off, the button is greyed out and the tooltip
// explains why. Returns true and writes the new `keepAudio` value if the
// user toggled.
//
// `small=true` for SmallButton-height rows; `small=false` for the
// export-dialog table cell variant.
static bool KeepAudioToggle(const char* id, const TimeRange& range,
                            bool& outKeepAudio, bool small) {
    bool forced = AudioForciblyDropped(range);
    bool effective = !forced && range.keepAudio;
    const char* label = effective ? "Audio" : "No audio";
    char btnLabel[24];
    snprintf(btnLabel, sizeof(btnLabel), "%s%s", label, id);

    if (forced) ImGui::BeginDisabled();
    bool clicked = FixedWidthButton(btnLabel, FixedAudioButtonWidth(), small);
    if (forced) ImGui::EndDisabled();

    if (forced) {
        // Disabled tooltips: ImGui suppresses tooltips on disabled items by
        // default, so render an explicit one when hovered with the disabled-
        // bypass flag.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted((range.mode == ExportMode::GIF)
                ? "GIF has no audio track."
                : "Audio export is not supported with non-1x playback speeds.");
            ImGui::EndTooltip();
        }
        return false;
    }

    if (clicked) {
        outKeepAudio = !range.keepAudio;
        return true;
    }
    return false;
}

// Dropdown picker over the discrete speed multipliers. Looks like a button
// showing the current multiplier label (e.g. ".25x"); clicking opens a
// popup with the options. Returns true and writes to `outSpeed` if the
// user picked a different value than `currentSpeed`. `id` must be unique
// within the current ImGui ID context; pass e.g. "##segspeed".
//
// `small=true` uses a SmallButton (matches the height of neighboring
// SmallButtons in the Marks panel). `small=false` uses a regular Button
// (matches an InputText / checkbox row, e.g. the Export dialog).
static bool SpeedCombo(const char* id, double currentSpeed, double& outSpeed, bool small) {
    int curIdx = 3; // default 1x
    for (int i = 0; i < kPlaybackSpeedCount; i++) {
        if (std::abs(kPlaybackSpeeds[i] - currentSpeed) < 0.01) {
            curIdx = i;
            break;
        }
    }

    char btnLabel[32];
    snprintf(btnLabel, sizeof(btnLabel), "%s%s", kPlaybackSpeedLabels[curIdx], id);

    bool clicked = FixedWidthButton(btnLabel, FixedSpeedButtonWidth(), small);
    if (clicked) ImGui::OpenPopup(id);

    bool changed = false;
    if (ImGui::BeginPopup(id)) {
        for (int i = 0; i < kPlaybackSpeedCount; i++) {
            if (ImGui::Selectable(kPlaybackSpeedLabels[i], i == curIdx)) {
                outSpeed = kPlaybackSpeeds[i];
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    return changed;
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

    // Delete layout files before ImGui loads them
    bool resetLayout = CommandLine::Get().HasFlag("-resetlayout");
    if (resetLayout) {
        std::filesystem::remove(GetAppDataDir() / "imgui.ini");
        std::filesystem::remove(GetAppDataDir() / "layout.ini");
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
    if (CommandLine::Get().HasFlag("-profileseek")) {
        m_player.SetProfileSeek(true);
        LOG_INFO("Seek profiling enabled");
    }
    m_layoutSettings.Load(GetAppDataDir() / "layout.ini");
    m_prefSettings.Load(GetAppDataDir() / "preferences.ini");

    // Preferences always load
    m_showChapters = m_prefSettings.GetBool("show_chapters", true);
    m_showTooltips = m_prefSettings.GetBool("show_tooltips", true);
    m_useDpiScaling = m_prefSettings.GetBool("use_dpi_scaling", false);
    m_autoHideCursor = m_prefSettings.GetBool("auto_hide_cursor", true);
    m_autoHideUI = m_prefSettings.GetBool("auto_hide_ui", true);
    m_player.SetVolume(std::clamp(m_prefSettings.GetFloat("volume", 1.0f), 0.0f, 1.0f));
    m_player.SetMuted(m_prefSettings.GetBool("muted", false));
    m_exportDirMode = static_cast<ExportDirMode>(
        std::clamp(m_prefSettings.GetInt("export_dir_mode", 0), 0, 1));
    {
        std::string customDir = m_prefSettings.GetString("export_custom_dir", "");
        snprintf(m_exportCustomDir, sizeof(m_exportCustomDir), "%s", customDir.c_str());
    }

    // Apply initial DPI scale to the UI (1.0 unless DPI scaling is enabled on Windows).
    m_ui.SetDpiScale(GetEffectiveDpiScale());

    if (resetLayout) {
        // Default window size is scaled by the effective DPI so UI elements
        // are readable on high-DPI displays.
        float dpiScale = m_ui.GetDpiScale();
        SDL_SetWindowSize(m_window,
                          static_cast<int>(1280 * dpiScale),
                          static_cast<int>(720 * dpiScale));
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        int ww = m_layoutSettings.GetInt("window_width", 1280);
        int wh = m_layoutSettings.GetInt("window_height", 720);
        SDL_SetWindowSize(m_window, ww, wh);
        int wx = m_layoutSettings.GetInt("window_x", SDL_WINDOWPOS_UNDEFINED);
        int wy = m_layoutSettings.GetInt("window_y", SDL_WINDOWPOS_UNDEFINED);
        if (wx != SDL_WINDOWPOS_UNDEFINED && wy != SDL_WINDOWPOS_UNDEFINED)
            SDL_SetWindowPosition(m_window, wx, wy);
        m_showTimeline = m_layoutSettings.GetBool("show_timeline", true);
        m_showSegments = m_layoutSettings.GetBool("show_segments", false);
    }

    // Block until the windowing system commits the requested size/position.
    // Without this, on X11/Wayland SDL_GetWindowSize returns the creation
    // default for several frames, and the proportional-resize logic in
    // Render() sees a spurious viewport jump and inflates floating panels.
    SDL_SyncWindow(m_window);

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

        // Handle deferred file open (from dialog callback, possibly off-thread)
        if (m_pendingOpenImmediate) {
            m_pendingOpenImmediate = false;
            OpenFile(m_pendingOpenFilePath);
            m_pendingOpenFilePath.clear();
        }

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
    // If closing while in fullscreen, revert floating-panel ImGui positions
    // to their pre-fullscreen snapshots. Otherwise ImGui would save the
    // fullscreen-repositioned coordinates to imgui.ini, and on the next
    // launch (into a windowed viewport) the panels would load off-screen.
    if (m_fullscreen) {
        RestoreFloatingWindowSnapshots();
        ImGui::MarkIniSettingsDirty();
    }

    m_player.Close();

    // Save windowed mode geometry (tracked while not fullscreen)
    if (m_window) {
        m_layoutSettings.SetInt("window_x", m_windowedX);
        m_layoutSettings.SetInt("window_y", m_windowedY);
        m_layoutSettings.SetInt("window_width", m_windowedW);
        m_layoutSettings.SetInt("window_height", m_windowedH);
        m_layoutSettings.SetBool("show_timeline", m_showTimeline);
        m_layoutSettings.SetBool("show_segments", m_showSegments);
        m_layoutSettings.Save();

        m_prefSettings.SetBool("show_chapters", m_showChapters);
        m_prefSettings.SetBool("show_tooltips", m_showTooltips);
        m_prefSettings.SetBool("use_dpi_scaling", m_useDpiScaling);
        m_prefSettings.SetBool("auto_hide_cursor", m_autoHideCursor);
        m_prefSettings.SetBool("auto_hide_ui", m_autoHideUI);
        m_prefSettings.SetFloat("volume", m_player.GetVolume());
        m_prefSettings.SetBool("muted", m_player.IsMuted());
        m_prefSettings.SetInt("export_dir_mode", static_cast<int>(m_exportDirMode));
        m_prefSettings.SetString("export_custom_dir", m_exportCustomDir);
        m_prefSettings.Save();
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
    // Always defer to main thread — SDL_ShowOpenFileDialog callback may fire
    // on a non-main thread, and OpenFile uses OpenGL calls that require
    // the GL context thread.
    m_pendingOpenFilePath = path;
    if (m_segments.GetCount() > 0)
        m_showOpenFileConfirm = true;
    else
        m_pendingOpenImmediate = true;
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

        // Rescale ImGui when the window moves to a display with different DPI.
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
            event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
            m_ui.SetDpiScale(GetEffectiveDpiScale());
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
        // F12 screenshots work even when ImGui has keyboard capture (e.g. a
        // modal popup is open), so the shortcut stays useful for debugging.
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F12) {
            m_screenshotPending = true;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !ImGui::GetIO().WantCaptureKeyboard) {
            SDL_Keymod mod = event.key.mod;
            bool shift     = (mod & SDL_KMOD_SHIFT)      != 0;
            bool cmd       = (mod & kKeys.cmdMod)        != 0;
            bool winMod    = (mod & kKeys.winMod)        != 0;
            bool seekFine  = (mod & kKeys.seekFineMod)   != 0;
            bool frameStep = (mod & kKeys.frameStepMod)  != 0 && (!kKeys.frameStepNeedsCmd || cmd);
            bool jump      = kKeys.jumpMod && (mod & kKeys.jumpMod) != 0 && !frameStep;
            bool noMod     = !shift && !cmd && !(mod & SDL_KMOD_ALT) && !(mod & SDL_KMOD_GUI) && !(mod & SDL_KMOD_CTRL);

            switch (event.key.key) {
            case SDLK_SPACE:
                if (!noMod) break;
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
                if (!noMod) break;
                m_player.StepFrame(-1);
                break;
            case SDLK_PERIOD:
                if (!noMod) break;
                m_player.StepFrame(+1);
                break;
            case SDLK_HOME:
                if (!noMod) break;
                m_player.SeekTo(0.0);
                break;
            case SDLK_END:
                if (!noMod) break;
                m_player.SeekTo(m_player.GetDuration());
                break;
            case SDLK_J: {
                if (!noMod) break;
                const auto& chs = m_player.GetChapters();
                if (chs.empty()) break;
                double now_t = m_player.GetPlaybackTime();
                // Largest startSec strictly less than (now - epsilon) so a
                // second press steps back even when slightly past a boundary.
                const double eps = 0.25;
                double target = -1.0;
                for (const auto& c : chs) {
                    if (c.startSec < now_t - eps && c.startSec > target)
                        target = c.startSec;
                }
                if (target < 0.0) target = 0.0; // before first chapter → jump to start
                m_player.SeekTo(target);
                break;
            }
            case SDLK_K: {
                if (!noMod) break;
                const auto& chs = m_player.GetChapters();
                if (chs.empty()) break;
                double now_t = m_player.GetPlaybackTime();
                const double eps = 0.05;
                double target = -1.0;
                for (const auto& c : chs) {
                    if (c.startSec > now_t + eps && (target < 0.0 || c.startSec < target))
                        target = c.startSec;
                }
                if (target >= 0.0) m_player.SeekTo(target);
                break;
            }
            case SDLK_EQUALS: // + key (= without shift, + with shift)
            case SDLK_KP_PLUS: {
                if (cmd) break;
                double spd = m_player.GetSpeed();
                if (spd < 0.1) spd = 0.1;
                else if (spd < 0.25) spd = 0.25;
                else if (spd < 0.5) spd = 0.5;
                else if (spd < 1.0) spd = 1.0;
                else if (spd < 2.0) spd = 2.0;
                else if (spd < 4.0) spd = 4.0;
                else if (spd < 8.0) spd = 8.0;
                else spd = 8.0;
                m_player.SetSpeed(spd);
                break;
            }
            case SDLK_MINUS:
            case SDLK_KP_MINUS: {
                if (cmd) break;
                double spd = m_player.GetSpeed();
                if (spd > 8.0) spd = 8.0;
                else if (spd > 4.0) spd = 4.0;
                else if (spd > 2.0) spd = 2.0;
                else if (spd > 1.0) spd = 1.0;
                else if (spd > 0.5) spd = 0.5;
                else if (spd > 0.25) spd = 0.25;
                else if (spd > 0.1) spd = 0.1;
                else spd = 0.1;
                m_player.SetSpeed(spd);
                break;
            }
            case SDLK_I:
            case SDLK_LEFTBRACKET:
                if (!noMod) break;
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
                if (!noMod) break;
                [[fallthrough]];
            case SDLK_RIGHTBRACKET:
                if (!noMod) break;
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
                if (!noMod) break;
                if (m_segments.GetTotalCount() > 0) {
                    m_segments.RemoveLastMark();
                    m_lastUIActivityNS = SDL_GetTicksNS();
                    if (m_autoHideUI) m_uiHidden = false;
                }
                break;
            case SDLK_P:
                if (!noMod) break;
                if (m_player.HasMedia()) {
                    int before = m_segments.GetTotalCount();
                    m_segments.AddFrame(m_player.GetPlaybackTime());
                    if (m_segments.GetTotalCount() > before && !m_showSegments && !m_segmentsClosedManually)
                        m_showSegments = true;
                    m_lastUIActivityNS = SDL_GetTicksNS();
                    if (m_autoHideUI) m_uiHidden = false;
                }
                break;
            case SDLK_E:
                if (cmd && m_segments.GetTotalCount() > 0 && !m_exporter.IsRunning()) {
                    m_showExportDialog = true;
                    auto stem = std::filesystem::path(m_currentFilePath).stem().string();
                    InitExportDir();
                    snprintf(m_exportName, sizeof(m_exportName), "%s", stem.c_str());
                    m_pendingExport = ExportSettings{};
                }
                break;
            case SDLK_T:
                if (winMod) m_showTimeline = !m_showTimeline;
                break;
            case SDLK_M:
                if (winMod) {
                    m_showSegments = !m_showSegments;
                    if (!m_showSegments) m_segmentsClosedManually = true;
                } else if (noMod) {
                    m_player.SetMuted(!m_player.IsMuted());
                }
                break;
            case SDLK_UP:
            case SDLK_DOWN: {
                if (!noMod) break;
                // Step volume in 10% increments, snapped to a clean grid so a
                // slider position like 0.77 lands on 0.8/0.7 rather than 0.87/0.67.
                float v = m_player.GetVolume();
                float snapped = std::round(v * 10.0f) / 10.0f;
                float step = (event.key.key == SDLK_UP) ? 0.1f : -0.1f;
                m_player.SetVolume(std::clamp(snapped + step, 0.0f, 1.0f));
                // Bumping volume up should also unmute, matching the slider's
                // existing behaviour. Down doesn't unmute.
                if (event.key.key == SDLK_UP && m_player.IsMuted())
                    m_player.SetMuted(false);
                break;
            }
            case SDLK_F:
                if (!noMod) break;
                SetFullscreen(!m_fullscreen);
                break;
            case SDLK_ESCAPE:
                if (!noMod) break;
                if (m_fullscreen) {
                    SetFullscreen(false);
                } else if (m_uiHidden) {
                    m_uiHidden = false;
                    m_uiAlpha = 1.0f;
                }
                break;
            case SDLK_H:
                if (winMod) {
                    m_showHelpPanel = !m_showHelpPanel;
                } else if (noMod) {
                    if (!m_autoHideUI) {
                        m_uiHidden = !m_uiHidden;
                        m_uiAlpha = m_uiHidden ? 0.0f : 1.0f;
                    }
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
    g_tooltipsEnabled = m_showTooltips;
    m_hoveredSegmentThisFrame = -1;
    m_hoveredFrameThisFrame = -1;
    int w, h;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Keep seek target in sync with player when not actively seeking.
    // Use GetSeekTargetTime() which returns the pending seek position immediately
    // (before decode completes), falling back to the clock time when no seek is pending.
    if (!m_isTimelineSeeking && m_player.HasMedia()) {
        m_seekTarget = m_player.GetSeekTargetTime();
    }

    m_ui.BeginFrame();

    ImGuiCond layoutCond = m_ui.IsLayoutResetPending() ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    bool wasResetFrame = (layoutCond == ImGuiCond_Always);

    // Reposition floating windows proportionally when the viewport size changes.
    // Use the full viewport Size (not WorkSize) so that the menu bar appearing
    // or disappearing — e.g. on app start before it renders, or on auto-hide
    // toggle — doesn't cause floating panels to creep over time.
    {
        ImGuiViewport* mvp = ImGui::GetMainViewport();
        ImVec2 newSize = mvp->Size;
        if (!m_waitingForFullscreenExit && layoutCond != ImGuiCond_Always &&
            newSize.x > 0 && newSize.y > 0 &&
            m_prevViewportSize.x > 0 && m_prevViewportSize.y > 0 &&
            (newSize.x != m_prevViewportSize.x || newSize.y != m_prevViewportSize.y)) {

            const char* floatingWindows[] = { "Timeline", "Marks", "Help" };
            float margin = 20.0f;
            for (const char* name : floatingWindows) {
                ImGuiWindow* win = ImGui::FindWindowByName(name);
                if (!win || win->DockId != 0) continue;

                // Timeline scales its width with the viewport — the wider it
                // is the more scrubbing resolution you get. Height stays.
                bool scaleWidth = (strcmp(name, "Timeline") == 0);
                float newW = scaleWidth
                    ? win->SizeFull.x * (newSize.x / m_prevViewportSize.x)
                    : win->SizeFull.x;
                float newH = win->SizeFull.y;

                // Compute center as ratio of old viewport
                float cx = (win->Pos.x + win->SizeFull.x * 0.5f - mvp->Pos.x) / m_prevViewportSize.x;
                float cy = (win->Pos.y + win->SizeFull.y * 0.5f - mvp->Pos.y) / m_prevViewportSize.y;

                // Apply ratio to new viewport to get new center, then top-left
                float newX = mvp->Pos.x + cx * newSize.x - newW * 0.5f;
                float newY = mvp->Pos.y + cy * newSize.y - newH * 0.5f;

                // Clamp with margin
                newX = std::max(mvp->Pos.x + margin,
                       std::min(newX, mvp->Pos.x + newSize.x - newW - margin));
                newY = std::max(mvp->Pos.y + margin,
                       std::min(newY, mvp->Pos.y + newSize.y - newH - margin));

                win->Pos = ImVec2(newX, newY);
                if (scaleWidth) {
                    win->SizeFull.x = newW;
                    win->Size.x = newW;
                }
            }
        }
        m_prevViewportSize = newSize;
    }

    // After fullscreen exit on macOS, wait for the viewport to settle at
    // windowed size, then restore snapshot positions and suppress the
    // proportional repositioning that would have fired from the size change.
    if (m_waitingForFullscreenExit) {
        ImVec2 vpSize = ImGui::GetMainViewport()->Size;
        float expectedW = static_cast<float>(m_windowedW);
        float expectedH = static_cast<float>(m_windowedH);
        if (std::abs(vpSize.x - expectedW) < 2.0f && std::abs(vpSize.y - expectedH) < 2.0f) {
            m_waitingForFullscreenExit = false;
            RestoreFloatingWindowSnapshots();
            m_prevViewportSize = vpSize;
        }
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

    // Menu bar — overlay on top of video with fade, subject to auto-hide
    bool showMenuBar = !m_uiHidden;
    if (showMenuBar) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_uiAlpha);
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        // Match the transparency of floating panels (Timeline, Segments, Help)
        ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg.x, bg.y, bg.z, 0.85f * m_uiAlpha));
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
                auto stem = std::filesystem::path(m_currentFilePath).stem().string();
                InitExportDir();
                snprintf(m_exportName, sizeof(m_exportName), "%s", stem.c_str());
                m_pendingExport = ExportSettings{};
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open App Data Folder")) {
                OpenFolderInShell(GetAppDataDir());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", kKeys.quitShortcut)) {
                m_running = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Fullscreen", "F", m_fullscreen)) {
                SetFullscreen(!m_fullscreen);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Timeline", (std::string(kKeys.winModName) + "+T").c_str(), m_showTimeline))
                m_showTimeline = !m_showTimeline;
            if (ImGui::MenuItem("Marks", (std::string(kKeys.winModName) + "+M").c_str(), m_showSegments)) {
                m_showSegments = !m_showSegments;
                if (!m_showSegments) m_segmentsClosedManually = true;
            }
            if (ImGui::MenuItem("Help", (std::string(kKeys.winModName) + "+H or ?").c_str(), m_showHelpPanel))
                m_showHelpPanel = !m_showHelpPanel;
            ImGui::Separator();
            if (ImGui::MenuItem("Show Chapters", nullptr, m_showChapters))
                m_showChapters = !m_showChapters;
            if (ImGui::MenuItem("Tooltips", nullptr, m_showTooltips))
                m_showTooltips = !m_showTooltips;
#ifndef __APPLE__
            if (ImGui::MenuItem("Use DPI scaling", nullptr, m_useDpiScaling)) {
                m_useDpiScaling = !m_useDpiScaling;
                m_ui.SetDpiScale(GetEffectiveDpiScale());
            }
#endif
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
                SetFullscreen(false);
                m_ui.ResetLayout();
                // Force all floating panels visible so their Begin blocks
                // run on the next frame with layoutCond = Always and apply
                // their default Pos/Size. m_hideFloatingWindowsAfterReset
                // re-hides Marks and Help once that's done, so the user-
                // visible end state matches the intended hidden defaults.
                m_showTimeline = true;
                m_showSegments = true;
                m_showHelpPanel = true;
                m_segmentsClosedManually = false;
                m_hideFloatingWindowsAfterReset = true;
                float scale = m_ui.GetDpiScale();
                SDL_SetWindowSize(m_window, static_cast<int>(1280 * scale), static_cast<int>(720 * scale));
                SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                std::filesystem::remove(GetAppDataDir() / "layout.ini");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Help", (std::string(kKeys.winModName) + "+H or ?").c_str())) {
                m_showHelpPanel = !m_showHelpPanel;
            }
            ImGui::EndMenu();
        }

        // Version string at right edge of the menu bar
        {
            char verBuf[64];
            snprintf(verBuf, sizeof(verBuf), "v%s - %s%s",
                     SCRUBCUT_VERSION, SCRUBCUT_GIT_HASH,
                     SCRUBCUT_GIT_DIRTY ? "*" : "");
            float w = ImGui::CalcTextSize(verBuf).x;
            float avail = ImGui::GetContentRegionAvail().x;
            float pad = ImGui::GetStyle().ItemSpacing.x;
            if (avail > w + pad) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w - pad);
                ImGui::TextDisabled("%s", verBuf);
            }
        }

        ImGui::EndMainMenuBar();
    }
    if (showMenuBar) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Viewport panel
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
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
        const char* text = "Drag and drop a video file to open it.";
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2(
            (avail.x - textSize.x) * 0.5f,
            (avail.y - textSize.y) * 0.5f
        ));
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // Timeline panel (unified controls + timeline bar)
    if (m_showTimeline && !m_uiHidden) {
    ImGui::SetNextWindowBgAlpha(0.85f * m_uiAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_uiAlpha);
    float dpi = m_ui.GetDpiScale();
    float tlPad = 40.0f * dpi;
    float tlWidth = std::min(vp->WorkSize.x - tlPad * 2, 1100.0f * dpi);
    float tlHeight = 110.0f * dpi;
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
        static const double speeds[] = { 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };
        static const char* speedLabels[] = { ".1x", ".25x", ".5x", "1x", "2x", "4x", "8x" };
        constexpr int kSpeedCount = sizeof(speeds) / sizeof(speeds[0]);
        double curSpeed = m_player.GetSpeed();
        for (int i = 0; i < kSpeedCount; i++) {
            if (i > 0) ImGui::SameLine();
            bool selected = (std::abs(curSpeed - speeds[i]) < 0.01);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
            if (ImGui::Button(speedLabels[i])) {
                m_player.SetSpeed(speeds[i]);
            }
            if (selected) ImGui::PopStyleColor();
            char desc[32];
            snprintf(desc, sizeof(desc), "Playback speed %gx", speeds[i]);
            TooltipFor(desc, "+ / - to cycle");
        }

        // Center: transport buttons
        const float fp2 = ImGui::GetStyle().FramePadding.x * 2;
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        auto btnW = [&](const char* s) { return ImGui::CalcTextSize(s).x + fp2; };
        const char* playLabel = m_player.IsPlaying() ? "Pause" : "Play";
        const float playBtnW = std::max(btnW("Play"), btnW("Pause"));
        float transportWidth =
            btnW("|<") + btnW("<|<") + btnW("<<30") + btnW("<<5") + btnW("<<1") +
            btnW("<") + playBtnW + btnW(">") +
            btnW("1>>") + btnW("5>>") + btnW("30>>") + btnW(">|>") + btnW(">|") +
            12 * sp;
        float transportStart = (panelWidth - transportWidth) * 0.5f;
        ImGui::SameLine(transportStart > 0 ? transportStart : 0);

        std::string prevFrameSc = std::string(kKeys.frameStepName) + " + Left  or  ,";
        std::string nextFrameSc = std::string(kKeys.frameStepName) + " + Right  or  .";
        std::string back1Sc     = std::string(kKeys.seekFineName)  + " + Left";
        std::string fwd1Sc      = std::string(kKeys.seekFineName)  + " + Right";
        const auto& chs = m_player.GetChapters();
        bool hasChapters = !chs.empty();
        if (ImGui::Button("|<")) { m_player.SeekTo(0.0); }
        TooltipFor("Jump to start", "Home");
        ImGui::SameLine();
        if (!hasChapters) ImGui::BeginDisabled();
        if (ImGui::Button("<|<")) {
            double now_t = m_player.GetPlaybackTime();
            const double eps = 0.25;
            double target = -1.0;
            for (const auto& c : chs) {
                if (c.startSec < now_t - eps && c.startSec > target)
                    target = c.startSec;
            }
            if (target < 0.0) target = 0.0;
            m_player.SeekTo(target);
        }
        if (!hasChapters) ImGui::EndDisabled();
        TooltipFor("Previous chapter", "J");
        ImGui::SameLine();
        if (ImGui::Button("<<30")) { m_player.SeekRelative(-30.0); }
        TooltipFor("Back 30s", "Shift + Left");
        ImGui::SameLine();
        if (ImGui::Button("<<5")) { m_player.SeekRelative(-5.0); }
        TooltipFor("Back 5s", "Left");
        ImGui::SameLine();
        if (ImGui::Button("<<1")) { m_player.SeekRelative(-1.0); }
        TooltipFor("Back 1s", back1Sc.c_str());
        ImGui::SameLine();
        if (ImGui::Button("<")) { m_player.StepFrame(-1); }
        TooltipFor("Previous frame", prevFrameSc.c_str());
        ImGui::SameLine();
        if (ImGui::Button(playLabel, ImVec2(playBtnW, 0))) {
            m_player.TogglePlayPause();
        }
        TooltipFor("Play / Pause", "Space");
        ImGui::SameLine();
        if (ImGui::Button(">")) { m_player.StepFrame(+1); }
        TooltipFor("Next frame", nextFrameSc.c_str());
        ImGui::SameLine();
        if (ImGui::Button("1>>")) { m_player.SeekRelative(1.0); }
        TooltipFor("Forward 1s", fwd1Sc.c_str());
        ImGui::SameLine();
        if (ImGui::Button("5>>")) { m_player.SeekRelative(5.0); }
        TooltipFor("Forward 5s", "Right");
        ImGui::SameLine();
        if (ImGui::Button("30>>")) { m_player.SeekRelative(30.0); }
        TooltipFor("Forward 30s", "Shift + Right");
        ImGui::SameLine();
        if (!hasChapters) ImGui::BeginDisabled();
        if (ImGui::Button(">|>")) {
            double now_t = m_player.GetPlaybackTime();
            const double eps = 0.05;
            double target = -1.0;
            for (const auto& c : chs) {
                if (c.startSec > now_t + eps && (target < 0.0 || c.startSec < target))
                    target = c.startSec;
            }
            if (target >= 0.0) m_player.SeekTo(target);
        }
        if (!hasChapters) ImGui::EndDisabled();
        TooltipFor("Next chapter", "K");
        ImGui::SameLine();
        if (ImGui::Button(">|")) { m_player.SeekTo(duration); }
        TooltipFor("Jump to end", "End");

        // Mark buttons — centered between transport and audio controls
        float markGroupGap = 12.0f * dpi;  // extra gap between segment buttons and the frame button
        {
            float afterTransport = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x;
            float muteW_ = std::max(ImGui::CalcTextSize("Mute").x, ImGui::CalcTextSize("Unmute").x) + ImGui::GetStyle().FramePadding.x * 2;
            float audioStart = panelWidth - muteW_ - ImGui::GetStyle().ItemSpacing.x - 80.0f;
            float bracketW = ImGui::CalcTextSize("[").x + ImGui::GetStyle().FramePadding.x * 2;
            float frameBtnW = ImGui::CalcTextSize("[]").x + ImGui::GetStyle().FramePadding.x * 2;
            float segBtnsW = bracketW * 2 + ImGui::GetStyle().ItemSpacing.x + markGroupGap + frameBtnW;
            float segStart = afterTransport + (audioStart - afterTransport - segBtnsW) * 0.5f;
            ImGui::SameLine(segStart);
        }
        if (ImGui::Button("[")) {
            int before = m_segments.GetCount();
            m_segments.SetMarkIn(m_player.GetPlaybackTime());
            if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                m_showSegments = true;
            m_lastUIActivityNS = SDL_GetTicksNS();
            if (m_autoHideUI) m_uiHidden = false;
        }
        TooltipFor("Mark segment start for export", "I  or  [");
        ImGui::SameLine();
        bool canMarkOut = m_segments.HasPendingMarkIn() || m_segments.GetCount() > 0;
        if (!canMarkOut) ImGui::BeginDisabled();
        if (ImGui::Button("]")) {
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
        TooltipFor("Mark segment end for export", "O  or  ]");
        ImGui::SameLine(0.0f, markGroupGap);
        if (ImGui::Button("[]")) {
            if (m_player.HasMedia()) {
                int before = m_segments.GetTotalCount();
                m_segments.AddFrame(m_player.GetPlaybackTime());
                if (m_segments.GetTotalCount() > before && !m_showSegments && !m_segmentsClosedManually)
                    m_showSegments = true;
                m_lastUIActivityNS = SDL_GetTicksNS();
                if (m_autoHideUI) m_uiHidden = false;
            }
        }
        TooltipFor("Mark frame for PNG export", "P");

        // Right: Mute + volume slider
        {
            bool muted = m_player.IsMuted();
            bool noAudio = hasMedia && !m_player.HasAudio();
            float muteW = std::max(ImGui::CalcTextSize("Mute").x, ImGui::CalcTextSize("Unmute").x) + ImGui::GetStyle().FramePadding.x * 2;
            float sliderW = 80.0f;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float volumeAreaWidth = muteW + spacing + sliderW;
            ImGui::SameLine(panelWidth - volumeAreaWidth);
            if (noAudio) ImGui::BeginDisabled();
            if (ImGui::Button(muted ? "Unmute" : "Mute", ImVec2(muteW, 0))) {
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

        ImVec2 mousePos = ImGui::GetIO().MousePos;

        // Background — single rect when the file has no chapters, or one rect
        // per chapter (with a small gap between) when it does. The hovered
        // chapter region brightens slightly and shows a tooltip with the title.
        ImVec4 frameBg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        ImU32 bgCol = IM_COL32(static_cast<int>(frameBg.x * 255), static_cast<int>(frameBg.y * 255),
                               static_cast<int>(frameBg.z * 255), static_cast<int>(frameBg.w * 255 * m_uiAlpha));
        const auto& chapters = m_player.GetChapters();
        int hoveredChapter = -1;
        bool hoveredChapterOverflows = false;
        if (duration > 0.0 && !chapters.empty() && m_showChapters) {
            const float kChapGap = 2.0f;
            bool overBar = mousePos.y >= barPos.y && mousePos.y <= barPos.y + barHeight;
            for (int i = 0; i < static_cast<int>(chapters.size()); i++) {
                float x0 = barPos.x + static_cast<float>(chapters[i].startSec / duration) * barWidth;
                float x1 = barPos.x + static_cast<float>(chapters[i].endSec   / duration) * barWidth;
                if (i + 1 < static_cast<int>(chapters.size())) x1 -= kChapGap;
                if (x1 <= x0) continue;
                bool hovered = overBar && mousePos.x >= x0 && mousePos.x <= x1;
                if (hovered) hoveredChapter = i;
                drawList->AddRectFilled(ImVec2(x0, barPos.y), ImVec2(x1, barPos.y + barHeight), bgCol);
                if (hovered) {
                    drawList->AddRectFilled(ImVec2(x0, barPos.y), ImVec2(x1, barPos.y + barHeight),
                                            IM_COL32(255, 255, 255, static_cast<int>(12 * m_uiAlpha)));
                }

                // Chapter title drawn inside the rect, clipped so long titles
                // don't bleed into the next chapter. Falls back to "Chapter N"
                // when the file's chapter has no title set.
                char fallback[32];
                const char* label = chapters[i].title.empty()
                    ? (snprintf(fallback, sizeof(fallback), "Chapter %d", i + 1), fallback)
                    : chapters[i].title.c_str();
                const float labelPadX = 6.0f;
                ImVec2 textSize = ImGui::CalcTextSize(label);
                float textY = barPos.y + (barHeight - textSize.y) * 0.5f;
                int textAlpha = static_cast<int>((hovered ? 230 : 160) * m_uiAlpha);
                ImU32 textCol = IM_COL32(255, 255, 255, textAlpha);
                float availTextW = (x1 - 2.0f) - (x0 + labelPadX);
                if (hovered && textSize.x > availTextW)
                    hoveredChapterOverflows = true;
                drawList->PushClipRect(ImVec2(x0 + 2.0f, barPos.y),
                                       ImVec2(x1 - 2.0f, barPos.y + barHeight), true);
                drawList->AddText(ImVec2(x0 + labelPadX, textY), textCol, label);
                drawList->PopClipRect();
            }
            if (hoveredChapter >= 0 && hoveredChapterOverflows) {
                const Chapter& ch = chapters[hoveredChapter];
                if (!ch.title.empty())
                    ImGui::SetTooltip("%s", ch.title.c_str());
                else
                    ImGui::SetTooltip("Chapter %d", hoveredChapter + 1);
            }
        } else {
            drawList->AddRectFilled(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight), bgCol);
        }

        // Segments
        const auto& segs = m_segments.GetSegments();
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

        // Frame marks — vertical line + triangle caps at top and bottom of the
        // bar (pointing inward). Draw the two caps as stacked integer-height
        // rectangles (a discrete pixel pyramid) instead of AA triangles — this
        // gives pixel-perfect symmetric rasterization. ImGui's AA triangle
        // fill was rendering the bottom cap visibly taller than the top even
        // with matching vertex coords, due to the direction the rasterizer
        // distributes sub-pixel coverage across flat-top vs flat-bottom edges.
        const auto& frames = m_segments.GetFrames();
        if (duration > 0.0) {
            // Use odd pixel widths so the pyramid apex (1 pixel) aligns
            // exactly with the vertical line (1 pixel), and same for hover
            // (apex and line both 3 pixels wide). ImGui's AddLine was being
            // centered between pixels for thickness=2 which left a 1-pixel
            // misalignment vs. integer-positioned triangle apexes.
            int triHalf = static_cast<int>(std::round(4.0f * dpi));
            int topY = static_cast<int>(std::round(barPos.y));
            int botY = static_cast<int>(std::round(barPos.y + barHeight));
            drawList->PushClipRect(ImVec2(barPos.x, static_cast<float>(topY)),
                                   ImVec2(barPos.x + barWidth, static_cast<float>(botY)), true);
            for (int i = 0; i < static_cast<int>(frames.size()); i++) {
                int fxi = static_cast<int>(std::round(barPos.x + static_cast<float>(frames[i].timeSec / duration) * barWidth));
                float fx = static_cast<float>(fxi);
                bool barHovered = mousePos.y >= topY && mousePos.y <= botY &&
                                  std::abs(mousePos.x - fx) <= triHalf + 1.0f;
                if (barHovered) m_hoveredFrameThisFrame = i;
                bool highlighted = (m_hoveredFrame == i);
                float alpha = highlighted ? 1.0f : 0.9f;
                int lineHalf = highlighted ? 1 : 0;            // 3 px line on hover, else 1 px
                int pyramidHalf = triHalf + (highlighted ? 2 : 0);  // grow base on hover
                ImU32 col = GetSegmentColor(frames[i].colorIndex, alpha * m_uiAlpha);
                // Vertical line as a pixel-aligned rect (not AddLine) so it
                // cannot drift half a pixel relative to the pyramid caps.
                drawList->AddRectFilled(
                    ImVec2(fx - static_cast<float>(lineHalf), static_cast<float>(topY)),
                    ImVec2(fx + static_cast<float>(lineHalf + 1), static_cast<float>(botY)),
                    col);
                // Pyramid caps: row k has half-width (pyramidHalf - k) and
                // spans 2*halfW+1 pixels centered on fx. Stops at halfW ==
                // lineHalf so the apex matches the line width exactly.
                for (int halfW = pyramidHalf; halfW >= lineHalf; halfW--) {
                    int k = pyramidHalf - halfW;
                    float x0 = fx - static_cast<float>(halfW);
                    float x1 = fx + static_cast<float>(halfW + 1);
                    float yt = static_cast<float>(topY + k);
                    float yb = static_cast<float>(botY - k - 1);
                    drawList->AddRectFilled(ImVec2(x0, yt), ImVec2(x1, yt + 1.0f), col);
                    drawList->AddRectFilled(ImVec2(x0, yb), ImVec2(x1, yb + 1.0f), col);
                }
            }
            drawList->PopClipRect();
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
                double newStart = ComputeScrubTarget(mouseX, barWidth, duration,
                                                     segs[i].startSec, ImGui::IsItemActivated());
                newStart = std::max(0.0, std::min(newStart, segs[i].endSec - 0.01));
                m_segments.UpdateSegment(i, newStart, segs[i].endSec);
                m_seekTarget = newStart;
                handleActive = true;

                if (!m_isTimelineSeeking) {
                    m_wasPlayingBeforeTimelineSeek = m_player.IsPlaying();
                    if (m_wasPlayingBeforeTimelineSeek) m_player.Pause();
                    m_isTimelineSeeking = true;
                    m_player.SetScrubbing(true);
                }
                uint64_t now = SDL_GetTicksNS();
                if (now - m_lastSeekTime > 33000000ULL) {
                    m_player.SeekTo(newStart);
                    m_lastSeekTime = now;
                }
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (!ImGui::IsItemActive() && ImGui::IsItemDeactivated() && m_isTimelineSeeking) {
                m_player.SetScrubbing(false);
                // SeekTo on release in both cases: when was-playing it
                // resumes playback; when was-paused the SeekThread's
                // REUSE fast-path runs PopulateCacheAroundCurrent (cache
                // population is skipped during drag when scrubbing=true).
                m_player.SeekTo(m_seekTarget, m_wasPlayingBeforeTimelineSeek);
                m_isTimelineSeeking = false;
            }

            // Right handle
            ImGui::SetCursorScreenPos(ImVec2(x1 - handleW * 0.5f, barPos.y));
            snprintf(idBuf, sizeof(idBuf), "##seg_r_%d", i);
            ImGui::InvisibleButton(idBuf, ImVec2(handleW, barHeight));
            if (ImGui::IsItemActive()) {
                float mouseX = ImGui::GetIO().MousePos.x - barPos.x;
                double newEnd = ComputeScrubTarget(mouseX, barWidth, duration,
                                                   segs[i].endSec, ImGui::IsItemActivated());
                newEnd = std::max(segs[i].startSec + 0.01, std::min(newEnd, duration));
                m_segments.UpdateSegment(i, segs[i].startSec, newEnd);
                m_seekTarget = newEnd;
                handleActive = true;

                if (!m_isTimelineSeeking) {
                    m_wasPlayingBeforeTimelineSeek = m_player.IsPlaying();
                    if (m_wasPlayingBeforeTimelineSeek) m_player.Pause();
                    m_isTimelineSeeking = true;
                    m_player.SetScrubbing(true);
                }
                uint64_t now = SDL_GetTicksNS();
                if (now - m_lastSeekTime > 33000000ULL) {
                    m_player.SeekTo(newEnd);
                    m_lastSeekTime = now;
                }
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (!ImGui::IsItemActive() && ImGui::IsItemDeactivated() && m_isTimelineSeeking) {
                m_player.SetScrubbing(false);
                m_player.SeekTo(m_seekTarget, m_wasPlayingBeforeTimelineSeek);
                m_isTimelineSeeking = false;
            }
        }

        // Frame marker drag handles (rendered after segment handles so they
        // take priority when overlapping, and before the click-to-seek fallback)
        for (int i = 0; i < static_cast<int>(frames.size()); i++) {
            float fx = barPos.x + static_cast<float>(frames[i].timeSec / duration) * barWidth;
            float handleW = 10.0f;

            ImGui::SetCursorScreenPos(ImVec2(fx - handleW * 0.5f, barPos.y));
            char idBuf[32];
            snprintf(idBuf, sizeof(idBuf), "##frm_%d", i);
            ImGui::InvisibleButton(idBuf, ImVec2(handleW, barHeight));
            if (ImGui::IsItemActive()) {
                float mouseX = ImGui::GetIO().MousePos.x - barPos.x;
                double newTime = ComputeScrubTarget(mouseX, barWidth, duration,
                                                    frames[i].timeSec, ImGui::IsItemActivated());
                newTime = std::max(0.0, std::min(newTime, duration));
                m_segments.UpdateFrame(i, newTime);
                m_seekTarget = newTime;
                handleActive = true;
                m_hoveredFrameThisFrame = i;

                if (!m_isTimelineSeeking) {
                    m_wasPlayingBeforeTimelineSeek = m_player.IsPlaying();
                    if (m_wasPlayingBeforeTimelineSeek) m_player.Pause();
                    m_isTimelineSeeking = true;
                    m_player.SetScrubbing(true);
                }
                uint64_t now = SDL_GetTicksNS();
                if (now - m_lastSeekTime > 33000000ULL) {
                    m_player.SeekTo(newTime);
                    m_lastSeekTime = now;
                }
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (!ImGui::IsItemActive() && ImGui::IsItemDeactivated() && m_isTimelineSeeking) {
                m_player.SetScrubbing(false);
                m_player.SeekTo(m_seekTarget, m_wasPlayingBeforeTimelineSeek);
                m_isTimelineSeeking = false;
            }
        }

        // Click-to-seek on timeline bar (only if no handle is active). With
        // Ctrl held at mouse-down, the interaction creates a mark instead:
        // a click adds a Frame, a drag adds a Segment.
        ImGui::SetCursorScreenPos(barPos);
        ImGui::InvisibleButton("##timeline_bar", ImVec2(barWidth, barHeight));

        if (ImGui::IsItemActivated() && !handleActive) {
            float mouseX = ImGui::GetIO().MousePos.x - barPos.x;
            double t = std::max(0.0, std::min(static_cast<double>(mouseX / barWidth) * duration, duration));
            m_barCtrlMode = ImGui::GetIO().KeyCtrl;
            m_barCtrlStartX = mouseX;
            m_barCtrlStartTime = t;
            m_barCtrlSegIdx = -1;
        }

        if (ImGui::IsItemActive() && !handleActive) {
            float mouseX = ImGui::GetIO().MousePos.x - barPos.x;
            // Alt-click: anchor at the current playhead instead of jumping to
            // the click position. Lets the user release/re-click while
            // precision-scrubbing without disrupting the timeline.
            bool altHeld = (ImGui::GetIO().KeyMods & ImGuiMod_Alt) != 0;
            double initial = altHeld
                ? m_seekTarget
                : static_cast<double>(mouseX / barWidth) * duration;
            double clickTime = ComputeScrubTarget(mouseX, barWidth, duration,
                                                  initial, ImGui::IsItemActivated());
            clickTime = std::max(0.0, std::min(clickTime, duration));
            m_seekTarget = clickTime;

            // Create-mode: once the mouse drags past a small threshold,
            // promote the interaction from a pending Frame into a live
            // Segment that follows the scrub target.
            if (m_barCtrlMode) {
                const float dragThreshold = 5.0f;
                if (std::abs(mouseX - m_barCtrlStartX) >= dragThreshold) {
                    double a = std::min(m_barCtrlStartTime, clickTime);
                    double b = std::max(m_barCtrlStartTime, clickTime);
                    if (m_barCtrlSegIdx < 0) {
                        int before = m_segments.GetTotalCount();
                        m_barCtrlSegIdx = m_segments.AddSegment(a, b);
                        if (m_segments.GetTotalCount() > before && !m_showSegments && !m_segmentsClosedManually)
                            m_showSegments = true;
                    } else {
                        m_segments.UpdateSegment(m_barCtrlSegIdx, a, b);
                    }
                }
            }

            if (!m_isTimelineSeeking) {
                m_wasPlayingBeforeTimelineSeek = m_player.IsPlaying();
                if (m_wasPlayingBeforeTimelineSeek) m_player.Pause();
                m_isTimelineSeeking = true;
                m_player.SetScrubbing(true);
            }
            uint64_t now = SDL_GetTicksNS();
            if (now - m_lastSeekTime > 33000000ULL) {
                m_player.SeekTo(m_seekTarget);
                m_lastSeekTime = now;
            }
        }

        if (ImGui::IsItemDeactivated()) {
            if (m_barCtrlMode && m_barCtrlSegIdx < 0) {
                // Ctrl+click without drag → add a single-frame mark.
                int before = m_segments.GetTotalCount();
                m_segments.AddFrame(m_barCtrlStartTime);
                if (m_segments.GetTotalCount() > before && !m_showSegments && !m_segmentsClosedManually)
                    m_showSegments = true;
            }
            m_barCtrlMode = false;
            m_barCtrlSegIdx = -1;
        }

        if (m_isTimelineSeeking && !ImGui::IsItemActive()) {
            m_player.SetScrubbing(false);
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
    float dpi = m_ui.GetDpiScale();
    float marksW = 450.0f * dpi;
    float marksH = 250.0f * dpi;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - marksW - 40 * dpi, vp->WorkPos.y + 40 * dpi), layoutCond);
    ImGui::SetNextWindowSize(ImVec2(marksW, marksH), layoutCond);
    bool segmentsWasOpen = m_showSegments;
    ImGui::Begin("Marks", &m_showSegments);

    // Pending mark-in indicator
    if (m_segments.HasPendingMarkIn()) {
        double markIn = m_segments.GetPendingMarkIn();
        int mMin = static_cast<int>(markIn) / 60;
        int mSec = static_cast<int>(markIn) % 60;
        int mMs  = static_cast<int>(markIn * 1000) % 1000;
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Mark In: %02d:%02d.%03d", mMin, mSec, mMs);
    }

    // Scrollable mark list — segments and frames interleaved by timestamp
    float bottomHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 4;
    if (ImGui::BeginChild("##seglist", ImVec2(0, -bottomHeight), ImGuiChildFlags_None)) {
        const auto& segs = m_segments.GetSegments();
        const auto& frames = m_segments.GetFrames();
        if (!segs.empty() || !frames.empty()) {
            struct Row { bool isFrame; int index; double t; uint64_t seq; };
            std::vector<Row> rows;
            rows.reserve(segs.size() + frames.size());
            for (int i = 0; i < static_cast<int>(segs.size()); i++)
                rows.push_back({false, i, segs[i].startSec, segs[i].addSeq});
            for (int i = 0; i < static_cast<int>(frames.size()); i++)
                rows.push_back({true, i, frames[i].timeSec, frames[i].addSeq});
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                if (a.t != b.t) return a.t < b.t;
                return a.seq < b.seq;
            });

            int removeSegIdx = -1;
            int removeFrameIdx = -1;
            float cardPad = 4.0f;

            for (size_t r = 0; r < rows.size(); r++) {
                const Row& row = rows[r];
                // Stable ID: addSeq is monotonic and unique per mark, so text-cursor /
                // focus state survives re-sorting when a new mark is added.
                uint64_t stableId = row.isFrame ? frames[row.index].addSeq : segs[row.index].addSeq;
                ImGui::PushID(static_cast<int>(stableId));

                ImVec2 cardStart = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(cardStart.x + cardPad, cardStart.y + cardPad));
                ImGui::BeginGroup();

                // Row 1: color square, type label, name field, delete button
                int colorIdx = row.isFrame ? frames[row.index].colorIndex : segs[row.index].colorIndex;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 sq = ImGui::GetCursorScreenPos();
                float sqSize = ImGui::GetTextLineHeight();
                dl->AddRectFilled(sq, ImVec2(sq.x + sqSize, sq.y + sqSize), GetSegmentColor(colorIdx));
                ImGui::Dummy(ImVec2(sqSize, sqSize));
                ImGui::SameLine();
                ImGui::TextDisabled("%s", row.isFrame ? "Frame" : "Segment");
                ImGui::SameLine();

                // Measure widths so the format indicator and delete button can
                // be right-aligned to the same column regardless of whether
                // this row is a Frame (shows "PNG" text) or a Segment (shows
                // a "MP4"/"GIF" SmallButton).
                float framePadX = ImGui::GetStyle().FramePadding.x * 2;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float xBtnW = ImGui::CalcTextSize("X").x + framePadX;
                float mp4W = ImGui::CalcTextSize("MP4").x + framePadX;
                float gifW = ImGui::CalcTextSize("GIF").x + framePadX;
                float pngW = ImGui::CalcTextSize("PNG").x;
                float maxFmtW = std::max(std::max(mp4W, gifW), pngW);

                float nameStartX = ImGui::GetCursorPosX();
                float rightEdge = nameStartX + ImGui::GetContentRegionAvail().x;
                // Stop one cardPad short of the window content edge so the
                // card's hover-highlight rect (which adds cardPad past the
                // rightmost item) lands on the edge, not past it.
                float xBtnX = rightEdge - cardPad - xBtnW;
                float fmtRightX = xBtnX - spacing;

                char nameBuf[64];
                const std::string& curName = row.isFrame ? frames[row.index].name : segs[row.index].name;
                snprintf(nameBuf, sizeof(nameBuf), "%s", curName.c_str());
                ImGui::SetNextItemWidth(fmtRightX - maxFmtW - spacing - nameStartX);
                if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
                    if (row.isFrame) m_segments.SetFrameName(row.index, nameBuf);
                    else             m_segments.SetSegmentName(row.index, nameBuf);
                }

                // Format indicator: right-aligned so its right edge sits just
                // before the delete button, regardless of its intrinsic width.
                float fmtActualW = row.isFrame ? pngW
                    : ((segs[row.index].mode == ExportMode::GIF) ? gifW : mp4W);
                ImGui::SameLine();
                ImGui::SetCursorPosX(fmtRightX - fmtActualW);
                if (row.isFrame) {
                    ImGui::TextDisabled("PNG");
                } else {
                    const TimeRange& s = segs[row.index];
                    const char* fmtLabel = (s.mode == ExportMode::GIF) ? "GIF##fmt" : "MP4##fmt";
                    if (ImGui::SmallButton(fmtLabel)) {
                        ExportMode newMode = (s.mode == ExportMode::GIF)
                            ? ExportMode::SourceFormat : ExportMode::GIF;
                        m_segments.SetSegmentMode(row.index, newMode);
                    }
                    TooltipFor("Toggle export format");
                }

                // Delete button: pinned to the right edge.
                ImGui::SameLine();
                ImGui::SetCursorPosX(xBtnX);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
                if (ImGui::SmallButton("X")) {
                    if (row.isFrame) removeFrameIdx = row.index;
                    else             removeSegIdx   = row.index;
                }
                ImGui::PopStyleColor(3);
                TooltipFor("Delete this mark");

                if (!row.isFrame) {
                    // Row 2 (segments only): start/end seek buttons + set-to-current + duration
                    const TimeRange& s = segs[row.index];
                    int sMin = static_cast<int>(s.startSec) / 60;
                    int sSec = static_cast<int>(s.startSec) % 60;
                    int sMs  = static_cast<int>(s.startSec * 1000) % 1000;
                    int eMin = static_cast<int>(s.endSec) / 60;
                    int eSec = static_cast<int>(s.endSec) % 60;
                    int eMs  = static_cast<int>(s.endSec * 1000) % 1000;
                    char startBuf[32], endBuf[32];
                    snprintf(startBuf, sizeof(startBuf), "%02d:%02d.%03d##start", sMin, sSec, sMs);
                    snprintf(endBuf,   sizeof(endBuf),   "%02d:%02d.%03d##end",   eMin, eSec, eMs);
                    double playhead = m_seekTarget;
                    bool canSetStart = playhead < s.endSec;
                    bool canSetEnd   = playhead > s.startSec;
                    if (!canSetStart) ImGui::BeginDisabled();
                    if (ImGui::SmallButton(">[##setstart"))
                        m_segments.UpdateSegment(row.index, playhead, s.endSec);
                    if (!canSetStart) ImGui::EndDisabled();
                    TooltipFor("Set start to playhead");
                    ImGui::SameLine();
                    if (ImGui::SmallButton(startBuf)) m_player.SeekTo(s.startSec);
                    TooltipFor("Seek to this time");
                    ImGui::SameLine();
                    ImGui::Text("->");
                    ImGui::SameLine();
                    if (ImGui::SmallButton(endBuf)) m_player.SeekTo(s.endSec);
                    TooltipFor("Seek to this time");
                    ImGui::SameLine();
                    if (!canSetEnd) ImGui::BeginDisabled();
                    if (ImGui::SmallButton("]<##setend"))
                        m_segments.UpdateSegment(row.index, s.startSec, playhead);
                    if (!canSetEnd) ImGui::EndDisabled();
                    TooltipFor("Set end to playhead");

                    // Per-segment export playback speed (matches the timeline's
                    // discrete multipliers). SpeedCombo renders as a SmallButton
                    // so it fits inline with the other small buttons at the
                    // same row height.
                    ImGui::SameLine();
                    double newSpeed;
                    if (SpeedCombo("##segspeed", s.speed, newSpeed, /*small*/ true)) {
                        m_segments.SetSegmentSpeed(row.index, newSpeed);
                    }
                    TooltipFor("Export playback speed");

                    double dur = s.endSec - s.startSec;
                    double effDur = (s.speed > 0.0) ? dur / s.speed : dur;
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%.1fs)", effDur);

                    // Audio toggle at the end of the row. Disabled with an
                    // explanatory tooltip when format/speed forces audio off.
                    ImGui::SameLine();
                    bool newKeep;
                    if (KeepAudioToggle("##segaudio", s, newKeep, /*small*/ true)) {
                        m_segments.SetSegmentKeepAudio(row.index, newKeep);
                    }
                    if (!AudioForciblyDropped(s)) TooltipFor("Toggle audio in export");
                } else {
                    // Row 2 (frames): seek button + set-to-current
                    const FrameMark& f = frames[row.index];
                    int fMin = static_cast<int>(f.timeSec) / 60;
                    int fSec = static_cast<int>(f.timeSec) % 60;
                    int fMs  = static_cast<int>(f.timeSec * 1000) % 1000;
                    char timeBuf[32];
                    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d.%03d##ftime", fMin, fSec, fMs);
                    if (ImGui::SmallButton(timeBuf)) m_player.SeekTo(f.timeSec);
                    TooltipFor("Seek to this time");
                    ImGui::SameLine();
                    if (ImGui::SmallButton(">[]<##fset"))
                        m_segments.UpdateFrame(row.index, m_seekTarget);
                    TooltipFor("Snap frame to playhead");
                }

                ImGui::EndGroup();

                ImVec2 cardEnd = ImVec2(ImGui::GetItemRectMax().x + cardPad,
                                        ImGui::GetItemRectMax().y + cardPad);
                ImGui::SetCursorScreenPos(ImVec2(cardStart.x, cardEnd.y));
                ImGui::Dummy(ImVec2(0, 0));

                bool cardHovered = ImGui::IsMouseHoveringRect(cardStart, cardEnd);
                if (cardHovered) {
                    if (row.isFrame) m_hoveredFrameThisFrame = row.index;
                    else             m_hoveredSegmentThisFrame = row.index;
                }

                bool highlighted = row.isFrame
                    ? (m_hoveredFrame == row.index)
                    : (m_hoveredSegment == row.index);
                if (highlighted) {
                    ImDrawList* dl2 = ImGui::GetWindowDrawList();
                    dl2->AddRectFilled(cardStart, cardEnd, GetSegmentColor(colorIdx, 0.15f), 3.0f);
                    dl2->AddRect(cardStart, cardEnd, GetSegmentColor(colorIdx, 0.5f), 3.0f);
                }

                if (r + 1 < rows.size()) ImGui::Separator();

                ImGui::PopID();
            }
            if (removeSegIdx >= 0)   m_segments.RemoveSegment(removeSegIdx);
            if (removeFrameIdx >= 0) m_segments.RemoveFrame(removeFrameIdx);
        }
    }
    ImGui::EndChild();

    // Bottom bar: Export + Clear All (always visible)
    ImGui::Separator();
    bool hasMarks = m_segments.GetTotalCount() > 0;
    bool canExport = hasMarks && !m_exporter.IsRunning();
    if (!canExport) ImGui::BeginDisabled();
    float btnWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.7f;
    if (ImGui::Button("Export", ImVec2(btnWidth, 0))) {
        m_showExportDialog = true;
        auto stem = std::filesystem::path(m_currentFilePath).stem().string();
        InitExportDir();
        snprintf(m_exportName, sizeof(m_exportName), "%s", stem.c_str());
        m_pendingExport = ExportSettings{};
    }
    if (!canExport) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!hasMarks) ImGui::BeginDisabled();
    if (ImGui::Button("Clear All", ImVec2(-1, 0))) {
        m_segments.Clear();
    }
    if (!hasMarks) ImGui::EndDisabled();

    ImGui::End();
    if (segmentsWasOpen && !m_showSegments) m_segmentsClosedManually = true;
    ImGui::PopStyleVar();
    } // m_showSegments

    // Help panel (toggled with ?)
    if (m_showHelpPanel && !m_uiHidden) {
        ImGui::SetNextWindowBgAlpha(0.85f * m_uiAlpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_uiAlpha);
        // Default position: top-left of the workspace. layoutCond is
        // FirstUseEver normally and Always on Reset Layout. On the reset
        // frame we also pass AlwaysAutoResize so the size snaps to the
        // shortcuts table's content size — that's the single source of
        // truth, not a hardcoded number duplicated in the reset path.
        float dpi = m_ui.GetDpiScale();
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + 40 * dpi, vp->WorkPos.y + 40 * dpi),
            layoutCond);
        ImGuiWindowFlags helpFlags =
            wasResetFrame ? ImGuiWindowFlags_AlwaysAutoResize : 0;
        ImGui::Begin("Help", &m_showHelpPanel, helpFlags);
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
            row("Volume up / down",     "Up / Down");
            row("Mute / unmute",        "M");
            row("Jump to start / end",  kKeys.jumpName);
            row("Prev / next chapter",  "J / K");
            row("Mark In",              "I  or  [");
            row("Mark Out",             "O  or  ]");
            row("Mark Frame",           "P");
            row("Remove last mark",     kKeys.deleteName);
            row("Export",               (std::string(kKeys.cmdName) + " + E").c_str());
            row("Toggle timeline",     (std::string(kKeys.winModName) + " + T").c_str());
            row("Toggle marks",        (std::string(kKeys.winModName) + " + M").c_str());
            row("Quit",                 kKeys.quitShortcut);
            row("Precision scrub",      (std::string(kKeys.altKeyName) + " + drag timeline").c_str());
            row("Add mark on timeline",
                (std::string(kKeys.cmdName) + " + click (frame) / drag (segment)").c_str());
            row("Toggle help",         (std::string(kKeys.winModName) + " + H  or  ?").c_str());

            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // Reset Layout briefly forced Marks/Help visible so their Begin blocks ran
    // with layoutCond = Always (re-applying default Pos/Size). Now re-hide them
    // so the user-visible end state matches the intended hidden defaults.
    if (wasResetFrame && m_hideFloatingWindowsAfterReset) {
        m_hideFloatingWindowsAfterReset = false;
        m_showSegments = false;
        m_showHelpPanel = false;
    }

    // Export dialog
    if (m_showExportDialog) {
        m_exporter.ResetProgress();
        m_exportChecked.assign(m_segments.GetCount(), true);
        m_frameExportChecked.assign(m_segments.GetFrameCount(), true);
        ImGui::OpenPopup("Export");
        m_showExportDialog = false;
    }

    if (ImGui::BeginPopupModal("Export", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& progress = m_exporter.GetProgress();
        const auto& segs = m_segments.GetSegments();
        const auto& frames = m_segments.GetFrames();
        // Source-format segment exports use the source's extension, except
        // MKV/WebM are remuxed to MP4 (so the cuts can be frame-accurate at
        // both ends via MP4 edit lists) — must match Exporter::ExportThread.
        std::string inputExt = std::filesystem::path(m_currentFilePath).extension().string();
        {
            std::string lower = inputExt;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (lower == ".mkv" || lower == ".webm") inputExt = ".mp4";
        }

        // Interleaved-by-time row list (same pattern as the Segments panel)
        struct Row { bool isFrame; int index; double t; uint64_t seq; };
        std::vector<Row> rows;
        rows.reserve(segs.size() + frames.size());
        for (int i = 0; i < static_cast<int>(segs.size()); i++)
            rows.push_back({false, i, segs[i].startSec, segs[i].addSeq});
        for (int i = 0; i < static_cast<int>(frames.size()); i++)
            rows.push_back({true, i, frames[i].timeSec, frames[i].addSeq});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.t != b.t) return a.t < b.t;
            return a.seq < b.seq;
        });

        auto isChecked = [&](const Row& row) -> bool {
            return row.isFrame
                ? (row.index < static_cast<int>(m_frameExportChecked.size()) && m_frameExportChecked[row.index])
                : (row.index < static_cast<int>(m_exportChecked.size()) && m_exportChecked[row.index]);
        };
        auto extForRow = [&](const Row& row) -> std::string {
            if (row.isFrame) return ".png";
            return (segs[row.index].mode == ExportMode::GIF) ? std::string(".gif") : inputExt;
        };
        auto nameForRow = [&](const Row& row) -> const std::string& {
            return row.isFrame ? frames[row.index].name : segs[row.index].name;
        };

        if (!progress.running && !progress.finished && !progress.error) {
            // --- Settings form ---
            const float dialogWidth = 600.0f;

            // Output directory mode: same-as-video vs custom (persistent path).
            ImGui::Text("Output Directory:");
            const char* modeLabels[] = { "Same as opened video", "Custom" };
            int modeIdx = static_cast<int>(m_exportDirMode);
            if (ImGui::RadioButton(modeLabels[0], &modeIdx, 0)) {
                // SameAsVideo: re-derive from current video, ignore custom dir.
                m_exportDirMode = ExportDirMode::SameAsVideo;
                InitExportDir();
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(modeLabels[1], &modeIdx, 1)) {
                m_exportDirMode = ExportDirMode::Custom;
                InitExportDir();
            }

            float padX = ImGui::GetStyle().FramePadding.x * 2;
            float dirSpacing = ImGui::GetStyle().ItemSpacing.x;
            float browseW = ImGui::CalcTextSize("Browse").x + padX;
            float openW = ImGui::CalcTextSize("Open").x + padX;
            bool sameAsVideo = (m_exportDirMode == ExportDirMode::SameAsVideo);

            if (sameAsVideo) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(dialogWidth - browseW - openW - dirSpacing * 2);
            ImGui::InputText("##dir", m_exportDir, sizeof(m_exportDir));
            ImGui::SameLine();
            if (ImGui::Button("Browse")) {
                // SDL's folder picker is async: the callback fires later
                // when the user confirms. The InputText above is the
                // (sync) target buffer — Browse just hands SDL a pointer.
                // We sync m_exportDir → m_exportCustomDir each frame below,
                // which catches both typed edits and Browse results.
                SDL_ShowOpenFolderDialog([](void* userdata, const char* const* filelist, int) {
                    if (filelist && filelist[0]) {
                        char* dst = static_cast<char*>(userdata);
                        snprintf(dst, 512, "%s", filelist[0]);
                    }
                }, m_exportDir, m_window, m_exportDir, false);
            }
            if (sameAsVideo) ImGui::EndDisabled();
            else snprintf(m_exportCustomDir, sizeof(m_exportCustomDir), "%s", m_exportDir);
            ImGui::SameLine();
            std::error_code dirCheckEc;
            bool dirExists = std::filesystem::is_directory(m_exportDir, dirCheckEc);
            if (!dirExists) ImGui::BeginDisabled();
            if (ImGui::Button("Open")) {
                OpenFolderInShell(m_exportDir);
            }
            if (!dirExists) ImGui::EndDisabled();

            ImGui::Text("Filename Base:");
            ImGui::SetNextItemWidth(dialogWidth);
            ImGui::InputText("##name", m_exportName, sizeof(m_exportName));

            // Mark list with checkboxes in scrollable table
            ImGui::Spacing();
            ImGui::Text("Marks to export:");
            float rowH = ImGui::GetTextLineHeightWithSpacing() + 4;
            ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
            ImVec2 tableSize(0, 0);
            if (static_cast<int>(rows.size()) > 10) {
                tableFlags |= ImGuiTableFlags_ScrollY;
                tableSize.y = 10 * rowH + 30;
            }
            if (ImGui::BeginTable("##export_marks", 7, tableFlags, tableSize)) {
                ImGui::TableSetupColumn("##chk_col", ImGuiTableColumnFlags_WidthFixed, 24);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 65);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Fmt", ImGuiTableColumnFlags_WidthFixed, 45);
                ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Audio", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();

                for (size_t r = 0; r < rows.size(); r++) {
                    const Row& row = rows[r];
                    uint64_t stableId = row.isFrame ? frames[row.index].addSeq : segs[row.index].addSeq;
                    ImGui::PushID(static_cast<int>(stableId));
                    ImGui::TableNextRow();

                    // Checkbox
                    ImGui::TableNextColumn();
                    bool checked = isChecked(row);
                    if (ImGui::Checkbox("##chk", &checked)) {
                        if (row.isFrame) m_frameExportChecked[row.index] = checked;
                        else             m_exportChecked[row.index] = checked;
                    }

                    // Type
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.isFrame ? "Frame" : "Segment");

                    // Editable name
                    ImGui::TableNextColumn();
                    char nameBuf[64];
                    snprintf(nameBuf, sizeof(nameBuf), "%s", nameForRow(row).c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
                        if (row.isFrame) m_segments.SetFrameName(row.index, nameBuf);
                        else             m_segments.SetSegmentName(row.index, nameBuf);
                    }

                    // Format (toggle for segments, static PNG label for frames)
                    ImGui::TableNextColumn();
                    if (row.isFrame) {
                        ImGui::TextDisabled("PNG");
                    } else {
                        const char* fmtLabel = (segs[row.index].mode == ExportMode::GIF) ? "GIF" : "MP4";
                        if (ImGui::SmallButton(fmtLabel)) {
                            ExportMode newMode = (segs[row.index].mode == ExportMode::GIF)
                                ? ExportMode::SourceFormat : ExportMode::GIF;
                            m_segments.SetSegmentMode(row.index, newMode);
                        }
                    }

                    // Speed (segments only) — placed before Duration so users
                    // see the multiplier they're applying right next to the
                    // effective output length.
                    ImGui::TableNextColumn();
                    if (row.isFrame) {
                        ImGui::TextDisabled("-");
                    } else {
                        double newSpeed;
                        if (SpeedCombo("##speed", segs[row.index].speed, newSpeed, /*small*/ true)) {
                            m_segments.SetSegmentSpeed(row.index, newSpeed);
                        }
                    }

                    // Duration (effective, after speed)
                    ImGui::TableNextColumn();
                    if (row.isFrame) {
                        ImGui::TextDisabled("-");
                    } else {
                        const TimeRange& s = segs[row.index];
                        double srcDur = s.endSec - s.startSec;
                        double effDur = (s.speed > 0.0) ? srcDur / s.speed : srcDur;
                        ImGui::Text("%.1fs", effDur);
                    }

                    // Audio toggle column. Frames have no audio anyway, so
                    // we just show a disabled label.
                    ImGui::TableNextColumn();
                    if (row.isFrame) {
                        ImGui::TextDisabled("-");
                    } else {
                        bool newKeep;
                        if (KeepAudioToggle("##audio", segs[row.index], newKeep, /*small*/ true)) {
                            m_segments.SetSegmentKeepAudio(row.index, newKeep);
                        }
                    }

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
                if (ImGui::CollapsingHeader("GIF Settings")) {
                    ImGui::SliderInt("Width", &m_pendingExport.gifWidth, 120, 1920);
                    float fps = static_cast<float>(m_pendingExport.gifFps);
                    if (ImGui::SliderFloat("FPS", &fps, 5.0f, 60.0f, "%.0f")) {
                        m_pendingExport.gifFps = static_cast<double>(fps);
                    }
                }
            }

            // Output file preview
            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Files to be exported", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string stem = std::filesystem::path(m_exportName).stem().string();
                if (stem.empty()) stem = m_exportName;
                ImGui::BeginChild("##export_preview", ImVec2(0, 80.0f * m_ui.GetDpiScale()),
                                  ImGuiChildFlags_Borders);
                bool anyForPreview = false;
                for (const Row& row : rows) {
                    if (!isChecked(row)) continue;
                    anyForPreview = true;
                    std::string ext = extForRow(row);
                    const std::string& nm = nameForRow(row);
                    std::string filename = nm.empty() ? std::string("<unnamed>") : (stem + "_" + nm + ext);
                    bool exists = !nm.empty() &&
                                  std::filesystem::exists(std::filesystem::path(m_exportDir) / filename);
                    if (exists)
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                                           "%s  (already exists)", filename.c_str());
                    else
                        ImGui::TextUnformatted(filename.c_str());
                }
                if (!anyForPreview)
                    ImGui::TextDisabled("(nothing selected)");
                ImGui::EndChild();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Validate: check for empty names and any checked
            bool anyChecked = false;
            bool hasEmptyName = false;
            for (const Row& row : rows) {
                if (!isChecked(row)) continue;
                anyChecked = true;
                if (nameForRow(row).empty()) hasEmptyName = true;
            }
            if (hasEmptyName)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "All marks must have a name.");

            auto triggerExport = [&]() {
                m_pendingExport.segments.clear();
                m_pendingExport.frames.clear();
                for (const Row& row : rows) {
                    if (!isChecked(row)) continue;
                    if (row.isFrame) m_pendingExport.frames.push_back(frames[row.index]);
                    else             m_pendingExport.segments.push_back(segs[row.index]);
                }
                m_pendingExport.outputPath = (std::filesystem::path(m_exportDir) / m_exportName).string();

                // Check for existing files
                std::filesystem::path base(m_pendingExport.outputPath);
                std::string stem = base.stem().string();
                std::string dir = base.parent_path().string();
                m_conflictingFiles.clear();
                for (const auto& seg : m_pendingExport.segments) {
                    std::string ext = (seg.mode == ExportMode::GIF) ? ".gif" : inputExt;
                    std::filesystem::path outPath = std::filesystem::path(dir) / (stem + "_" + seg.name + ext);
                    if (std::filesystem::exists(outPath))
                        m_conflictingFiles.push_back(outPath.filename().string());
                }
                for (const auto& fm : m_pendingExport.frames) {
                    std::filesystem::path outPath = std::filesystem::path(dir) / (stem + "_" + fm.name + ".png");
                    if (std::filesystem::exists(outPath))
                        m_conflictingFiles.push_back(outPath.filename().string());
                }
                if (m_conflictingFiles.empty())
                    m_exporter.Start(m_currentFilePath, m_pendingExport);
                else
                    m_showOverwriteConfirm = true;
            };

            bool canExport = anyChecked && !hasEmptyName;
            if (!canExport) ImGui::BeginDisabled();
            float expBtnWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.7f;
            if (ImGui::Button("Export", ImVec2(expBtnWidth, 0))) triggerExport();
            if (!canExport) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
                ImGui::CloseCurrentPopup();
            }

            // Enter → Export when the popup is focused (and no sub-modal is open)
            if (canExport && !m_showOverwriteConfirm &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                triggerExport();
            }
        } else if (progress.running) {
            // --- Progress display ---
            int cur = progress.currentItem;
            int total = progress.totalItems;
            ImGui::Text("Exporting %d of %d...", cur, total);
            ImGui::ProgressBar(progress.fraction, ImVec2(400, 0));

            if (ImGui::Button("Cancel Export", ImVec2(120, 0))) {
                m_exporter.Cancel();
                ImGui::CloseCurrentPopup();
            }
        } else if (progress.finished) {
            // --- Done ---
            ImGui::Dummy(ImVec2(500, 0));
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Export complete!");
            ImGui::Spacing();

            std::filesystem::path basePath(m_pendingExport.outputPath);
            std::filesystem::path dirPath = basePath.parent_path();
            std::string dir = dirPath.string();
            std::string stem = basePath.stem().string();
            ImGui::Text("Output folder:");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  %s", dir.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open")) {
                OpenFolderInShell(dirPath);
            }
            ImGui::Spacing();
            ImGui::Text("Exported files:");
            for (const auto& seg : m_pendingExport.segments) {
                std::string ext = (seg.mode == ExportMode::GIF) ? ".gif" : basePath.extension().string();
                if (ext.empty()) ext = ".mp4";
                ImGui::BulletText("%s_%s%s", stem.c_str(), seg.name.c_str(), ext.c_str());
            }
            for (const auto& fm : m_pendingExport.frames) {
                ImGui::BulletText("%s_%s.png", stem.c_str(), fm.name.c_str());
            }
            ImGui::Spacing();

            bool closeRequested = false;
            if (ImGui::Button("Close", ImVec2(-1, 0))) closeRequested = true;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                closeRequested = true;
            }
            if (closeRequested) ImGui::CloseCurrentPopup();
        } else if (progress.error) {
            // --- Error ---
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Export failed:");
            ImGui::TextWrapped("%s", progress.GetError().c_str());
            bool closeRequested = false;
            if (ImGui::Button("Close", ImVec2(120, 0))) closeRequested = true;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                closeRequested = true;
            }
            if (closeRequested) ImGui::CloseCurrentPopup();
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
                std::filesystem::path base(m_pendingExport.outputPath);
                std::string stem = base.stem().string();
                std::string dir = base.parent_path().string();
                std::vector<TimeRange> filteredSegs;
                for (const auto& seg : m_pendingExport.segments) {
                    std::string ext = (seg.mode == ExportMode::GIF) ? ".gif" : inputExt;
                    std::filesystem::path outPath = std::filesystem::path(dir) / (stem + "_" + seg.name + ext);
                    if (!std::filesystem::exists(outPath)) filteredSegs.push_back(seg);
                }
                std::vector<FrameMark> filteredFrames;
                for (const auto& fm : m_pendingExport.frames) {
                    std::filesystem::path outPath = std::filesystem::path(dir) / (stem + "_" + fm.name + ".png");
                    if (!std::filesystem::exists(outPath)) filteredFrames.push_back(fm);
                }
                m_pendingExport.segments = filteredSegs;
                m_pendingExport.frames = filteredFrames;
                if (!filteredSegs.empty() || !filteredFrames.empty())
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
    m_hoveredFrame = m_hoveredFrameThisFrame;

    if (m_screenshotPending) {
        TakeScreenshot();
        m_screenshotPending = false;
    }

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

float App::GetEffectiveDpiScale() const {
#ifdef __APPLE__
    // On macOS the OS already handles scaling via the window's point/pixel
    // separation, so we leave UI at 1.0.
    return 1.0f;
#else
    if (!m_useDpiScaling) return 1.0f;
    return SDL_GetWindowDisplayScale(m_window);
#endif
}

double App::ComputeScrubTarget(float mouseX, float barWidth, double duration,
                                double initialTime, bool justActivated) {
    bool altNow = ImGui::GetIO().KeyAlt;
    if (justActivated) {
        m_scrubAnchorX = mouseX;
        m_scrubAnchorTime = initialTime;
        m_scrubAltWasHeld = altNow;
    } else if (altNow != m_scrubAltWasHeld) {
        // Rebase so toggling Alt mid-drag doesn't snap the target.
        double prevScale = m_scrubAltWasHeld ? 0.1 : 1.0;
        double curTarget = m_scrubAnchorTime +
            static_cast<double>(mouseX - m_scrubAnchorX) / barWidth * duration * prevScale;
        m_scrubAnchorX = mouseX;
        m_scrubAnchorTime = curTarget;
        m_scrubAltWasHeld = altNow;
    }
    double scale = altNow ? 0.1 : 1.0;
    return m_scrubAnchorTime +
        static_cast<double>(mouseX - m_scrubAnchorX) / barWidth * duration * scale;
}

void App::InitExportDir() {
    auto videoParent = std::filesystem::path(m_currentFilePath).parent_path().string();
    if (m_exportDirMode == ExportDirMode::Custom) {
        // First time using Custom: seed from the opened video's parent so
        // the user has a sensible starting point rather than an empty box.
        if (m_exportCustomDir[0] == '\0') {
            snprintf(m_exportCustomDir, sizeof(m_exportCustomDir), "%s", videoParent.c_str());
        }
        snprintf(m_exportDir, sizeof(m_exportDir), "%s", m_exportCustomDir);
    } else {
        snprintf(m_exportDir, sizeof(m_exportDir), "%s", videoParent.c_str());
    }
}

void App::TakeScreenshot() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    if (w <= 0 || h <= 0) { LOG_WARN("Screenshot: invalid window size"); return; }

    std::vector<uint8_t> raw(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

    // OpenGL origin is bottom-left; image formats expect top-left. Flip rows.
    int stride = w * 4;
    std::vector<uint8_t> flipped(raw.size());
    for (int y = 0; y < h; y++)
        std::memcpy(flipped.data() + y * stride, raw.data() + (h - 1 - y) * stride, stride);

    auto dir = GetAppDataDir() / "screenshots";
    std::filesystem::create_directories(dir);
    m_screenshotCounter++;
    char name[64];
    snprintf(name, sizeof(name), "screenshot_%03d.png", m_screenshotCounter);
    auto path = (dir / name).string();

    if (stbi_write_png(path.c_str(), w, h, 4, flipped.data(), stride))
        LOG_INFO("Screenshot saved: %s", path.c_str());
    else
        LOG_ERROR("Screenshot failed to write: %s", path.c_str());
}

void App::RestoreFloatingWindowSnapshots() {
    auto restore = [](const FloatingWindowSnap& s, const char* name) {
        if (!s.valid) return;
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        if (!w || w->DockId != 0) return;
        w->Pos = s.pos;
        w->SizeFull = s.size;
        w->Size = s.size;
    };
    restore(m_snapTimeline, "Timeline");
    restore(m_snapSegments, "Marks");
    restore(m_snapHelp,     "Help");
}

void App::SetFullscreen(bool fullscreen) {
    if (fullscreen == m_fullscreen) return;
    m_fullscreen = fullscreen;

    // Snapshot / restore floating windows so repeated fullscreen toggles don't
    // accumulate rounding drift from proportional repositioning.
    if (fullscreen) {
        // If we're re-entering fullscreen before the previous exit settled,
        // keep the existing snapshots (they hold the correct windowed positions).
        if (!m_waitingForFullscreenExit) {
            auto snap = [](App::FloatingWindowSnap& s, const char* name) {
                ImGuiWindow* w = ImGui::FindWindowByName(name);
                if (!w || w->DockId != 0) { s.valid = false; return; }
                s.pos = w->Pos;
                s.size = w->SizeFull;
                s.valid = true;
            };
            snap(m_snapTimeline, "Timeline");
            snap(m_snapSegments, "Marks");
            snap(m_snapHelp,     "Help");
        }
        m_waitingForFullscreenExit = false;
    } else {
        // macOS animates the fullscreen exit — the viewport stays at
        // fullscreen size during the animation, then snaps to windowed.
        // Wait for that snap before restoring window positions.
        m_waitingForFullscreenExit = true;
    }

#ifdef _WIN32
    // SDL_SetWindowFullscreen uses exclusive fullscreen, which causes screen
    // flashing, slow Alt-Tab, and broken overlays. We want borderless fullscreen
    // instead. However, simply creating a borderless window at the display
    // resolution with an OpenGL context also triggers exclusive fullscreen due
    // to a known SDL3/driver issue. The workaround is to set Win32 window styles
    // directly, which avoids the driver's exclusive fullscreen detection.
    // See: https://github.com/libsdl-org/SDL/issues/12791
    SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));

    if (fullscreen) {
        SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
        SDL_GetWindowSize(m_window, &m_windowedW, &m_windowedH);

        SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
        SDL_Rect bounds;
        SDL_GetDisplayBounds(display, &bounds);

        SetWindowLongPtr(hwnd, GWL_STYLE, WS_OVERLAPPED | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowPos(hwnd, HWND_TOP, bounds.x, bounds.y, bounds.w, bounds.h,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        LONG style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        LONG exStyle = WS_EX_APPWINDOW;
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

        // SDL returns client area position and size, but SetWindowPos expects
        // outer window bounds (including title bar and borders). Use the
        // per-monitor-DPI-aware version so the border math matches the actual
        // monitor the window will appear on — AdjustWindowRectEx (non-DPI)
        // uses the process DPI and drifts across mixed-DPI multi-monitor setups.
        RECT rc = { m_windowedX, m_windowedY,
                    m_windowedX + m_windowedW, m_windowedY + m_windowedH };
        UINT dpi = GetDpiForWindow(hwnd);
        AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, dpi);

        SetWindowPos(hwnd, HWND_NOTOPMOST, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
#else
    // On macOS, SDL_SetWindowFullscreen works as expected.
    SDL_SetWindowFullscreen(m_window, fullscreen);
#endif
}
