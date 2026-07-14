#include "App.h"
#include "util/Log.h"
#include "util/Trace.h"
#include "util/CommandLine.h"
#include "util/AppPaths.h"
#include "scrubcut_version.h"
#include "scrubcut_shaders.h"

#include "stb/stb_image_write.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <future>

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
#include <shellapi.h>
#include <dxgi1_6.h>
// dxgi pulls in <rpcndr.h> (via <unknwn.h>), whose `small` macro collides
// with parameter names in this file.
#undef small
// We only need DwmSetWindowAttribute from dwmapi. Forward-declare instead of
// including <dwmapi.h>, which transitively pulls in <rpcndr.h> and its
// `small` macro that collides with our parameter names. Linker resolves the
// symbol via dwmapi.lib (linked in CMakeLists.txt).
extern "C" HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD dwAttribute,
                                                LPCVOID pvAttribute, DWORD cbAttribute);
#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif
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

// Open the system file browser with `file` highlighted (vs. just opening its
// parent folder). Same non-blocking constraints as OpenFolderInShell.
static void RevealInShell(const std::filesystem::path& file) {
#ifdef _WIN32
    std::wstring args = L"/select,\"" + file.wstring() + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    // `open -R` reveals (parent folder opens with the file selected) and
    // returns immediately, so no need to background it.
    std::string p = file.string();
    // Escape any double quotes in the path so the shell command can't break.
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '"') { p.insert(i, "\\"); i++; }
    }
    std::string cmd = "open -R \"" + p + "\"";
    std::system(cmd.c_str());
#else
    OpenFolderInShell(file.parent_path());
#endif
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
// format / speed / source forces audio off, the button is greyed out and the
// tooltip explains why. Returns true and writes the new `keepAudio` value if
// the user toggled.
//
// `sourceHasNoAudio=true` forces audio off regardless of mode/speed — used
// when the opened file has no audio stream (e.g. an animated GIF).
// `small=true` for SmallButton-height rows; `small=false` for the
// export-dialog table cell variant.
static bool KeepAudioToggle(const char* id, const TimeRange& range,
                            bool sourceHasNoAudio,
                            bool& outKeepAudio, bool small) {
    bool forced = sourceHasNoAudio || AudioForciblyDropped(range);
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
            const char* reason = sourceHasNoAudio
                ? "Source has no audio track."
                : (range.mode == ExportMode::GIF)
                    ? "GIF has no audio track."
                    : "Audio export is not supported with non-1x playback speeds.";
            ImGui::TextUnformatted(reason);
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

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // Kick off GPU device creation on a worker thread as early as possible —
    // creating a D3D12 device costs ~250 ms on some drivers and needs only
    // the video subsystem. It overlaps everything below (audio init, window
    // creation, settings, geometry, showing the pre-painted window) and is
    // joined right before SDL_ClaimWindowForGPUDevice.
    //
    // The D3D12 fewer-resource-slots property drops the Tier-2
    // resource-binding requirement (supports older GPUs like Intel
    // Haswell/Broadwell); legal because we bind no storage resources.
    // DXBC (SM 5.x) rather than DXIL: our shaders don't need SM6, DXBC works
    // on all D3D12 hardware, and declaring DXIL would force SDL's D3D12
    // driver probe to create a throwaway device just to check SM6 support —
    // another ~250 ms on the same drivers. With DXBC + fewer-resource-slots,
    // the probe is skipped entirely on Windows 11.
    std::future<SDL_GPUDevice*> deviceFuture = std::async(std::launch::async, []() {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXBC_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
        SDL_SetBooleanProperty(props,
            SDL_PROP_GPU_DEVICE_CREATE_D3D12_ALLOW_FEWER_RESOURCE_SLOTS_BOOLEAN, true);
#ifndef NDEBUG
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
#endif
        SDL_GPUDevice* device = SDL_CreateGPUDeviceWithProperties(props);
        SDL_DestroyProperties(props);
        return device;
    });

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR("SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        return false;
    }

    const char* windowTitle = "ScrubCut";
#ifndef NDEBUG
    windowTitle = "ScrubCut - Debug";
#endif

    // Create hidden so the user never sees the brief flash of the creation
    // size / position before we apply the saved geometry (and optional
    // maximize). SDL_ShowWindow is called as soon as the geometry is settled —
    // before the GPU init, so the window appears without waiting for it.
    m_window = SDL_CreateWindow(
        windowTitle,
        1280, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
        SDL_WINDOW_HIDDEN
    );

    if (!m_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    // Load settings, apply the saved window geometry, and show the window
    // BEFORE the GPU init below — creating a D3D12 device costs ~250 ms on
    // some drivers, and this way the window appears almost immediately (SDL
    // clears it to black until the first frame) instead of after the full
    // GPU setup.

    // Delete layout files before ImGui loads them
    bool resetLayout = CommandLine::Get().HasFlag("-resetlayout");
    if (resetLayout) {
        std::filesystem::remove(GetAppDataDir() / "imgui.ini");
        std::filesystem::remove(GetAppDataDir() / "layout.ini");
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
    m_showWaveform = m_prefSettings.GetBool("show_waveform", false);
    m_showTooltips = m_prefSettings.GetBool("show_tooltips", true);
    m_flashStatusOverlay = m_prefSettings.GetBool("flash_status_overlay", true);
    m_useDpiScaling = m_prefSettings.GetBool("use_dpi_scaling", true);
    m_userUiScale = std::clamp(m_prefSettings.GetFloat("ui_scale", 1.0f), 0.5f, 2.0f);
    m_subtitleScale = std::clamp(m_prefSettings.GetFloat("subtitle_scale", 1.0f), 0.5f, 3.0f);
    m_autoHideCursor = m_prefSettings.GetBool("auto_hide_cursor", true);
    m_autoHideUI = m_prefSettings.GetBool("auto_hide_ui", false);
    m_player.SetVolume(std::clamp(m_prefSettings.GetFloat("volume", 1.0f), 0.0f, 1.0f));
    m_player.SetMuted(m_prefSettings.GetBool("muted", false));
    m_exportDirMode = static_cast<ExportDirMode>(
        std::clamp(m_prefSettings.GetInt("export_dir_mode", 0), 0, 1));
    m_tonemapper = static_cast<Tonemapper>(std::clamp(
        m_prefSettings.GetInt("tonemapper", static_cast<int>(Tonemapper::Uncharted2)),
        0, kTonemapperCount - 1));
    m_hdrOutputEnabled = m_prefSettings.GetBool("hdr_output", true);
    {
        std::string customDir = m_prefSettings.GetString("export_custom_dir", "");
        snprintf(m_exportCustomDir, sizeof(m_exportCustomDir), "%s", customDir.c_str());
    }

    LoadRecentFiles();

    if (resetLayout) {
        // Default window size is scaled by the effective UI scale so UI elements
        // are readable on high-DPI displays. (GetEffectiveUiScale rather than
        // m_ui.GetUiScale() — ImGui isn't initialized yet at this point; the
        // values are identical.)
        float uiScale = GetEffectiveUiScale();
        SDL_SetWindowSize(m_window,
                          static_cast<int>(1280 * uiScale),
                          static_cast<int>(720 * uiScale));
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

#if !defined(_WIN32) && !defined(__APPLE__)
    // Block until the windowing system commits the requested size/position.
    // Without this, on X11/Wayland SDL_GetWindowSize returns the creation
    // default for several frames, and the proportional-resize logic in
    // Render() sees a spurious viewport jump and inflates floating panels.
    // On macOS, however, this considerably slows down startup, and the app
    // works just fine without.
    SDL_SyncWindow(m_window);
#endif

    // Capture the actual unmaximized geometry before any maximize call,
    // so m_windowedX/Y/W/H holds the right "restore" position later.
    SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
    SDL_GetWindowSize(m_window, &m_windowedW, &m_windowedH);

    // Restore maximized state if the previous session ended maximized.
    // SDL_EVENT_WINDOW_MAXIMIZED will fire and our handler sets m_maximized.
    // Applied while the window is still hidden so the user never sees the
    // windowed-size intermediate state.
    if (m_layoutSettings.GetBool("window_maximized", false)) {
        SDL_MaximizeWindow(m_window);
    }

    // Reveal the window — at the correct geometry and maximize state.
    SDL_ShowWindow(m_window);

    // During the GPU init below the freshly shown window would sit solid
    // black (SDL's WM_ERASEBKGND fill). Paint it with the UI background color
    // through the software window surface — no GPU needed — so it looks like
    // the app frame immediately. The surface is destroyed right away; the
    // window is claimed for the GPU device below.
    if (SDL_Surface* surface = SDL_GetWindowSurface(m_window)) {
        // What's visible once the UI is up isn't the raw 0.1f clear color but
        // the Viewport window's WindowBg (0.06, alpha 0.94) composited over
        // it: 0.06*0.94 + 0.1*0.06 = 0.0624 -> RGB(15,15,15).
        SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGB(surface, 15, 15, 15));
        SDL_UpdateWindowSurface(m_window);
        SDL_DestroyWindowSurface(m_window);
    }
    m_startupWindowShownNS = SDL_GetTicksNS();

    // Join the GPU device creation started at the top of Init.
    m_gpuDevice = deviceFuture.get();
    if (!m_gpuDevice) {
        LOG_ERROR("SDL_CreateGPUDeviceWithProperties failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("GPU driver: %s", SDL_GetGPUDeviceDriver(m_gpuDevice));

    // Claiming defaults to SDR composition + VSYNC present mode. HDR output
    // is content-gated, so this stays SDR until an HDR video opens — the call
    // here just initializes the SDR-white/headroom state.
    if (!SDL_ClaimWindowForGPUDevice(m_gpuDevice, m_window)) {
        LOG_ERROR("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    UpdateHDROutput();

    // The GPU HDR->SDR tone-mapper (m_tonemap) is created lazily when the
    // first HDR video opens — see fetchFrame in Run(). The Exporter's
    // background thread runs the same tone-map pass for HDR re-encode exports
    // (GIF/PNG) on its own command buffers of this device.
    m_exporter.SetTonemapDevice(m_gpuDevice);

    if (!m_ui.Init(m_window, m_gpuDevice, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT)) {
        LOG_ERROR("UIManager::Init failed");
        return false;
    }

    // Apply the initial UI scale (1.0 unless DPI scaling is enabled on Windows).
    m_ui.SetUiScale(GetEffectiveUiScale());

    m_startupInitDoneNS = SDL_GetTicksNS();
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

        // While HDR output is active, poll the OS HDR state every frame so
        // the UI tracks the SDR-brightness slider live (cached DXGI reads,
        // ~6 us; the slider changes without any event SDL forwards). When
        // inactive, the display-change events and the video-open check in
        // fetchFrame cover (dis)engagement.
        if (m_swapchainHDR)
            UpdateHDROutput();

        // Handle deferred file open (from dialog callback, possibly off-thread)
        if (m_pendingOpenImmediate) {
            m_pendingOpenImmediate = false;
            OpenFile(m_pendingOpenFilePath);
            m_pendingOpenFilePath.clear();
        }
        // Deferred subtitle-file open (dialog callback, possibly off-thread).
        if (!m_pendingSubtitlePath.empty()) {
            std::string p = m_pendingSubtitlePath;
            m_pendingSubtitlePath.clear();
            OpenSubtitleFile(p);
        }

        // Fetch frame before render (for async playback)
        // and after render (for sync seeks triggered by UI during Render)
        auto fetchFrame = [&]() {
            const uint8_t* rgba = nullptr;
            int w = 0, h = 0;
            if (m_player.TryGetVideoFrame(&rgba, &w, &h)) {
                VideoColorMode mode = m_player.GetVideoColorMode();
                if (w != m_videoWidth || h != m_videoHeight || mode != m_videoColorMode) {
                    const bool modeChanged = (mode != m_videoColorMode);
                    m_videoWidth = w;
                    m_videoHeight = h;
                    m_videoColorMode = mode;
                    CreateVideoTexture(w, h);
                    // HDR output is content-gated: (dis)engage as the opened
                    // video's color mode becomes known.
                    if (modeChanged)
                        UpdateHDROutput();
                }
                UploadFrame(rgba, w, h);
                // Lazily create the tone-map pipeline the first time an HDR
                // video shows up. Non-fatal on failure (don't retry per frame)
                // — SDR playback is unaffected; only HDR videos would then
                // display incorrectly.
                if (m_videoColorMode != VideoColorMode::SDR && !m_tonemap.IsReady() &&
                    !m_tonemapInitFailed && !m_tonemap.Init(m_gpuDevice)) {
                    m_tonemapInitFailed = true;
                    LOG_ERROR("HDR tone-mapper unavailable; HDR videos may display incorrectly");
                }
                // HDR frames carry their PQ/HLG transfer in a 10-bit texture;
                // map them before ImGui composites them: tone-mapped to SDR on
                // SDR output, passthrough at native brightness on HDR output.
                if (m_videoColorMode != VideoColorMode::SDR && m_tonemap.IsReady()) {
                    m_displayTexture = m_tonemap.Process(m_videoTexture, w, h,
                                                         m_videoColorMode,
                                                         m_player.GetVideoColorPrimaries(),
                                                         m_tonemapper,
                                                         m_swapchainHDR, m_hdrHeadroom,
                                                         m_sdrWhite);
                    m_lastProcessedTonemapper = m_tonemapper;
                    m_lastProcessedHDROut = m_swapchainHDR;
                    m_lastProcessedHeadroom = m_hdrHeadroom;
                    m_lastProcessedSDRWhite = m_sdrWhite;
                } else {
                    m_displayTexture = m_videoTexture;
                }
            }
        };

        // Re-run the HDR tone-map on the held frame when the operator changes
        // while paused — no new frame arrives then, so fetchFrame() won't.
        auto refreshTonemap = [&]() {
            if (m_videoColorMode != VideoColorMode::SDR && m_tonemap.IsReady() &&
                m_videoTexture &&
                (m_tonemapper != m_lastProcessedTonemapper ||
                 m_swapchainHDR != m_lastProcessedHDROut ||
                 m_hdrHeadroom != m_lastProcessedHeadroom ||
                 m_sdrWhite != m_lastProcessedSDRWhite)) {
                m_displayTexture = m_tonemap.Process(m_videoTexture, m_videoWidth,
                                                     m_videoHeight, m_videoColorMode,
                                                     m_player.GetVideoColorPrimaries(),
                                                     m_tonemapper,
                                                     m_swapchainHDR, m_hdrHeadroom,
                                                     m_sdrWhite);
                m_lastProcessedTonemapper = m_tonemapper;
                m_lastProcessedHDROut = m_swapchainHDR;
                m_lastProcessedHeadroom = m_hdrHeadroom;
                m_lastProcessedSDRWhite = m_sdrWhite;
            }
        };

        {
            TRACE_EVENT("TryGetVideoFrame");
            fetchFrame();
        }
        refreshTonemap();

        Render();

        // Pick up frames from seeks triggered during Render (timeline/slider drags)
        fetchFrame();
        // Apply a tone-mapper change made via the View > HDR menu this frame.
        refreshTonemap();
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

    // Save windowed mode geometry (tracked while not fullscreen and not
    // maximized — m_windowedX/Y/W/H holds the pre-maximize geometry).
    // m_maximized is tracked separately so reopening returns to maximized
    // state without losing the unmaximized fallback size.
    if (m_window) {
        m_layoutSettings.SetInt("window_x", m_windowedX);
        m_layoutSettings.SetInt("window_y", m_windowedY);
        m_layoutSettings.SetInt("window_width", m_windowedW);
        m_layoutSettings.SetInt("window_height", m_windowedH);
        m_layoutSettings.SetBool("window_maximized", m_maximized);
        m_layoutSettings.SetBool("show_timeline", m_showTimeline);
        m_layoutSettings.SetBool("show_segments", m_showSegments);
        m_layoutSettings.Save();

        m_prefSettings.SetBool("show_chapters", m_showChapters);
        m_prefSettings.SetBool("show_waveform", m_showWaveform);
        m_prefSettings.SetBool("show_tooltips", m_showTooltips);
        m_prefSettings.SetBool("flash_status_overlay", m_flashStatusOverlay);
        m_prefSettings.SetBool("use_dpi_scaling", m_useDpiScaling);
        m_prefSettings.SetFloat("ui_scale", m_userUiScale);
        m_prefSettings.SetFloat("subtitle_scale", m_subtitleScale);
        m_prefSettings.SetBool("auto_hide_cursor", m_autoHideCursor);
        m_prefSettings.SetBool("auto_hide_ui", m_autoHideUI);
        m_prefSettings.SetFloat("volume", m_player.GetVolume());
        m_prefSettings.SetBool("muted", m_player.IsMuted());
        m_prefSettings.SetInt("export_dir_mode", static_cast<int>(m_exportDirMode));
        m_prefSettings.SetString("export_custom_dir", m_exportCustomDir);
        m_prefSettings.SetInt("tonemapper", static_cast<int>(m_tonemapper));
        m_prefSettings.SetBool("hdr_output", m_hdrOutputEnabled);
        SaveRecentFiles();
        m_prefSettings.Save();
    }
    SDL_ShowCursor();

    // Stop any in-flight export (joins its thread) before tearing down the GPU
    // resources it may still be using, then drain the GPU before releasing.
    m_exporter.Cancel();
    if (m_gpuDevice)
        SDL_WaitForGPUIdle(m_gpuDevice);

    if (m_videoTexture) {
        SDL_ReleaseGPUTexture(m_gpuDevice, m_videoTexture);
        m_videoTexture = nullptr;
    }
    m_displayTexture = nullptr;
    if (m_frameTransfer) {
        SDL_ReleaseGPUTransferBuffer(m_gpuDevice, m_frameTransfer);
        m_frameTransfer = nullptr;
    }
    m_tonemap.Shutdown();

    m_ui.Shutdown();

    if (m_sceneTarget) {
        SDL_ReleaseGPUTexture(m_gpuDevice, m_sceneTarget);
        m_sceneTarget = nullptr;
    }
    for (int i = 0; i < kMaxCompositePipelines; i++) {
        if (m_compositePipelines[i]) {
            SDL_ReleaseGPUGraphicsPipeline(m_gpuDevice, m_compositePipelines[i]);
            m_compositePipelines[i] = nullptr;
        }
    }
    if (m_compositeSampler) {
        SDL_ReleaseGPUSampler(m_gpuDevice, m_compositeSampler);
        m_compositeSampler = nullptr;
    }
    if (m_gpuDevice) {
        SDL_ReleaseWindowFromGPUDevice(m_gpuDevice, m_window);
        SDL_DestroyGPUDevice(m_gpuDevice);
        m_gpuDevice = nullptr;
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
    // on a non-main thread, and OpenFile touches GPU resources and player
    // state owned by the main thread.
    m_pendingOpenFilePath = path;
    if (m_segments.GetCount() > 0)
        m_showOpenFileConfirm = true;
    else
        m_pendingOpenImmediate = true;
}

void App::OpenFile(const std::string& path) {
    m_player.Close();

    if (m_videoTexture) {
        SDL_ReleaseGPUTexture(m_gpuDevice, m_videoTexture);  // deferred-safe
        m_videoTexture = nullptr;
    }
    m_displayTexture = nullptr;
    m_videoWidth = 0;
    m_videoHeight = 0;

    if (!m_player.Open(path))
        return;

    m_currentFilePath = path;
    m_segments.Clear();
    m_segmentsClosedManually = false;

    // Reset subtitles to "off" and rebuild the track list from the new file's
    // embedded subtitle streams (session-only — no persistence across opens).
    ResetSubtitleState();

    // Kick off the background waveform scan only when enabled. Otherwise
    // reset so stale peaks from the previously-open file don't show up if
    // the user enables the waveform later (we Start it then).
    if (m_showWaveform) {
        m_waveform.Start(path, m_player.GetDuration());
    } else {
        m_waveform.Reset();
    }

    m_videoWidth = m_player.GetVideoWidth();
    m_videoHeight = m_player.GetVideoHeight();
    m_videoColorMode = m_player.GetVideoColorMode();
    CreateVideoTexture(m_videoWidth, m_videoHeight);
    // HDR output is content-gated — (dis)engage for the newly opened video.
    UpdateHDROutput();

    std::string title = "ScrubCut - " + path;
#ifndef NDEBUG
    title += " - Debug";
#endif
    SDL_SetWindowTitle(m_window, title.c_str());

    AddToRecent(path);

    m_player.Play();
}

// --- Media: audio / subtitle track selection ---

bool App::IsSubtitlePath(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt" || ext == "sub";
}

void App::ResetSubtitleState() {
    m_subtitleExtractor.Reset();
    m_activeSubtitle = -1;
    m_subtitleDelaySec = 0.0;
    m_subtitleTracks = m_player.GetSubtitleTracks(); // embedded tracks; externals appended later
}

void App::ResetSettings() {
    m_showChapters = true;
    m_showWaveform = false;
    m_showTooltips = true;
    m_flashStatusOverlay = true;
    m_autoHideCursor = true;
    m_autoHideUI = false;
    m_subtitleScale = 1.0f;
    m_useDpiScaling = true;
    m_userUiScale = 1.0f;
    m_tonemapper = Tonemapper::Uncharted2;
    if (!m_hdrOutputEnabled) {
        m_hdrOutputEnabled = true;
        UpdateHDROutput();
    }
    m_ui.SetUiScale(GetEffectiveUiScale());
}

void App::SubtitleSizeControl(const char* idSuffix) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Subtitle Size");
    ImGui::SameLine();
    const ImGuiStyle& st = ImGui::GetStyle();
    float btn = ImGui::GetFrameHeight();
    float fieldW = ImGui::CalcTextSize("0.00x").x + st.FramePadding.x * 2.0f;
    float inputW = fieldW + 2.0f * (btn + st.ItemInnerSpacing.x);
    float resetW = ImGui::CalcTextSize("1").x + st.FramePadding.x * 2.0f;
    float rowW = inputW + st.ItemInnerSpacing.x + resetW;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > rowW)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - rowW));
    ImGui::SetNextItemWidth(inputW);
    std::string inId = std::string("##subsize") + idSuffix;
    if (ImGui::InputFloat(inId.c_str(), &m_subtitleScale, 0.1f, 0.25f, "%.2fx"))
        m_subtitleScale = std::clamp(m_subtitleScale, 0.5f, 3.0f);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    std::string btnId = std::string("1##subsize") + idSuffix;
    if (ImGui::Button(btnId.c_str())) m_subtitleScale = 1.0f;
}

void App::SelectAudioTrack(int streamIndex) {
    m_player.SetAudioTrack(streamIndex);
}

void App::SelectSubtitleTrack(int index) {
    if (index < -1 || index >= static_cast<int>(m_subtitleTracks.size())) return;
    m_activeSubtitle = index;
    if (index < 0) { m_subtitleExtractor.Reset(); return; }
    const SubtitleTrackInfo& t = m_subtitleTracks[index];
    if (!t.textBased) { m_subtitleExtractor.Reset(); return; } // bitmap — nothing to render
    if (t.external) m_subtitleExtractor.Start(t.path, -1);
    else            m_subtitleExtractor.Start(m_currentFilePath, t.streamIndex);
}

void App::FlashAudioTrack() {
    const auto& tracks = m_player.GetAudioTracks();
    int active = m_player.GetActiveAudioStreamIndex();
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].streamIndex == active) {
            ShowStatus("Audio " + std::to_string(i + 1) + "/" +
                       std::to_string(tracks.size()) + ": " + tracks[i].title);
            return;
        }
    }
}

void App::FlashSubtitleTrack() {
    if (m_activeSubtitle < 0) { ShowStatus("Subtitles: Off"); return; }
    const SubtitleTrackInfo& t = m_subtitleTracks[m_activeSubtitle];
    ShowStatus("Subtitle " + std::to_string(m_activeSubtitle + 1) + "/" +
               std::to_string(m_subtitleTracks.size()) + ": " + t.title +
               (t.external ? " (file)" : ""));
}

void App::FlashSubtitleDelay() {
    char buf[48];
    // lround avoids "-0 ms" (a tiny negative or -0.0 printed via %+.0f).
    snprintf(buf, sizeof(buf), "Subtitle delay: %+ld ms",
             std::lround(m_subtitleDelaySec * 1000.0));
    ShowStatus(buf);
}

std::string App::ChapterLabel(int index) const {
    const auto& chs = m_player.GetChapters();
    if (index < 0 || index >= static_cast<int>(chs.size())) return "Chapter";
    const std::string& t = chs[index].title;
    return t.empty() ? ("Chapter " + std::to_string(index + 1)) : t;
}

void App::OpenSubtitleFile(const std::string& path) {
    if (!m_player.HasMedia()) return;
    SubtitleTrackInfo t;
    t.external = true;
    t.textBased = true;
    t.path = path;
    t.title = std::filesystem::path(path).filename().string();
    m_subtitleTracks.push_back(std::move(t));
    SelectSubtitleTrack(static_cast<int>(m_subtitleTracks.size()) - 1);
}

void App::CycleAudioTrack(int dir) {
    const auto& tracks = m_player.GetAudioTracks();
    if (tracks.size() < 2) return;
    int active = m_player.GetActiveAudioStreamIndex();
    int cur = 0;
    for (int i = 0; i < static_cast<int>(tracks.size()); i++)
        if (tracks[i].streamIndex == active) { cur = i; break; }
    int n = static_cast<int>(tracks.size());
    int next = ((cur + dir) % n + n) % n;
    SelectAudioTrack(tracks[next].streamIndex);
    FlashAudioTrack();
    BumpUIActivity();
}

void App::CycleSubtitleTrack(int dir) {
    // Cycle through n+1 states: off (-1) then each track 0..n-1.
    int n = static_cast<int>(m_subtitleTracks.size());
    if (n == 0) return;
    int states = n + 1;
    int cur = m_activeSubtitle + 1; // 0 = off
    int next = ((cur + dir) % states + states) % states;
    SelectSubtitleTrack(next - 1);
    FlashSubtitleTrack();
    BumpUIActivity();
}

void App::CycleTonemapper(int dir) {
    // On HDR output HDR video is shown natively — the operator has no effect,
    // so cycling it would be confusing. Flash why instead.
    if (m_swapchainHDR) {
        ShowStatus("HDR output active — no tone-mapping");
        BumpUIActivity();
        return;
    }
    int n = kTonemapperCount;
    int next = ((static_cast<int>(m_tonemapper) + dir) % n + n) % n;
    m_tonemapper = static_cast<Tonemapper>(next);
    FlashTonemapper();
    BumpUIActivity();
}

void App::FlashTonemapper() {
    ShowStatus(std::string("Tonemapper: ") + TonemapperName(m_tonemapper));
}

void App::NudgeSubtitleDelay(double deltaSec) {
    m_subtitleDelaySec += deltaSec;
    BumpUIActivity();
}

void App::ShowStatus(const std::string& msg) {
    m_statusMsg = msg;
    m_statusMsgStartNS = SDL_GetTicksNS();
}

void App::FlashLabeledTime(const std::string& label, double t) {
    int m  = static_cast<int>(t) / 60;
    int s  = static_cast<int>(t) % 60;
    int ms = static_cast<int>(t * 1000) % 1000;
    char buf[80];
    // "\xe2\x80\x94" is an em dash (explicit UTF-8 so it's charset-independent).
    snprintf(buf, sizeof(buf), "%s \xe2\x80\x94 %02d:%02d.%03d", label.c_str(), m, s, ms);
    ShowStatus(buf);
}

void App::FlashTime(const std::string& label) {
    FlashLabeledTime(label, m_player.GetSeekTargetTime());
}

void App::SeekRelativeWithFlash(double deltaSec) {
    m_player.SeekRelative(deltaSec);
    char lbl[24];
    snprintf(lbl, sizeof(lbl), "%+gs", deltaSec);
    FlashTime(lbl);
}
void App::SeekToWithFlash(double seconds, const std::string& label) {
    m_player.SeekTo(seconds);
    FlashTime(label);
}
void App::StepFrameWithFlash(int direction) {
    m_player.StepFrame(direction);
    FlashTime(direction < 0 ? "-1f" : "+1f");
}

void App::FlashSpeed() {
    char buf[24];
    snprintf(buf, sizeof(buf), "Speed: %gx", m_player.GetSpeed());
    ShowStatus(buf);
}

void App::FlashVolume() {
    char buf[24];
    snprintf(buf, sizeof(buf), "Volume: %d%%",
             static_cast<int>(std::round(m_player.GetVolume() * 100.0f)));
    ShowStatus(buf);
}

void App::RenderSubtitleOverlay(const ImVec2& imgMin, const ImVec2& imgMax) {
    if (m_activeSubtitle < 0) return;
    ImFont* font = m_ui.GetSubtitleFont();
    if (!font) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float videoH = imgMax.y - imgMin.y;
    float centerX = (imgMin.x + imgMax.x) * 0.5f;

    float margin = videoH * 0.035f;

    // Bottom anchor for a subtitle block of height blockH: hug the video bottom,
    // but lift above the Timeline panel only when that panel's rectangle actually
    // overlaps where the block would sit. (A timeline docked elsewhere — e.g. at
    // the top — leaves the subtitle at the bottom.)
    auto liftedBottom = [&](float blockH) -> float {
        float subBottom = imgMax.y - margin;
        float subTop = subBottom - blockH;
        if (m_showTimeline && !m_uiHidden) {
            // WasActive: the Viewport draws before the Timeline's Begin() this frame.
            if (ImGuiWindow* tw = ImGui::FindWindowByName("Timeline")) {
                if (tw->WasActive) {
                    float twTop = tw->Pos.y;
                    float twBot = tw->Pos.y + tw->Size.y;
                    if (twTop > imgMin.y && twTop < subBottom && twBot > subTop)
                        return twTop; // timeline overlaps the block — sit above it
                }
            }
        }
        return imgMax.y;
    };

    // Bitmap (unsupported) track that the user explicitly selected: show a note.
    if (m_activeSubtitle < static_cast<int>(m_subtitleTracks.size()) &&
        !m_subtitleTracks[m_activeSubtitle].textBased) {
        const char* msg = "Subtitle format not supported";
        float sz = std::clamp(videoH * 0.035f, 12.0f, 40.0f);
        ImVec2 ts = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, msg);
        float bottom = liftedBottom(ts.y);
        ImVec2 pos(centerX - ts.x * 0.5f, bottom - ts.y - margin);
        dl->AddText(font, sz, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 220), msg);
        dl->AddText(font, sz, pos, IM_COL32(255, 220, 120, 235), msg);
        return;
    }

    double t = m_player.GetPlaybackTime() - m_subtitleDelaySec;
    std::string text = m_subtitleExtractor.ActiveText(t);
    if (text.empty()) return;

    float fontSize = std::clamp(videoH * 0.05f * m_subtitleScale, 8.0f, 200.0f);
    float lineH = fontSize * 1.15f;

    std::vector<std::string> lines;
    for (size_t start = 0; start <= text.size();) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(text.substr(start)); break; }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }

    float totalH = lineH * static_cast<float>(lines.size());
    float bottom = liftedBottom(totalH);
    float baseY = bottom - margin - totalH;

    // Outline: sample the black copies evenly around a circle of uniform radius
    // (rather than 8 cardinal/diagonal offsets, which give a bumpy, star-shaped
    // edge), then lay the white fill on top. Smooth and even at any size.
    float r = std::max(1.5f, fontSize * 0.045f);
    constexpr int kOutlineSamples = 12;
    ImVec2 offs[kOutlineSamples];
    for (int k = 0; k < kOutlineSamples; k++) {
        float a = (static_cast<float>(k) / kOutlineSamples) * 6.28318531f;
        offs[k] = ImVec2(std::cos(a) * r, std::sin(a) * r);
    }
    ImU32 white = IM_COL32(255, 255, 255, 255);
    ImU32 black = IM_COL32(0, 0, 0, 235);

    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].empty()) continue;
        const char* s = lines[i].c_str();
        ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, s);
        float x = centerX - ts.x * 0.5f;
        float y = baseY + static_cast<float>(i) * lineH;
        for (const ImVec2& d : offs)
            dl->AddText(font, fontSize, ImVec2(x + d.x, y + d.y), black, s);
        dl->AddText(font, fontSize, ImVec2(x, y), white, s);
    }
}

void App::RenderStatusOverlay(const ImVec2& imgMin, const ImVec2& imgMax) {
    if (!m_flashStatusOverlay) return;
    if (m_statusMsgStartNS == 0 || m_statusMsg.empty()) return;
    ImFont* font = m_ui.GetSubtitleFont();
    if (!font) return;

    // 0.6s hold + 0.4s fade.
    uint64_t elapsed = SDL_GetTicksNS() - m_statusMsgStartNS;
    constexpr uint64_t kHoldNS = 600000000ULL;
    constexpr uint64_t kFadeNS = 400000000ULL;
    if (elapsed >= kHoldNS + kFadeNS) { m_statusMsgStartNS = 0; return; }
    float alpha = (elapsed <= kHoldNS) ? 1.0f
        : 1.0f - static_cast<float>(elapsed - kHoldNS) / static_cast<float>(kFadeNS);

    float videoH = imgMax.y - imgMin.y;
    float sz = std::clamp(videoH * 0.032f, 13.0f, 40.0f);
    const char* s = m_statusMsg.c_str();
    ImVec2 ts = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, s);
    float pad = sz * 0.45f;
    // Anchor in the top-right corner, clearing the menu bar (when shown) at the
    // top and leaving a comfortable margin on the right so it doesn't collide
    // with the menu bar's version text / window controls.
    float menuH = m_uiHidden ? 0.0f : ImGui::GetFrameHeight();
    float marginRight = sz * 0.9f;
    float marginTop = menuH + sz * 0.55f;
    ImVec2 textPos(imgMax.x - marginRight - ts.x, imgMin.y + marginTop);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(textPos.x - pad, textPos.y - pad * 0.6f),
                      ImVec2(textPos.x + ts.x + pad, textPos.y + ts.y + pad * 0.6f),
                      IM_COL32(0, 0, 0, static_cast<int>(170 * alpha)), pad * 0.5f);
    dl->AddText(font, sz, ImVec2(textPos.x + 1, textPos.y + 1),
                IM_COL32(0, 0, 0, static_cast<int>(210 * alpha)), s);
    dl->AddText(font, sz, textPos, IM_COL32(255, 255, 255, static_cast<int>(240 * alpha)), s);
}

void App::LoadRecentFiles() {
    m_recentFiles.clear();
    for (int i = 0; i < kRecentMax; i++) {
        std::string key = "recent_" + std::to_string(i);
        std::string p = m_prefSettings.GetString(key, "");
        if (!p.empty()) m_recentFiles.push_back(std::move(p));
    }
}

void App::SaveRecentFiles() {
    for (int i = 0; i < kRecentMax; i++) {
        std::string key = "recent_" + std::to_string(i);
        m_prefSettings.SetString(key,
            i < static_cast<int>(m_recentFiles.size()) ? m_recentFiles[i] : std::string());
    }
}

void App::AddToRecent(const std::string& path) {
    // Move-to-front: remove any existing entry for this path (so the list
    // stays unique), prepend the new path, then trim to kRecentMax.
    auto eq = [&](const std::string& s) { return s == path; };
    m_recentFiles.erase(std::remove_if(m_recentFiles.begin(), m_recentFiles.end(), eq),
                        m_recentFiles.end());
    m_recentFiles.insert(m_recentFiles.begin(), path);
    if (static_cast<int>(m_recentFiles.size()) > kRecentMax)
        m_recentFiles.resize(kRecentMax);
}

void App::ProcessEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_MOUSE_MOTION ||
            event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
            event.type == SDL_EVENT_MOUSE_WHEEL) {
            BumpUIActivity();
        }

        // Track maximize / restore so we can preserve the unmaximized geometry
        // across sessions even when closing the app while maximized.
        if (event.type == SDL_EVENT_WINDOW_MAXIMIZED) {
            m_maximized = true;
        } else if (event.type == SDL_EVENT_WINDOW_RESTORED) {
            m_maximized = false;
        }

        // Capture the unmaximized windowed geometry. Skip when fullscreen
        // (would store fullscreen bounds) or maximized (would store the
        // maximized work-area bounds and lose the restore-size for next launch).
        if (!m_fullscreen && !m_maximized &&
            (event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED)) {
            SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
            SDL_GetWindowSize(m_window, &m_windowedW, &m_windowedH);
        }

        // Rescale ImGui when the window moves to a display with different DPI.
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
            event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
            m_ui.SetUiScale(GetEffectiveUiScale());
        }

        // Follow the display's HDR capability as the window moves between
        // monitors or the OS toggles HDR.
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
            event.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED) {
            UpdateHDROutput();
        }

        if (event.type == SDL_EVENT_QUIT) {
            m_running = false;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            m_running = false;
        }
        if (event.type == SDL_EVENT_DROP_FILE) {
            // A dropped subtitle file loads over the open video; anything else
            // is treated as a media file to open. (If no video is open yet, a
            // subtitle has nothing to attach to, so fall through to open it as
            // media and let the demuxer reject it.)
            std::string dropped = event.drop.data;
            if (IsSubtitlePath(dropped) && m_player.HasMedia())
                OpenSubtitleFile(dropped);
            else
                RequestOpenFile(dropped);
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
                TogglePlayPauseWithFlash();
                break;
            case SDLK_LEFT:
                if (jump)           SeekToWithFlash(0.0, "Jump to start");
                else if (frameStep) StepFrameWithFlash(-1);
                else if (shift)     SeekRelativeWithFlash(-30.0);
                else if (seekFine)  SeekRelativeWithFlash(-1.0);
                else                SeekRelativeWithFlash(-5.0);
                break;
            case SDLK_RIGHT:
                if (jump)           SeekToWithFlash(m_player.GetDuration(), "Jump to end");
                else if (frameStep) StepFrameWithFlash(+1);
                else if (shift)     SeekRelativeWithFlash(30.0);
                else if (seekFine)  SeekRelativeWithFlash(1.0);
                else                SeekRelativeWithFlash(5.0);
                break;
            case SDLK_COMMA:
                if (!noMod) break;
                StepFrameWithFlash(-1);
                break;
            case SDLK_PERIOD:
                if (!noMod) break;
                StepFrameWithFlash(+1);
                break;
            case SDLK_HOME:
                if (!noMod) break;
                SeekToWithFlash(0.0, "Jump to start");
                break;
            case SDLK_END:
                if (!noMod) break;
                SeekToWithFlash(m_player.GetDuration(), "Jump to end");
                break;
            case SDLK_J: {
                if (!noMod) break;
                const auto& chs = m_player.GetChapters();
                if (chs.empty()) break;
                double now_t = m_player.GetPlaybackTime();
                // Nearest chapter whose start is strictly less than (now - eps),
                // so a second press steps back even when slightly past a boundary.
                const double eps = 0.25;
                int idx = -1;
                for (int i = 0; i < static_cast<int>(chs.size()); i++)
                    if (chs[i].startSec < now_t - eps && (idx < 0 || chs[i].startSec > chs[idx].startSec))
                        idx = i;
                if (idx < 0) SeekToWithFlash(0.0, "Jump to start"); // before first chapter
                else         SeekToWithFlash(chs[idx].startSec, ChapterLabel(idx));
                break;
            }
            case SDLK_K: {
                if (!noMod) break;
                const auto& chs = m_player.GetChapters();
                if (chs.empty()) break;
                double now_t = m_player.GetPlaybackTime();
                const double eps = 0.05;
                int idx = -1;
                for (int i = 0; i < static_cast<int>(chs.size()); i++)
                    if (chs[i].startSec > now_t + eps && (idx < 0 || chs[i].startSec < chs[idx].startSec))
                        idx = i;
                if (idx >= 0) SeekToWithFlash(chs[idx].startSec, ChapterLabel(idx));
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
                FlashSpeed();
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
                FlashSpeed();
                break;
            }
            case SDLK_I:
            case SDLK_LEFTBRACKET:
                if (!noMod) break;
                if (m_player.HasMedia()) {
                    int before = m_segments.GetCount();
                    // Use the seek target (what the playhead UI shows), not
                    // the decoded position — they diverge during slow seeks
                    // (e.g. GIF) and the user expects the mark at the visible
                    // playhead, not wherever decode happens to be.
                    m_segments.SetMarkIn(m_seekTarget);
                    if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                        m_showSegments = true;
                    FlashLabeledTime("Mark In", m_seekTarget);
                    BumpUIActivity();
                }
                break;
            case SDLK_O:
                if (cmd) {
                    static const SDL_DialogFileFilter videoFilters[] = {
                        {"Video files", "mp4;mkv;avi;mov;wmv;flv;webm;mpg;mpeg;3gp;ts;m4v;gif"},
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
                    double now_t = m_seekTarget;
                    bool acted = false;
                    if (m_segments.HasPendingMarkIn()) {
                        int before = m_segments.GetCount();
                        m_segments.SetMarkOut(now_t);
                        if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                            m_showSegments = true;
                        acted = true;
                    } else if (m_segments.GetCount() > 0) {
                        int last = m_segments.GetCount() - 1;
                        m_segments.UpdateSegment(last, m_segments.GetSegments()[last].startSec, now_t);
                        acted = true;
                    }
                    if (acted) FlashLabeledTime("Mark Out", now_t);
                    BumpUIActivity();
                }
                break;
            case SDLK_DELETE:
            case SDLK_BACKSPACE:
                if (!noMod) break;
                if (m_segments.GetTotalCount() > 0) {
                    m_segments.RemoveLastMark();
                    ShowStatus("Mark removed");
                    BumpUIActivity();
                }
                break;
            case SDLK_P:
                if (!noMod) break;
                if (m_player.HasMedia()) {
                    int before = m_segments.GetTotalCount();
                    m_segments.AddFrame(m_seekTarget);
                    if (m_segments.GetTotalCount() > before && !m_showSegments && !m_segmentsClosedManually)
                        m_showSegments = true;
                    FlashLabeledTime("Mark Frame", m_seekTarget);
                    BumpUIActivity();
                }
                break;
            case SDLK_E:
                // Defer: ImGui's IsPopupOpen needs a current window context
                // (set up during BeginFrame), but this handler runs before
                // BeginFrame. Resolve to open/close in Render.
                if (cmd) m_pendingExportToggle = true;
                break;
            case SDLK_T:
                if (winMod) {
                    m_showTimeline = !m_showTimeline;
                    BumpUIActivity();
                } else if (!cmd) {
                    // Plain T: cycle HDR->SDR tone-mapping operator (Shift = previous).
                    CycleTonemapper(shift ? -1 : +1);
                }
                break;
            case SDLK_M:
                if (winMod) {
                    m_showSegments = !m_showSegments;
                    if (!m_showSegments) m_segmentsClosedManually = true;
                    BumpUIActivity();
                } else if (noMod) {
                    m_player.SetMuted(!m_player.IsMuted());
                    ShowStatus(m_player.IsMuted() ? "Muted" : "Unmuted");
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
                FlashVolume();
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
                    BumpUIActivity();
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
                if (shift) {
                    m_showHelpPanel = !m_showHelpPanel;
                    BumpUIActivity();
                }
                break;
            case SDLK_A:
                // Cycle audio track (Shift = previous).
                if (cmd || winMod) break;
                CycleAudioTrack(shift ? -1 : +1);
                break;
            case SDLK_S:
                // Cycle subtitle track, including "off" (Shift = previous).
                if (cmd || winMod) break;
                CycleSubtitleTrack(shift ? -1 : +1);
                break;
            case SDLK_SEMICOLON:
                // Subtitle delay −50ms.
                if (cmd || winMod) break;
                NudgeSubtitleDelay(-0.05);
                FlashSubtitleDelay();
                break;
            case SDLK_APOSTROPHE:
                // Subtitle delay +50ms.
                if (cmd || winMod) break;
                NudgeSubtitleDelay(+0.05);
                FlashSubtitleDelay();
                break;
            }
        }
    }
}

void App::Render() {
    g_tooltipsEnabled = m_showTooltips;
    m_hoveredSegmentThisFrame = -1;
    m_hoveredFrameThisFrame = -1;

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
                // is the more scrubbing resolution you get. Height stays. This
                // is the sole owner of the Timeline's width (SetUiScale leaves
                // it alone), so DPI/UI-scale grows track it here too.
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

    // After fullscreen exit, wait for the viewport to settle, then restore
    // snapshot positions and suppress the proportional repositioning that
    // would have fired from the size change. The viewport may settle at one
    // of two sizes: m_windowedW/H if exiting to windowed, or the maximized
    // work area if we re-maximized on exit. SDL_EVENT_WINDOW_MAXIMIZED fires
    // before this check runs, so m_maximized is the cue for the latter case.
    if (m_waitingForFullscreenExit) {
        ImVec2 vpSize = ImGui::GetMainViewport()->Size;
        bool settled = false;
        if (m_maximized) {
            settled = true;
        } else {
            float expectedW = static_cast<float>(m_windowedW);
            float expectedH = static_cast<float>(m_windowedH);
            if (std::abs(vpSize.x - expectedW) < 2.0f && std::abs(vpSize.y - expectedH) < 2.0f) {
                settled = true;
            }
        }
        if (settled && vpSize.x > 0 && vpSize.y > 0) {
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
        BumpUIActivity();

    // Auto-hide UI after 5 seconds of no mouse activity (with 0.3s fade).
    // Skip entirely until a video is loaded — the placeholder is the only
    // thing on screen and hiding the menu/text just to fade to black is
    // confusing in the empty state.
    if (!m_player.HasMedia()) {
        m_uiHidden = false;
        m_uiAlpha = 1.0f;
    } else if (m_autoHideUI && m_lastUIActivityNS > 0) {
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

    // Auto-hide mouse cursor (same empty-state carve-out as UI auto-hide).
    // Route through ImGui::SetMouseCursor instead of SDL_HideCursor directly:
    // the ImGui SDL3 backend's NewFrame already calls SDL_ShowCursor every
    // frame, so a late SDL_HideCursor fights it and causes per-frame flicker.
    // Doing nothing in the "show" case lets ImGui's default Arrow cursor win.
    if (m_autoHideCursor && m_lastUIActivityNS > 0 && m_player.HasMedia()) {
        uint64_t elapsed = SDL_GetTicksNS() - m_lastUIActivityNS;
        if (elapsed > 5000000000ULL && !hoveringUI)
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
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
                    {"Video files", "mp4;mkv;avi;mov;wmv;flv;webm;mpg;mpeg;3gp;ts;m4v;gif"},
                    {"All files", "*"},
                };
                SDL_ShowOpenFileDialog([](void* userdata, const char* const* filelist, int) {
                    if (filelist && filelist[0])
                        static_cast<App*>(userdata)->RequestOpenFile(filelist[0]);
                }, this, m_window, videoFilters, 2, nullptr, false);
            }
            if (ImGui::BeginMenu("Open Recent", !m_recentFiles.empty())) {
                std::string toReopen;
                bool clearRequested = false;
                for (int i = 0; i < static_cast<int>(m_recentFiles.size()); i++) {
                    const std::string& full = m_recentFiles[i];
                    std::string label = std::filesystem::path(full).filename().string();
                    // Suffix index lets duplicate filenames have unique IDs.
                    std::string itemId = label + "##recent" + std::to_string(i);
                    std::error_code ec;
                    bool exists = std::filesystem::exists(full, ec) && !ec;
                    if (ImGui::MenuItem(itemId.c_str(), nullptr, false, exists)) toReopen = full;
                    // AllowWhenDisabled so the missing-file tooltip still fires.
                    if (g_tooltipsEnabled &&
                        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        if (exists)
                            ImGui::SetTooltip("%s", full.c_str());
                        else
                            ImGui::SetTooltip("%s\n(file no longer exists)", full.c_str());
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Recent")) clearRequested = true;
                ImGui::EndMenu();
                if (!toReopen.empty()) RequestOpenFile(toReopen);
                if (clearRequested) m_recentFiles.clear();
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
#if defined(__APPLE__)
            const char* revealLabel = "Show Current Video in Finder";
#elif defined(_WIN32)
            const char* revealLabel = "Show Current Video in Explorer";
#else
            const char* revealLabel = "Show Current Video in File Manager";
#endif
            if (ImGui::MenuItem(revealLabel, nullptr, false, !m_currentFilePath.empty())) {
                RevealInShell(m_currentFilePath);
            }
            if (ImGui::MenuItem("Open App Data Folder")) {
                OpenFolderInShell(GetAppDataDir());
            }
            if (ImGui::MenuItem("Open App Install Folder")) {
                if (const char* base = SDL_GetBasePath())
                    OpenFolderInShell(base);
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
            if (ImGui::BeginMenu("HDR")) {
                if (ImGui::MenuItem("HDR Output", nullptr, m_hdrOutputEnabled)) {
                    m_hdrOutputEnabled = !m_hdrOutputEnabled;
                    UpdateHDROutput();
                }
                TooltipFor("Output HDR when the open video is HDR and the window "
                           "is on an HDR-enabled display.");
                ImGui::Separator();
                // Hints mark the operator that T / Shift+T would switch to (next/prev).
                int cur = static_cast<int>(m_tonemapper);
                int nextTm = (cur + 1) % kTonemapperCount;
                int prevTm = (cur - 1 + kTonemapperCount) % kTonemapperCount;
                auto tmHint = [&](int idx) -> const char* {
                    if (nextTm == idx && prevTm == idx) return "T / Shift+T";
                    if (nextTm == idx) return "T";
                    if (prevTm == idx) return "Shift+T";
                    return nullptr;
                };
                ImGui::BeginDisabled(m_swapchainHDR);
                for (int i = 0; i < kTonemapperCount; ++i) {
                    Tonemapper t = static_cast<Tonemapper>(i);
                    if (ImGui::MenuItem(TonemapperName(t), tmHint(i), m_tonemapper == t))
                        m_tonemapper = t;
                }
                ImGui::EndDisabled();
                // Status footer: what the current content/output state means
                // for the settings above.
                const bool videoIsHDR =
                    m_player.HasMedia() && m_videoColorMode != VideoColorMode::SDR;
                const char* statusLine1;
                const char* statusLine2;
                if (!videoIsHDR) {
                    statusLine1 = "SDR video";
                    statusLine2 = "HDR settings don't apply";
                } else if (!m_swapchainHDR) {
                    statusLine1 = m_displayHDR ? "HDR video, HDR output disabled"
                                               : "HDR video, SDR display";
                    statusLine2 = "Selected tonemapper is used";
                } else {
                    statusLine1 = (m_hdrCompositeMode == 1) ? "HDR output enabled (scRGB)"
                                                            : "HDR output enabled (HDR10)";
                    statusLine2 = "Tonemapper options don't apply";
                }
                ImGui::Separator();
                ImGui::TextDisabled("%s", statusLine1);
                ImGui::TextDisabled("%s", statusLine2);
                ImGui::EndMenu();
            }
            ImGui::SeparatorText("Windows");
            if (ImGui::MenuItem("Timeline", (std::string(kKeys.winModName) + "+T").c_str(), m_showTimeline))
                m_showTimeline = !m_showTimeline;
            if (ImGui::MenuItem("Marks", (std::string(kKeys.winModName) + "+M").c_str(), m_showSegments)) {
                m_showSegments = !m_showSegments;
                if (!m_showSegments) m_segmentsClosedManually = true;
            }
            if (ImGui::MenuItem("Help", (std::string(kKeys.winModName) + "+H or ?").c_str(), m_showHelpPanel))
                m_showHelpPanel = !m_showHelpPanel;
            ImGui::SeparatorText("Settings");
            if (ImGui::MenuItem("Show Chapters", nullptr, m_showChapters))
                m_showChapters = !m_showChapters;
            TooltipFor("Show chapter markers on the timeline.");
            if (ImGui::MenuItem("Show Waveform", nullptr, m_showWaveform)) {
                m_showWaveform = !m_showWaveform;
                // Lazily kick off a scan when enabled and there's nothing
                // cached for the currently-open file yet. Disabling leaves
                // any in-flight scan running so re-enabling is instant.
                if (m_showWaveform &&
                    m_player.HasMedia() && m_player.HasAudio() &&
                    m_waveform.GetFilledBuckets() == 0 &&
                    !m_waveform.IsRunning()) {
                    m_waveform.Start(m_currentFilePath, m_player.GetDuration());
                }
            }
            TooltipFor("Overlay the audio waveform on the timeline.");
            if (ImGui::MenuItem("Tooltips", nullptr, m_showTooltips))
                m_showTooltips = !m_showTooltips;
            TooltipFor("Show these hover tooltips throughout the UI.");
            if (ImGui::MenuItem("Flash Status Overlay", nullptr, m_flashStatusOverlay))
                m_flashStatusOverlay = !m_flashStatusOverlay;
            TooltipFor("Brief on-screen feedback for keyboard actions (seeks, play/pause, volume, etc.).");
#ifndef __APPLE__
            // Show the display's scale in the hotkey-hint slot, e.g. "125%",
            // so it's visible what the toggle would apply even while disabled.
            char dpiHint[16];
            snprintf(dpiHint, sizeof(dpiHint), "%d%%",
                     static_cast<int>(std::round(SDL_GetWindowDisplayScale(m_window) * 100.0f)));
            if (ImGui::MenuItem("Use DPI scaling", dpiHint, m_useDpiScaling)) {
                m_useDpiScaling = !m_useDpiScaling;
                float scale = GetEffectiveUiScale();
                m_ui.SetUiScale(scale);
                // Grow the window by the UI scale if it's smaller than the
                // now-scaled default size, so the scaled-up UI doesn't get
                // cramped in a window sized for 1.0x.
                if (m_useDpiScaling && scale > 1.0f) {
                    int ww = 0, wh = 0;
                    SDL_GetWindowSize(m_window, &ww, &wh);
                    if (ww < static_cast<int>(1280 * scale) ||
                        wh < static_cast<int>(720 * scale)) {
                        GrowWindowForUiScale(scale);
                    }
                }
            }
#endif
            // Explicit UI scale, applied as a multiplier on top of the automatic
            // DPI scale (see GetEffectiveUiScale). Shown on all platforms; on
            // macOS it's the only UI-scaling control since DPI is OS-handled.
            // A stepped +/- input rather than a slider, so it doesn't
            // continuously resize the very menu being interacted with. Laid out
            // label-left / input-right to match the other rows.
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("UI Scale");
                ImGui::SameLine();
                const ImGuiStyle& st = ImGui::GetStyle();
                float btn = ImGui::GetFrameHeight();
                float fieldW = ImGui::CalcTextSize("0.00x").x + st.FramePadding.x * 2.0f;
                float inputW = fieldW + 2.0f * (btn + st.ItemInnerSpacing.x);
                float resetW = ImGui::CalcTextSize("1").x + st.FramePadding.x * 2.0f;
                float rowW = inputW + st.ItemInnerSpacing.x + resetW;
                float avail = ImGui::GetContentRegionAvail().x;
                if (avail > rowW)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - rowW));
                ImGui::SetNextItemWidth(inputW);
                float prevUserUiScale = m_userUiScale;
                if (ImGui::InputFloat("##uiscale", &m_userUiScale, 0.25f, 0.25f, "%.2fx")) {
                    m_userUiScale = std::clamp(m_userUiScale, 0.5f, 2.0f);
                    if (m_userUiScale != prevUserUiScale) {
                        float scale = GetEffectiveUiScale();
                        m_ui.SetUiScale(scale);
                        // Grow the window to match, just like enabling DPI
                        // scaling does — but only on an increase and only while
                        // it's smaller than the scaled default.
                        if (m_userUiScale > prevUserUiScale && scale > 1.0f) {
                            int ww = 0, wh = 0;
                            SDL_GetWindowSize(m_window, &ww, &wh);
                            if (ww < static_cast<int>(1280 * scale) ||
                                wh < static_cast<int>(720 * scale)) {
                                GrowWindowForUiScale(m_userUiScale / prevUserUiScale);
                            }
                        }
                    }
                }
                ImGui::SameLine(0, st.ItemInnerSpacing.x);
                if (ImGui::Button("1##uiscale") && m_userUiScale != 1.0f) {
                    m_userUiScale = 1.0f;            // reset (a decrease — no window grow)
                    m_ui.SetUiScale(GetEffectiveUiScale());
                }
            }
            SubtitleSizeControl("view");
            if (ImGui::MenuItem("Auto-hide Mouse Cursor", nullptr, m_autoHideCursor))
                m_autoHideCursor = !m_autoHideCursor;
            if (ImGui::MenuItem("Auto-hide UI", nullptr, m_autoHideUI))
                m_autoHideUI = !m_autoHideUI;
            if (ImGui::MenuItem("Show/Hide UI", "H", false, !m_autoHideUI)) {
                m_uiHidden = !m_uiHidden;
                m_uiAlpha = m_uiHidden ? 0.0f : 1.0f;
            }
            ImGui::SeparatorText("Reset");
            if (ImGui::MenuItem("Reset Layout")) {
                // Clear "re-maximize on fullscreen exit" so the upcoming
                // SetFullscreen(false) doesn't undo our un-maximize below.
                m_wasMaximizedBeforeFullscreen = false;
                SetFullscreen(false);
                if (m_maximized) {
                    SDL_RestoreWindow(m_window);
                }
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
                float scale = m_ui.GetUiScale();
                SDL_SetWindowSize(m_window, static_cast<int>(1280 * scale), static_cast<int>(720 * scale));
                SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                std::filesystem::remove(GetAppDataDir() / "layout.ini");
            }
            TooltipFor("Reset the window size and panel layout to defaults.");
            if (ImGui::MenuItem("Reset Settings"))
                ResetSettings();
            TooltipFor("Restore the View settings to defaults.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Media", m_player.HasMedia())) {
            const auto& atracks = m_player.GetAudioTracks();
            if (ImGui::BeginMenu("Audio Track", !atracks.empty())) {
                int n = static_cast<int>(atracks.size());
                int active = m_player.GetActiveAudioStreamIndex();
                int cur = 0;
                for (int i = 0; i < n; i++)
                    if (atracks[i].streamIndex == active) { cur = i; break; }
                // Hints mark the tracks A / Shift+A would switch to (next / prev).
                int nextIdx = (n > 1) ? (cur + 1) % n : -1;
                int prevIdx = (n > 1) ? (cur - 1 + n) % n : -1;
                for (int i = 0; i < n; i++) {
                    std::string label = std::to_string(i + 1) + ": " + atracks[i].title +
                                        "##atrk" + std::to_string(i);
                    const char* sc = (i == nextIdx && i == prevIdx) ? "A / Shift+A"
                                   : (i == nextIdx) ? "A"
                                   : (i == prevIdx) ? "Shift+A" : nullptr;
                    bool sel = atracks[i].streamIndex == active;
                    if (ImGui::MenuItem(label.c_str(), sc, sel))
                        SelectAudioTrack(atracks[i].streamIndex);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Subtitles")) {
                // Hints mark the track (or Off) that S / Shift+S would switch to
                // (next / prev); the cycle is Off → track 0 → … → last → Off.
                int sn = static_cast<int>(m_subtitleTracks.size());
                int states = sn + 1;
                int curState = m_activeSubtitle + 1; // 0 == Off
                int nextSub = (curState + 1) % states - 1;          // -1 == Off
                int prevSub = (curState - 1 + states) % states - 1; // -1 == Off
                auto subHint = [&](int idx) -> const char* {
                    if (nextSub == idx && prevSub == idx) return "S / Shift+S";
                    if (nextSub == idx) return "S";
                    if (prevSub == idx) return "Shift+S";
                    return nullptr;
                };
                if (ImGui::MenuItem("Off", subHint(-1), m_activeSubtitle < 0))
                    SelectSubtitleTrack(-1);
                for (int i = 0; i < sn; i++) {
                    const SubtitleTrackInfo& s = m_subtitleTracks[i];
                    std::string label = s.title;
                    if (s.external) label += " (file)";
                    label += "##strk" + std::to_string(i);
                    if (ImGui::MenuItem(label.c_str(), subHint(i), m_activeSubtitle == i))
                        SelectSubtitleTrack(i);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open File...")) {
                    static const SDL_DialogFileFilter subFilters[] = {
                        {"Subtitle files", "srt;ass;ssa;vtt;sub"},
                        {"All files", "*"},
                    };
                    SDL_ShowOpenFileDialog([](void* userdata, const char* const* filelist, int) {
                        if (filelist && filelist[0])
                            static_cast<App*>(userdata)->m_pendingSubtitlePath = filelist[0];
                    }, this, m_window, subFilters, 2, nullptr, false);
                }
                ImGui::EndMenu();
            }
            SubtitleSizeControl("media");
            // Subtitle delay: stepped +/- input (milliseconds) with a Reset
            // button, same widget style as Subtitle Size above.
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Subtitle Delay");
                TooltipFor("Subtitle delay (negative = earlier)", "; / '");
                ImGui::SameLine();
                const ImGuiStyle& st = ImGui::GetStyle();
                float btn = ImGui::GetFrameHeight();
                float fieldW = ImGui::CalcTextSize("-9999 ms").x + st.FramePadding.x * 2.0f;
                float inputW = fieldW + 2.0f * (btn + st.ItemInnerSpacing.x);
                float resetW = ImGui::CalcTextSize("0").x + st.FramePadding.x * 2.0f;
                float rowW = inputW + st.ItemInnerSpacing.x + resetW;
                float avail = ImGui::GetContentRegionAvail().x;
                if (avail > rowW)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - rowW));
                ImGui::SetNextItemWidth(inputW);
                float ms = static_cast<float>(std::lround(m_subtitleDelaySec * 1000.0));
                if (ImGui::InputFloat("##subdelay", &ms, 50.0f, 50.0f, "%.0f ms"))
                    m_subtitleDelaySec = ms / 1000.0;
                ImGui::SameLine(0, st.ItemInnerSpacing.x);
                if (ImGui::Button("0##subdelay")) m_subtitleDelaySec = 0.0;
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
    // Gate on m_displayTexture, not m_videoTexture: a freshly created texture
    // has undefined contents (pink garbage on Metal) until the first decoded
    // frame is uploaded, and m_displayTexture is only set after that upload.
    if (m_displayTexture && m_player.HasMedia()) {
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
        ImGui::Image(reinterpret_cast<ImTextureID>(m_displayTexture), ImVec2(displayW, displayH));
        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImVec2 imgMax = ImGui::GetItemRectMax();
        // Click the video to toggle play/pause; double-click to toggle
        // fullscreen. ImGui fires both single and double click on the second
        // click of a double-click, so the play/pause toggles cancel out and
        // the user sees only a fullscreen change (matches VLC / MPC-HC).
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                TogglePlayPauseWithFlash();
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                SetFullscreen(!m_fullscreen);
        }

        // Briefly flash a Play/Pause icon at the center of the video after a
        // toggle — visual confirmation, the kind of thing every mainstream
        // player does. Shown even when the UI is hidden, since it's the only
        // feedback for the toggle in immersive mode. Gated by the same
        // "Flash Status Overlay" setting as the top-right status flash.
        // ~100ms hold + ~400ms fade.
        if (m_playPauseFlashStartNS > 0 && m_flashStatusOverlay) {
            uint64_t elapsed = SDL_GetTicksNS() - m_playPauseFlashStartNS;
            constexpr uint64_t kHoldNS = 100000000ULL;
            constexpr uint64_t kFadeNS = 400000000ULL;
            if (elapsed >= kHoldNS + kFadeNS) {
                m_playPauseFlashStartNS = 0;
            } else {
                float alpha = (elapsed <= kHoldNS) ? 1.0f
                    : 1.0f - static_cast<float>(elapsed - kHoldNS) / static_cast<float>(kFadeNS);
                ImVec2 c((imgMin.x + imgMax.x) * 0.5f, (imgMin.y + imgMax.y) * 0.5f);
                float uiScale = m_ui.GetUiScale();
                float r = 48.0f * uiScale;
                float outline = 4.0f * uiScale;
                ImU32 bgCol = IM_COL32(0, 0, 0, static_cast<int>(200 * alpha));
                ImU32 fgCol = IM_COL32(255, 255, 255, static_cast<int>(240 * alpha));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (m_playPauseFlashIsPlaying) {
                    // Just started playing → play triangle. Centered on
                    // its geometric centroid (mean of the 3 vertices),
                    // which sits at half the bbox width from the apex.
                    float tri = r * 0.6f;
                    float w   = tri * 1.5f;          // base-to-apex distance
                    float leftX  = c.x - w / 3.0f;   // 2 verts here
                    float rightX = c.x + 2.0f * w / 3.0f; // apex
                    ImVec2 p1(leftX,  c.y - tri);
                    ImVec2 p2(leftX,  c.y + tri);
                    ImVec2 p3(rightX, c.y);
                    dl->AddTriangleFilled(p1, p2, p3, fgCol);
                    dl->AddTriangle(p1, p2, p3, bgCol, outline);
                } else {
                    // Just paused → two bars.
                    float barW = r * 0.22f;
                    float barH = r * 1.0f;
                    float gap  = r * 0.18f;
                    ImVec2 l1(c.x - gap - barW, c.y - barH * 0.5f);
                    ImVec2 l2(c.x - gap,        c.y + barH * 0.5f);
                    ImVec2 r1(c.x + gap,        c.y - barH * 0.5f);
                    ImVec2 r2(c.x + gap + barW, c.y + barH * 0.5f);
                    dl->AddRectFilled(l1, l2, fgCol);
                    dl->AddRect(l1, l2, bgCol, 0.0f, 0, outline);
                    dl->AddRectFilled(r1, r2, fgCol);
                    dl->AddRect(r1, r2, bgCol, 0.0f, 0, outline);
                }
            }
        }

        // Subtitle overlay sits on top of the video (and above the flash), and
        // shows even when the UI chrome is hidden — subtitles are content, not UI.
        RenderSubtitleOverlay(imgMin, imgMax);
        // Top-right flash for track/delay/size changes — explicit user feedback.
        RenderStatusOverlay(imgMin, imgMax);
    } else if (!m_player.HasMedia()) {
        // (When media is open but the first frame hasn't arrived yet, draw
        // nothing rather than flashing this hint for a frame or two.)
        std::string line1 = "Drag and drop a video file to open it";
        std::string line2 = "or press " + std::string(kKeys.cmdName) + "+O to browse";
        std::string line3 = "Press ? or " + std::string(kKeys.winModName) + "+H for keyboard shortcuts";
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 size1 = ImGui::CalcTextSize(line1.c_str());
        ImVec2 size2 = ImGui::CalcTextSize(line2.c_str());
        ImVec2 size3 = ImGui::CalcTextSize(line3.c_str());
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float blockH = lineH * 3.0f;
        float startY = (avail.y - blockH) * 0.5f;
        auto centerLine = [&](const std::string& s, const ImVec2& sz, int row, bool disabled) {
            ImGui::SetCursorPos(ImVec2((avail.x - sz.x) * 0.5f, startY + lineH * row));
            if (disabled) ImGui::TextDisabled("%s", s.c_str());
            else          ImGui::TextUnformatted(s.c_str());
        };
        centerLine(line1, size1, 0, false);
        centerLine(line2, size2, 1, false);
        centerLine(line3, size3, 2, true);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // Timeline panel (unified controls + timeline bar)
    if (m_showTimeline && !m_uiHidden) {
    ImGui::SetNextWindowBgAlpha(0.85f * m_uiAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_uiAlpha);
    float uiScale = m_ui.GetUiScale();
    float tlPad = 40.0f * uiScale;
    float tlWidth = std::min(vp->WorkSize.x - tlPad * 2, 1100.0f * uiScale);
    float tlHeight = 110.0f * uiScale;
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
            TogglePlayPauseWithFlash();
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
        float markGroupGap = 12.0f * uiScale;  // extra gap between segment buttons and the frame button
        {
            float afterTransport = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x;
            float muteW_ = std::max(ImGui::CalcTextSize("Mute").x, ImGui::CalcTextSize("Unmute").x) + ImGui::GetStyle().FramePadding.x * 2;
            float audioStart = panelWidth - muteW_ - ImGui::GetStyle().ItemSpacing.x - 80.0f * uiScale;
            float bracketW = ImGui::CalcTextSize("[").x + ImGui::GetStyle().FramePadding.x * 2;
            float frameBtnW = ImGui::CalcTextSize("[]").x + ImGui::GetStyle().FramePadding.x * 2;
            float segBtnsW = bracketW * 2 + ImGui::GetStyle().ItemSpacing.x + markGroupGap + frameBtnW;
            float segStart = afterTransport + (audioStart - afterTransport - segBtnsW) * 0.5f;
            ImGui::SameLine(segStart);
        }
        if (ImGui::Button("[")) {
            int before = m_segments.GetCount();
            m_segments.SetMarkIn(m_seekTarget);
            if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                m_showSegments = true;
            BumpUIActivity();
        }
        TooltipFor("Mark segment start for export", "I  or  [");
        ImGui::SameLine();
        bool canMarkOut = m_segments.HasPendingMarkIn() || m_segments.GetCount() > 0;
        if (!canMarkOut) ImGui::BeginDisabled();
        if (ImGui::Button("]")) {
            double now_t = m_seekTarget;
            if (m_segments.HasPendingMarkIn()) {
                int before = m_segments.GetCount();
                m_segments.SetMarkOut(now_t);
                if (m_segments.GetCount() > before && !m_showSegments && !m_segmentsClosedManually)
                    m_showSegments = true;
            } else if (m_segments.GetCount() > 0) {
                int last = m_segments.GetCount() - 1;
                m_segments.UpdateSegment(last, m_segments.GetSegments()[last].startSec, now_t);
            }
            BumpUIActivity();
        }
        if (!canMarkOut) ImGui::EndDisabled();
        TooltipFor("Mark segment end for export", "O  or  ]");
        ImGui::SameLine(0.0f, markGroupGap);
        if (ImGui::Button("[]")) {
            if (m_player.HasMedia()) {
                int before = m_segments.GetTotalCount();
                m_segments.AddFrame(m_seekTarget);
                if (m_segments.GetTotalCount() > before && !m_showSegments && !m_segmentsClosedManually)
                    m_showSegments = true;
                BumpUIActivity();
            }
        }
        TooltipFor("Mark frame for PNG export", "P");

        // Right: Mute + volume slider
        {
            bool muted = m_player.IsMuted();
            bool noAudio = hasMedia && !m_player.HasAudio();
            float muteW = std::max(ImGui::CalcTextSize("Mute").x, ImGui::CalcTextSize("Unmute").x) + ImGui::GetStyle().FramePadding.x * 2;
            float sliderW = 80.0f * uiScale;
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
        float barHeight = 32.0f * uiScale;
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
            const float kChapGap = 2.0f * uiScale;
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
                const float labelPadX = 6.0f * uiScale;
                const float clipPadX = 2.0f * uiScale;
                ImVec2 textSize = ImGui::CalcTextSize(label);
                float textY = barPos.y + (barHeight - textSize.y) * 0.5f;
                int textAlpha = static_cast<int>((hovered ? 230 : 160) * m_uiAlpha);
                ImU32 textCol = IM_COL32(255, 255, 255, textAlpha);
                float availTextW = (x1 - clipPadX) - (x0 + labelPadX);
                if (hovered && textSize.x > availTextW)
                    hoveredChapterOverflows = true;
                drawList->PushClipRect(ImVec2(x0 + clipPadX, barPos.y),
                                       ImVec2(x1 - clipPadX, barPos.y + barHeight), true);
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

        // Waveform overlay — drawn between the bar background and segments
        // so user marks always stay visible on top. Reads peaks atomically
        // up to GetFilledBuckets(); the worker thread fills them in order
        // so visible buckets are stable.
        if (m_showWaveform && duration > 0.0) {
            int filled = m_waveform.GetFilledBuckets();
            if (filled > 0) {
                // Normalize against the loudest bucket processed so far so
                // the peak always maxes out the bar height. As more audio is
                // scanned and a louder bucket arrives, the whole visible
                // waveform rescales — a slightly shifting envelope while
                // filling in, then stable once the scan completes.
                float maxPeak = 0.0f;
                for (int b = 0; b < filled; b++) {
                    float p = m_waveform.GetPeak(b);
                    if (p > maxPeak) maxPeak = p;
                }
                if (maxPeak > 1e-6f) {
                    // Filled buckets cover [0, filled/kBuckets * duration].
                    // Clip the drawn x-range to that span so the waveform
                    // grows left-to-right as decoding progresses.
                    int barW = static_cast<int>(std::round(barWidth));
                    int totalBuckets = WaveformExtractor::kBuckets;
                    float fillFrac = static_cast<float>(filled) / static_cast<float>(totalBuckets);
                    int xMax = static_cast<int>(std::round(barWidth * fillFrac));
                    float cy = barPos.y + barHeight * 0.5f;
                    float halfBar = barHeight * 0.45f;
                    float invMax = 1.0f / maxPeak;
                    ImU32 wfCol = fadeCol(180, 200, 230, 110);
                    for (int xi = 0; xi < xMax && xi < barW; xi++) {
                        int b0 = (xi * totalBuckets) / barW;
                        int b1 = ((xi + 1) * totalBuckets) / barW;
                        if (b1 <= b0) b1 = b0 + 1;
                        if (b1 > filled) b1 = filled;
                        if (b0 >= b1) break;
                        float peak = 0.0f;
                        for (int b = b0; b < b1; b++) {
                            float p = m_waveform.GetPeak(b);
                            if (p > peak) peak = p;
                        }
                        if (peak <= 0.0f) continue;
                        float h = peak * invMax * halfBar;
                        // Min visible height so very quiet sections still read.
                        if (h < 0.5f) h = 0.5f;
                        float x = barPos.x + static_cast<float>(xi);
                        drawList->AddRectFilled(ImVec2(x, cy - h), ImVec2(x + 1.0f, cy + h), wfCol);
                    }
                }
            }
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
                              0.0f, 0, (highlighted ? 3.0f : 1.0f) * uiScale);

            // Diagonal line pattern on highlighted segments
            if (highlighted) {
                ImU32 lineCol = GetSegmentColor(segs[i].colorIndex, 0.6f * m_uiAlpha);
                float spacing = 8.0f * uiScale;
                drawList->PushClipRect(ImVec2(x0, barPos.y), ImVec2(x1, barPos.y + barHeight), true);
                for (float offset = -barHeight; offset < (x1 - x0) + barHeight; offset += spacing) {
                    drawList->AddLine(
                        ImVec2(x0 + offset, barPos.y + barHeight),
                        ImVec2(x0 + offset + barHeight, barPos.y),
                        lineCol, 2.0f * uiScale);
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
            int triHalf = static_cast<int>(std::round(4.0f * uiScale));
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
            for (float y = barPos.y; y < barPos.y + barHeight; y += 6.0f * uiScale) {
                float yEnd = y + 3.0f * uiScale;
                if (yEnd > barPos.y + barHeight) yEnd = barPos.y + barHeight;
                drawList->AddLine(ImVec2(mx, y), ImVec2(mx, yEnd),
                                  fadeCol(255, 200, 50, 180), 2.0f * uiScale);
            }
        }

        // Playhead (follows mouse immediately during any seek)
        if (duration > 0.0) {
            float px = barPos.x + static_cast<float>(currentTime / duration) * barWidth;
            drawList->AddLine(ImVec2(px, barPos.y), ImVec2(px, barPos.y + barHeight),
                              fadeCol(255, 255, 255, 220), 2.0f * uiScale);
        }

        // --- Interaction: segment edge handles first, then bar click-to-seek ---
        // Handles are rendered first so they take priority over the bar click
        bool handleActive = false;
        for (int i = 0; i < static_cast<int>(segs.size()); i++) {
            float x0 = barPos.x + static_cast<float>(segs[i].startSec / duration) * barWidth;
            float x1 = barPos.x + static_cast<float>(segs[i].endSec / duration) * barWidth;
            float handleW = 8.0f * uiScale;

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
                        if (m_segments.GetTotalCount() > before && !m_showSegments && !m_segmentsClosedManually) {
                            m_showSegments = true;
                            m_segmentsNoFocusOnOpen = true;
                        }
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

        const std::string& colorLabel = m_player.GetColorSpaceLabel();
        char infoRight[160];
        snprintf(infoRight, sizeof(infoRight), "%dx%d  |  %.1f fps  |  %s  |  %s  |  %s  |  %s",
                 m_videoWidth, m_videoHeight, m_player.GetFrameRate(),
                 m_player.GetVideoCodecName(),
                 colorLabel.empty() ? "?" : colorLabel.c_str(), brateBuf, sizeBuf);
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
    float uiScale = m_ui.GetUiScale();
    float marksW = 450.0f * uiScale;
    float marksH = 250.0f * uiScale;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - marksW - 40 * uiScale, vp->WorkPos.y + 40 * uiScale), layoutCond);
    ImGui::SetNextWindowSize(ImVec2(marksW, marksH), layoutCond);
    bool segmentsWasOpen = m_showSegments;
    ImGui::Begin("Marks", &m_showSegments,
                 m_segmentsNoFocusOnOpen ? ImGuiWindowFlags_NoFocusOnAppearing : 0);
    m_segmentsNoFocusOnOpen = false;

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
                // a source-format / "GIF" SmallButton).
                std::string srcLabel = SourceFormatLabel();
                float framePadX = ImGui::GetStyle().FramePadding.x * 2;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float xBtnW = ImGui::CalcTextSize("X").x + framePadX;
                float srcW = ImGui::CalcTextSize(srcLabel.c_str()).x + framePadX;
                float gifW = ImGui::CalcTextSize("GIF").x + framePadX;
                float pngW = ImGui::CalcTextSize("PNG").x;
                float maxFmtW = std::max(std::max(srcW, gifW), pngW);

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
                bool gifSrc = IsGifSource();
                float fmtActualW = row.isFrame ? pngW
                    : ((!gifSrc && segs[row.index].mode == ExportMode::GIF) ? gifW : srcW);
                ImGui::SameLine();
                ImGui::SetCursorPosX(fmtRightX - fmtActualW);
                if (row.isFrame) {
                    ImGui::TextDisabled("PNG");
                } else if (gifSrc) {
                    // GIF source: both modes produce identical GIF output, so
                    // the toggle is meaningless. Show a disabled SmallButton.
                    ImGui::BeginDisabled();
                    ImGui::SmallButton("GIF##fmt");
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted("Only GIF export format is supported for GIF source");
                        ImGui::EndTooltip();
                    }
                } else {
                    const TimeRange& s = segs[row.index];
                    std::string fmtLabel = (s.mode == ExportMode::GIF) ? "GIF##fmt" : (srcLabel + "##fmt");
                    if (ImGui::SmallButton(fmtLabel.c_str())) {
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
                    double effSpeed = (s.speed > 0.0) ? s.speed : 1.0;
                    double effDur = dur / effSpeed;
                    float effDurF = static_cast<float>(effDur);
                    ImGui::SameLine();
                    // Match the surrounding SmallButton row height by flattening
                    // the input's vertical frame padding (same trick as
                    // FixedWidthButton uses for its small variant).
                    ImGuiStyle& smallStyle = ImGui::GetStyle();
                    float backupPadY = smallStyle.FramePadding.y;
                    smallStyle.FramePadding.y = 0;
                    ImGui::SetNextItemWidth(56.0f * uiScale);
                    bool durChanged = ImGui::InputFloat("##segdur", &effDurF, 0.0f, 0.0f, "%.1fs");
                    smallStyle.FramePadding.y = backupPadY;
                    if (durChanged) {
                        if (effDurF < 0.05f) effDurF = 0.05f;
                        double newEnd = s.startSec + effDurF * effSpeed;
                        double maxEnd = m_player.GetDuration();
                        if (newEnd > maxEnd) newEnd = maxEnd;
                        m_segments.UpdateSegment(row.index, s.startSec, newEnd);
                    }
                    TooltipFor("Segment duration (after speed)");

                    // Audio toggle at the end of the row. Disabled with an
                    // explanatory tooltip when format/speed/source forces audio off.
                    ImGui::SameLine();
                    bool newKeep;
                    bool srcNoAudio = !m_player.HasAudio();
                    if (KeepAudioToggle("##segaudio", s, srcNoAudio, newKeep, /*small*/ true)) {
                        m_segments.SetSegmentKeepAudio(row.index, newKeep);
                    }
                    if (!srcNoAudio && !AudioForciblyDropped(s)) TooltipFor("Toggle audio in export");
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
        // shortcuts' content size — that's the single source of truth, not a
        // hardcoded number duplicated in the reset path.
        float uiScale = m_ui.GetUiScale();
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + 40 * uiScale, vp->WorkPos.y + 40 * uiScale),
            layoutCond);
        ImGuiWindowFlags helpFlags =
            wasResetFrame ? ImGuiWindowFlags_AlwaysAutoResize : 0;
        ImGui::Begin("Help", &m_showHelpPanel, helpFlags);
        using ScRow = std::pair<const char*, std::string>;
        const std::vector<std::pair<const char*, std::vector<ScRow>>> scCategories = {
            {"General", {
                {"Open video",      std::string(kKeys.cmdName) + " + O"},
                {"Fullscreen",      "F"},
                {"Show / hide UI",  "H"},
                {"Toggle timeline", std::string(kKeys.winModName) + " + T"},
                {"Toggle marks",    std::string(kKeys.winModName) + " + M"},
                {"Toggle help",     std::string(kKeys.winModName) + " + H  or  ?"},
                {"Quit",            kKeys.quitShortcut},
            }},
            {"Scrub", {
                {"Play / Pause",        "Space"},
                {"Seek +/- 1s",         std::string(kKeys.seekFineName) + " + Left / Right"},
                {"Seek +/- 5s",         "Left / Right"},
                {"Seek +/- 30s",        "Shift + Left / Right"},
                {"Frame step",          std::string(kKeys.frameStepName) + " + Left / Right  or  , / ."},
                {"Jump to start / end", kKeys.jumpName},
                {"Prev / next chapter", "J / K"},
                {"Speed up / down",     "+ / -"},
                {"Precision scrub",     std::string(kKeys.altKeyName) + " + drag timeline"},
            }},
            {"Cut", {
                {"Mark In",              "I  or  ["},
                {"Mark Out",             "O  or  ]"},
                {"Mark Frame",           "P"},
                {"Remove last mark",     kKeys.deleteName},
                {"Add mark on timeline", std::string(kKeys.cmdName) + " + click (frame) / drag (segment)"},
                {"Export",               std::string(kKeys.cmdName) + " + E"},
            }},
            {"Media", {
                {"Volume up / down",           "Up / Down"},
                {"Mute / unmute",              "M"},
                {"Next / prev audio track",    "A / Shift + A"},
                {"Next / prev subtitle track", "S / Shift + S"},
                {"Subtitle delay -/+",         "; / '"},
            }},
        };

        // Size the action column to the widest action label (+ a margin) across
        // all categories, so it's snug rather than 50/50 and stays aligned
        // between the per-category tables. The hotkey column takes the rest.
        const ImGuiStyle& scStyle = ImGui::GetStyle();
        float actionColW = 0.0f;
        for (const auto& cat : scCategories)
            for (const auto& r : cat.second)
                actionColW = std::max(actionColW, ImGui::CalcTextSize(r.first).x);
        actionColW += scStyle.CellPadding.x * 2.0f + scStyle.ItemSpacing.x * 2.0f;

        constexpr ImGuiTableFlags kScFlags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
        for (const auto& cat : scCategories) {
            if (!ImGui::CollapsingHeader(cat.first, ImGuiTreeNodeFlags_DefaultOpen))
                continue;
            std::string tableId = std::string("##sc_") + cat.first;
            if (ImGui::BeginTable(tableId.c_str(), 2, kScFlags)) {
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, actionColW);
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
                for (const auto& r : cat.second) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.first);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.second.c_str());
                }
                ImGui::EndTable();
            }
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
    // Resolve a deferred Cmd+E from the global keyboard handler. We only
    // handle "open" here; closing is done by the in-modal Cmd+E handler,
    // which is the only path that actually fires while the modal is open
    // (the global handler is gated by WantCaptureKeyboard at that point).
    if (m_pendingExportToggle) {
        m_pendingExportToggle = false;
        if (!m_exportDialogOpen &&
            m_segments.GetTotalCount() > 0 && !m_exporter.IsRunning()) {
            m_showExportDialog = true;
            auto stem = std::filesystem::path(m_currentFilePath).stem().string();
            InitExportDir();
            snprintf(m_exportName, sizeof(m_exportName), "%s", stem.c_str());
            m_pendingExport = ExportSettings{};
        }
    }

    if (m_showExportDialog) {
        m_exporter.ResetProgress();
        m_exportChecked.assign(m_segments.GetCount(), true);
        m_frameExportChecked.assign(m_segments.GetFrameCount(), true);
        ImGui::OpenPopup("Export");
        m_showExportDialog = false;
    }

    bool popupVisibleThisFrame = ImGui::BeginPopupModal("Export", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    // Capture the prior-frame state before updating: the in-modal Cmd+E
    // close handler must NOT consume the same Cmd+E that just opened the
    // modal this frame, otherwise the modal opens and immediately closes.
    bool exportDialogWasOpenPriorFrame = m_exportDialogOpen;
    m_exportDialogOpen = popupVisibleThisFrame;
    if (popupVisibleThisFrame) {
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

                    // Format (toggle for segments, static PNG label for frames).
                    // For GIF sources both modes produce identical GIF output;
                    // lock the toggle to a disabled "GIF" button.
                    ImGui::TableNextColumn();
                    if (row.isFrame) {
                        ImGui::TextDisabled("PNG");
                    } else if (IsGifSource()) {
                        ImGui::BeginDisabled();
                        ImGui::SmallButton("GIF");
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted("Only GIF export format is supported for GIF source");
                            ImGui::EndTooltip();
                        }
                    } else {
                        std::string fmtLabel = (segs[row.index].mode == ExportMode::GIF)
                            ? std::string("GIF") : SourceFormatLabel();
                        if (ImGui::SmallButton(fmtLabel.c_str())) {
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

                    // Duration (effective, after speed) — editable for segments.
                    ImGui::TableNextColumn();
                    if (row.isFrame) {
                        ImGui::TextDisabled("-");
                    } else {
                        const TimeRange& s = segs[row.index];
                        double srcDur = s.endSec - s.startSec;
                        double effSpeed = (s.speed > 0.0) ? s.speed : 1.0;
                        double effDur = srcDur / effSpeed;
                        float effDurF = static_cast<float>(effDur);
                        ImGui::SetNextItemWidth(-FLT_MIN);  // fill table cell
                        if (ImGui::InputFloat("##dur", &effDurF, 0.0f, 0.0f, "%.1fs")) {
                            if (effDurF < 0.05f) effDurF = 0.05f;
                            double newEnd = s.startSec + effDurF * effSpeed;
                            double maxEnd = m_player.GetDuration();
                            if (newEnd > maxEnd) newEnd = maxEnd;
                            m_segments.UpdateSegment(row.index, s.startSec, newEnd);
                        }
                    }

                    // Audio toggle column. Frames have no audio anyway, so
                    // we just show a disabled label.
                    ImGui::TableNextColumn();
                    if (row.isFrame) {
                        ImGui::TextDisabled("-");
                    } else {
                        bool newKeep;
                        bool srcNoAudio = !m_player.HasAudio();
                        if (KeepAudioToggle("##audio", segs[row.index], srcNoAudio, newKeep, /*small*/ true)) {
                            m_segments.SetSegmentKeepAudio(row.index, newKeep);
                        }
                    }

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            // GIF settings (shown if any segment is set to GIF). Hidden for
            // GIF sources — output matches source W/FPS, the width/fps inputs
            // would have no effect.
            bool hasGif = false;
            if (!IsGifSource()) {
                for (const auto& s : segs)
                    if (s.mode == ExportMode::GIF) { hasGif = true; break; }
            }
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
                std::string stem = m_exportName;
                ImGui::BeginChild("##export_preview", ImVec2(0, 80.0f * m_ui.GetUiScale()),
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

            // HDR sources are tone-mapped to SDR when re-encoded (GIF/PNG). Show
            // which operator will be used so exports match what's on screen.
            if (m_videoColorMode != VideoColorMode::SDR) {
                ImGui::Spacing();
                const ImVec4 accent(0.35f, 0.70f, 1.0f, 1.0f); // info blue
                ImGui::PushStyleColor(ImGuiCol_Border, accent);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.70f, 1.0f, 0.08f));
                ImGui::BeginChild("##hdrExportInfo", ImVec2(dialogWidth, 0.0f),
                                  ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                  ImGuiWindowFlags_NoScrollbar);
                ImGui::TextColored(accent, "HDR video");
                ImGui::TextWrapped(
                    "GIF and PNG exports will be converted to SDR using the currently "
                    "selected tonemapper (%s). Source-format segment exports copy the "
                    "original HDR video untouched.",
                    TonemapperName(m_tonemapper));
                ImGui::EndChild();
                ImGui::PopStyleColor(2);
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
                // WYSIWYG: HDR re-encode exports use the operator the user is
                // currently viewing with.
                m_pendingExport.tonemapper = m_tonemapper;
                m_pendingExport.segments.clear();
                m_pendingExport.frames.clear();
                for (const Row& row : rows) {
                    if (!isChecked(row)) continue;
                    if (row.isFrame) m_pendingExport.frames.push_back(frames[row.index]);
                    else             m_pendingExport.segments.push_back(segs[row.index]);
                }
                m_pendingExport.outputPath = (std::filesystem::path(m_exportDir) / m_exportName).string();

                // Check for existing files. outputPath is dir + extension-less
                // base name, so filename() (not stem()) — base names may
                // contain dots.
                std::filesystem::path base(m_pendingExport.outputPath);
                std::string stem = base.filename().string();
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

            bool anyItemActive = ImGui::IsAnyItemActive();
            bool inputWasFocused = anyItemActive || m_exportDialogItemActiveLast;
            m_exportDialogItemActiveLast = anyItemActive;

            // Suppress the dialog's own shortcuts while the overwrite-confirm
            // child popup is showing — Esc / Cmd+E in that state should act
            // on the child, not close the whole export dialog. (The bare
            // m_showOverwriteConfirm flag is only true for the single frame
            // OpenPopup is requested, so we read the actual popup state.)
            bool overwriteOpen = ImGui::IsPopupOpen("Overwrite Files?");
            bool popupFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
                                && !overwriteOpen;
            if (popupFocused) {
                // ImGuiMod_Ctrl maps to Ctrl on Win/Linux and Cmd on macOS
                // (ImGui internally swaps Ctrl/Super on Mac). IsKeyChordPressed
                // handles the platform difference so we don't have to.
                // Cmd/Ctrl + Enter → Export. The modifier prevents accidental
                // exports while typing in name / duration / dir fields.
                if (canExport && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) {
                    triggerExport();
                }
                // Cmd/Ctrl + E → close. Guarded on "was open last frame" so
                // the Cmd+E that just opened the modal isn't consumed here
                // to immediately close it.
                if (exportDialogWasOpenPriorFrame && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E)) {
                    ImGui::CloseCurrentPopup();
                }
                // Esc → close, but only when no input is being edited (so the
                // user can press Esc to cancel an in-progress edit first).
                if (!inputWasFocused && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    ImGui::CloseCurrentPopup();
                }
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
            std::string stem = basePath.filename().string();
            ImGui::Text("Output folder:");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  %s", dir.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open")) {
                OpenFolderInShell(dirPath);
            }
            ImGui::Spacing();
            ImGui::Text("Exported files:");
            for (const auto& seg : m_pendingExport.segments) {
                std::string ext = (seg.mode == ExportMode::GIF) ? ".gif" : inputExt;
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
                std::string stem = base.filename().string();
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
            // Esc closes just this child popup (the parent's Esc handler is
            // gated by IsPopupOpen("Overwrite Files?") so it stays put).
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
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
            // Defer to Run()'s between-frames open path: this frame's ImGui
            // draw list already references the current video texture, so
            // OpenFile (which releases it) must not run mid-frame.
            m_pendingOpenImmediate = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
            m_pendingOpenFilePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    m_hoveredSegment = m_hoveredSegmentThisFrame;
    m_hoveredFrame = m_hoveredFrameThisFrame;

    // Render ImGui into the offscreen scene target, then blit that to the
    // (write-only) swapchain. On acquire failure or a zero-size swapchain
    // (minimized), EndFrame(null) still runs ImGui::Render() to keep the frame
    // lifecycle balanced.
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(m_gpuDevice);
    if (!cmd) {
        LOG_ERROR("Render: command buffer acquire failed: %s", SDL_GetError());
        m_ui.EndFrame(nullptr, nullptr);
        return;
    }

    SDL_GPUTexture* swapchain = nullptr;
    Uint32 swapW = 0, swapH = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, m_window, &swapchain, &swapW, &swapH)) {
        LOG_ERROR("Render: swapchain acquire failed: %s", SDL_GetError());
        m_ui.EndFrame(nullptr, nullptr);
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }
    if (!swapchain || swapW == 0 || swapH == 0 ||
        !EnsureSceneTarget(static_cast<int>(swapW), static_cast<int>(swapH))) {
        m_ui.EndFrame(nullptr, nullptr);
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    m_ui.EndFrame(cmd, m_sceneTarget);

    // Scene -> swapchain. SDR: a plain blit — the scene's encoded values are
    // the presentable values, and the FP16 -> 8-bit conversion clamps
    // inherently. HDR (scRGB): a composite pass that decodes to linear and
    // scales to the OS SDR white level.
    SDL_GPUGraphicsPipeline* composite = nullptr;
    if (m_swapchainHDR)
        composite = EnsureCompositePipeline(
            SDL_GetGPUSwapchainTextureFormat(m_gpuDevice, m_window));
    if (composite) {
        struct CompositeParams {
            int32_t mode;  // matches m_hdrCompositeMode (1 = scRGB, 2 = HDR10)
            float sdrWhite;
            float pad0, pad1;
        } params = {m_hdrCompositeMode, m_sdrWhite, 0.0f, 0.0f};
        SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params));

        SDL_GPUColorTargetInfo target = {};
        target.texture = swapchain;
        target.load_op = SDL_GPU_LOADOP_DONT_CARE;  // fullscreen triangle covers everything
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, composite);
        SDL_GPUTextureSamplerBinding binding = {};
        binding.texture = m_sceneTarget;
        binding.sampler = m_compositeSampler;
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    } else {
        SDL_GPUBlitInfo blit = {};
        blit.source.texture = m_sceneTarget;
        blit.source.w = swapW;
        blit.source.h = swapH;
        blit.destination.texture = swapchain;
        blit.destination.w = swapW;
        blit.destination.h = swapH;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_NEAREST;  // always 1:1
        SDL_BlitGPUTexture(cmd, &blit);
    }

    if (m_screenshotPending) {
        // Downloads the scene target; submits and waits on `cmd` itself.
        TakeScreenshot(cmd, static_cast<int>(swapW), static_cast<int>(swapH));
        m_screenshotPending = false;
    } else {
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    // One-time startup timing report (times are since SDL init).
    if (!m_startupReported) {
        m_startupReported = true;
        LOG_INFO("Startup: window shown %.0f ms, init done %.0f ms, first frame %.0f ms",
                 m_startupWindowShownNS / 1e6, m_startupInitDoneNS / 1e6,
                 SDL_GetTicksNS() / 1e6);
    }
}

void App::BumpUIActivity() {
    m_lastUIActivityNS = SDL_GetTicksNS();
    if (m_autoHideUI) m_uiHidden = false;
}

void App::TogglePlayPauseWithFlash() {
    m_player.TogglePlayPause();
    m_playPauseFlashStartNS = SDL_GetTicksNS();
    m_playPauseFlashIsPlaying = m_player.IsPlaying();
}

void App::CreateVideoTexture(int width, int height) {
    if (m_videoTexture) {
        SDL_ReleaseGPUTexture(m_gpuDevice, m_videoTexture);
        m_videoTexture = nullptr;
    }
    // May point at the texture released above; don't display anything until
    // the first frame lands in the new texture.
    m_displayTexture = nullptr;
    if (m_frameTransfer) {
        SDL_ReleaseGPUTransferBuffer(m_gpuDevice, m_frameTransfer);
        m_frameTransfer = nullptr;
    }

    // HDR frames are 10-bit packed BT.2020 (still PQ/HLG-encoded) and get
    // tone-mapped on the GPU; SDR frames are ready-to-display 8-bit sRGB.
    // (X2BGR10LE == R10G10B10A2_UNORM, both 4 bytes/pixel.)
    const bool hdr = (m_videoColorMode != VideoColorMode::SDR);

    SDL_GPUTextureCreateInfo texInfo = {};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format = hdr ? SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM
                         : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width = static_cast<Uint32>(width);
    texInfo.height = static_cast<Uint32>(height);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels = 1;
    m_videoTexture = SDL_CreateGPUTexture(m_gpuDevice, &texInfo);
    if (!m_videoTexture) {
        LOG_ERROR("CreateVideoTexture failed: %s", SDL_GetError());
        return;
    }

    SDL_GPUTransferBufferCreateInfo tbInfo = {};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbInfo.size = static_cast<Uint32>(width) * height * 4;
    m_frameTransfer = SDL_CreateGPUTransferBuffer(m_gpuDevice, &tbInfo);
    if (!m_frameTransfer)
        LOG_ERROR("CreateVideoTexture: transfer buffer failed: %s", SDL_GetError());
}

void App::UploadFrame(const uint8_t* rgba, int width, int height) {
    if (!m_videoTexture || !m_frameTransfer) return;

    void* mapped = SDL_MapGPUTransferBuffer(m_gpuDevice, m_frameTransfer, true);
    if (!mapped) return;
    std::memcpy(mapped, rgba, static_cast<size_t>(width) * height * 4);
    SDL_UnmapGPUTransferBuffer(m_gpuDevice, m_frameTransfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(m_gpuDevice);
    if (!cmd) return;
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = m_frameTransfer;
    src.pixels_per_row = static_cast<Uint32>(width);
    src.rows_per_layer = static_cast<Uint32>(height);
    SDL_GPUTextureRegion dst = {};
    dst.texture = m_videoTexture;
    dst.w = static_cast<Uint32>(width);
    dst.h = static_cast<Uint32>(height);
    dst.d = 1;
    SDL_UploadToGPUTexture(pass, &src, &dst, true);
    SDL_EndGPUCopyPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
}

#ifdef _WIN32
// Query the OS directly for the HDR state of the monitor under `window`:
// whether the desktop scans out in HDR (PQ colorspace), the SDR white level,
// and the resulting headroom. SDL exposes the same data as window properties,
// but only refreshes it on WM_DISPLAYCHANGE — moving the Windows "SDR content
// brightness" slider doesn't fire that, so SDL's values go stale.
//
// Polled every frame while HDR output is active so the app tracks the slider
// live. The DXGI factory and the resolved output are cached (~6 us per call
// vs ~0.3 ms with a fresh factory): the factory reports !IsCurrent() on any
// display topology/mode change (including HDR toggles), invalidating both,
// and the output is re-resolved when the window moves to another monitor.
static bool QueryNativeHDRInfo(SDL_Window* window, bool& hdrMode, float& sdrWhite,
                               float& headroom) {
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd)
        return false;
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return false;

    struct DXGICache {
        IDXGIFactory1* factory = nullptr;
        IDXGIOutput6* output = nullptr;
        HMONITOR monitor = nullptr;
        void ReleaseOutput() {
            if (output) {
                output->Release();
                output = nullptr;
            }
            monitor = nullptr;
        }
        void ReleaseAll() {
            ReleaseOutput();
            if (factory) {
                factory->Release();
                factory = nullptr;
            }
        }
        ~DXGICache() { ReleaseAll(); }
    };
    static DXGICache cache;

    if (cache.factory && !cache.factory->IsCurrent())
        cache.ReleaseAll();
    if (!cache.factory && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&cache.factory))))
        return false;
    if (cache.output && cache.monitor != monitor)
        cache.ReleaseOutput();
    if (!cache.output) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; !cache.output && cache.factory->EnumAdapters1(a, &adapter) == S_OK;
             a++) {
            IDXGIOutput* output = nullptr;
            for (UINT o = 0; !cache.output && adapter->EnumOutputs(o, &output) == S_OK; o++) {
                IDXGIOutput6* output6 = nullptr;
                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
                    DXGI_OUTPUT_DESC1 desc;
                    if (SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor) {
                        cache.output = output6;
                        cache.monitor = monitor;
                    } else {
                        output6->Release();
                    }
                }
                output->Release();
            }
            adapter->Release();
        }
        if (!cache.output)
            return false;
    }

    DXGI_OUTPUT_DESC1 desc;
    if (FAILED(cache.output->GetDesc1(&desc))) {
        cache.ReleaseOutput();
        return false;
    }
    const bool pq = (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    const float maxLum = desc.MaxLuminance;

    // SDR white level via DisplayConfig, matched by GDI device name.
    // SDRWhiteLevel is in 80-nit units scaled by 1000.
    float whiteNits = 200.0f;  // sane default if the query fails
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        UINT32 numPaths = 0, numModes = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) ==
                ERROR_SUCCESS && numPaths > 0) {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(),
                                   &numModes, modes.data(), nullptr) == ERROR_SUCCESS) {
                for (UINT32 i = 0; i < numPaths; i++) {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
                    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    src.header.size = sizeof(src);
                    src.header.adapterId = paths[i].sourceInfo.adapterId;
                    src.header.id = paths[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
                        wcscmp(mi.szDevice, src.viewGdiDeviceName) == 0) {
                        DISPLAYCONFIG_SDR_WHITE_LEVEL wl = {};
                        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                        wl.header.size = sizeof(wl);
                        wl.header.adapterId = paths[i].targetInfo.adapterId;
                        wl.header.id = paths[i].targetInfo.id;
                        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS &&
                            wl.SDRWhiteLevel > 0)
                            whiteNits = wl.SDRWhiteLevel / 1000.0f * 80.0f;
                        break;
                    }
                }
            }
        }
    }

    hdrMode = pq;
    sdrWhite = whiteNits / 80.0f;
    headroom = (maxLum > 0.0f && whiteNits > 0.0f) ? maxLum / whiteNits : 1.0f;
    return true;
}
#else
static bool QueryNativeHDRInfo(SDL_Window*, bool&, float&, float&) {
    return false;  // non-Windows: use SDL's window properties
}
#endif

SDL_GPUGraphicsPipeline* App::EnsureCompositePipeline(SDL_GPUTextureFormat format) {
    for (int i = 0; i < kMaxCompositePipelines; i++)
        if (m_compositePipelines[i] && m_compositeFormats[i] == format)
            return m_compositePipelines[i];

    int slot = -1;
    for (int i = 0; i < kMaxCompositePipelines; i++)
        if (!m_compositePipelines[i]) { slot = i; break; }
    if (slot < 0) {
        LOG_ERROR("Composite: out of pipeline slots (format %d)", static_cast<int>(format));
        return nullptr;
    }

    // Pick the shader blobs the device accepts (mirrors VideoTonemap::Init).
    const SDL_GPUShaderFormat avail = SDL_GetGPUShaderFormats(m_gpuDevice);
    SDL_GPUShaderFormat shaderFormat;
    tonemap_shader::Blob vertBlob, fragBlob;
    const char* entrypoint;
    if ((avail & SDL_GPU_SHADERFORMAT_MSL) && tonemap_shader::kCompositeFragMsl.size != 0) {
        shaderFormat = SDL_GPU_SHADERFORMAT_MSL;
        vertBlob = tonemap_shader::kVertMsl;
        fragBlob = tonemap_shader::kCompositeFragMsl;
        entrypoint = "main0";
    } else if ((avail & SDL_GPU_SHADERFORMAT_DXBC) && tonemap_shader::kCompositeFragDxbc.size != 0) {
        shaderFormat = SDL_GPU_SHADERFORMAT_DXBC;
        vertBlob = tonemap_shader::kVertDxbc;
        fragBlob = tonemap_shader::kCompositeFragDxbc;
        entrypoint = "main";
    } else if ((avail & SDL_GPU_SHADERFORMAT_SPIRV) && tonemap_shader::kCompositeFragSpirv.size != 0) {
        shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
        vertBlob = tonemap_shader::kVertSpirv;
        fragBlob = tonemap_shader::kCompositeFragSpirv;
        entrypoint = "main";
    } else {
        LOG_ERROR("Composite: no embedded shader for the device's formats (0x%x)", avail);
        return nullptr;
    }

    SDL_GPUShaderCreateInfo vsInfo = {};
    vsInfo.code = vertBlob.data;
    vsInfo.code_size = vertBlob.size;
    vsInfo.entrypoint = entrypoint;
    vsInfo.format = shaderFormat;
    vsInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShader* vs = SDL_CreateGPUShader(m_gpuDevice, &vsInfo);

    SDL_GPUShaderCreateInfo fsInfo = {};
    fsInfo.code = fragBlob.data;
    fsInfo.code_size = fragBlob.size;
    fsInfo.entrypoint = entrypoint;
    fsInfo.format = shaderFormat;
    fsInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsInfo.num_samplers = 1;
    fsInfo.num_uniform_buffers = 1;
    SDL_GPUShader* fs = SDL_CreateGPUShader(m_gpuDevice, &fsInfo);

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if (vs && fs) {
        SDL_GPUColorTargetDescription colorTarget = {};
        colorTarget.format = format;
        SDL_GPUGraphicsPipelineCreateInfo pipeInfo = {};
        pipeInfo.vertex_shader = vs;
        pipeInfo.fragment_shader = fs;
        pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeInfo.target_info.color_target_descriptions = &colorTarget;
        pipeInfo.target_info.num_color_targets = 1;
        pipeline = SDL_CreateGPUGraphicsPipeline(m_gpuDevice, &pipeInfo);
    }
    if (vs) SDL_ReleaseGPUShader(m_gpuDevice, vs);
    if (fs) SDL_ReleaseGPUShader(m_gpuDevice, fs);
    if (!pipeline) {
        LOG_ERROR("Composite: pipeline creation failed: %s", SDL_GetError());
        return nullptr;
    }

    if (!m_compositeSampler) {
        SDL_GPUSamplerCreateInfo samplerInfo = {};
        samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;  // always 1:1
        samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_compositeSampler = SDL_CreateGPUSampler(m_gpuDevice, &samplerInfo);
        if (!m_compositeSampler) {
            LOG_ERROR("Composite: sampler creation failed: %s", SDL_GetError());
            SDL_ReleaseGPUGraphicsPipeline(m_gpuDevice, pipeline);
            return nullptr;
        }
    }

    m_compositePipelines[slot] = pipeline;
    m_compositeFormats[slot] = format;
    return pipeline;
}

void App::UpdateHDROutput() {
    if (!m_gpuDevice || !m_window)
        return;

    // Prefer the direct OS query (fresh — tracks the Windows SDR-brightness
    // slider live); fall back to SDL's window properties elsewhere.
    bool hdrMode = false;
    float sdrWhite = 1.0f;
    float headroom = 1.0f;
    if (!QueryNativeHDRInfo(m_window, hdrMode, sdrWhite, headroom)) {
        SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
        hdrMode = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
        sdrWhite = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
        headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    }
    m_displayHDR = hdrMode;

    // HDR output is content-gated: only while an HDR video is open on a
    // display that's in HDR mode. SDR videos and the empty app present as a
    // plain SDR app, letting the OS do the SDR-to-HDR desktop mapping like
    // for every other window. Headroom is deliberately not part of the gate —
    // HDR video maps in absolute nits, and the rolloff just compresses harder
    // when headroom is low.
    const bool videoIsHDR =
        m_player.HasMedia() && m_videoColorMode != VideoColorMode::SDR;

    // Prefer scRGB (linear FP16 — what macOS EDR and the Vulkan backend
    // expose); fall back to HDR10 PQ, which is what stock SDL's D3D12 backend
    // reaches (its scRGB support check can only pass for a swapchain that
    // already has the FP16 buffer format). 0 = SDR.
    int wantMode = 0;
    if (m_hdrOutputEnabled && hdrMode && videoIsHDR) {
        if (SDL_WindowSupportsGPUSwapchainComposition(
                m_gpuDevice, m_window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR))
            wantMode = 1;
        else if (SDL_WindowSupportsGPUSwapchainComposition(
                     m_gpuDevice, m_window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084))
            wantMode = 2;
    }

    if (wantMode != m_hdrCompositeMode) {
        static const SDL_GPUSwapchainComposition kComps[3] = {
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
            SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR,
            SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084,
        };
        static const char* kNames[3] = {"SDR", "HDR (scRGB)", "HDR (HDR10 PQ)"};
        if (SDL_SetGPUSwapchainParameters(m_gpuDevice, m_window, kComps[wantMode],
                                          SDL_GPU_PRESENTMODE_VSYNC)) {
            m_hdrCompositeMode = wantMode;
            m_swapchainHDR = (wantMode != 0);
            LOG_INFO("Swapchain composition -> %s", kNames[wantMode]);
        } else {
            LOG_ERROR("SDL_SetGPUSwapchainParameters failed: %s", SDL_GetError());
        }
    }

    // Brightness metadata for the shaders — tracks the OS "SDR content
    // brightness" slider and the display's headroom.
    m_sdrWhite = std::max(sdrWhite, 1.0f);
    m_hdrHeadroom = std::max(headroom, 1.0f);

    // Log state changes at most once per second (the values change every
    // frame during a slider drag). Comparing against the last *logged* state
    // rather than the previous frame guarantees the settled value after a
    // burst still gets reported on a later frame.
    static bool loggedMode = false;
    static float loggedWhite = -1.0f;
    static float loggedHeadroom = -1.0f;
    static uint64_t lastLogNS = 0;
    const uint64_t nowNS = SDL_GetTicksNS();
    if ((hdrMode != loggedMode || m_sdrWhite != loggedWhite || m_hdrHeadroom != loggedHeadroom) &&
        (lastLogNS == 0 || nowNS - lastLogNS > 1000000000ull)) {
        loggedMode = hdrMode;
        loggedWhite = m_sdrWhite;
        loggedHeadroom = m_hdrHeadroom;
        lastLogNS = nowNS;
        LOG_INFO("HDR state: display %s, SDR white %.0f nits, headroom %.2fx",
                 hdrMode ? "HDR" : "SDR", m_sdrWhite * 80.0f, m_hdrHeadroom);
    }
}

bool App::EnsureSceneTarget(int width, int height) {
    if (m_sceneTarget && m_sceneW == width && m_sceneH == height)
        return true;

    if (m_sceneTarget) {
        SDL_ReleaseGPUTexture(m_gpuDevice, m_sceneTarget);
        m_sceneTarget = nullptr;
    }

    SDL_GPUTextureCreateInfo texInfo = {};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    texInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width = static_cast<Uint32>(width);
    texInfo.height = static_cast<Uint32>(height);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels = 1;
    m_sceneTarget = SDL_CreateGPUTexture(m_gpuDevice, &texInfo);
    if (!m_sceneTarget) {
        LOG_ERROR("EnsureSceneTarget failed: %s", SDL_GetError());
        return false;
    }
    m_sceneW = width;
    m_sceneH = height;
    return true;
}

float App::GetEffectiveUiScale() const {
#ifdef __APPLE__
    // On macOS the OS already handles scaling via the window's point/pixel
    // separation, so the automatic DPI component stays at 1.0.
    float dpi = 1.0f;
#else
    float dpi = m_useDpiScaling ? SDL_GetWindowDisplayScale(m_window) : 1.0f;
#endif
    // m_userUiScale is the user's explicit multiplier layered on top of the
    // automatic DPI scale (input in the View menu, 0.5x–2.0x).
    return dpi * m_userUiScale;
}

void App::GrowWindowForUiScale(float ratio) {
    if (ratio <= 1.0f || m_fullscreen || m_maximized) return;

    int ww = 0, wh = 0;
    SDL_GetWindowSize(m_window, &ww, &wh);
    int newW = static_cast<int>(ww * ratio);
    int newH = static_cast<int>(wh * ratio);
    SDL_SetWindowSize(m_window, newW, newH);

    // Keep the window center in place, clamped so the grown window stays within
    // the display work area. Position/size are client-area coordinates, so pad
    // the clamp with the decoration border sizes (title bar etc.) to keep those
    // on-screen too.
    int wx = 0, wy = 0;
    SDL_GetWindowPosition(m_window, &wx, &wy);
    int newX = wx + (ww - newW) / 2;
    int newY = wy + (wh - newH) / 2;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(m_window), &usable)) {
        int bTop = 0, bLeft = 0, bBottom = 0, bRight = 0;
        SDL_GetWindowBordersSize(m_window, &bTop, &bLeft, &bBottom, &bRight);
        newX = std::max(usable.x + bLeft,
               std::min(newX, usable.x + usable.w - newW - bRight));
        newY = std::max(usable.y + bTop,
               std::min(newY, usable.y + usable.h - newH - bBottom));
    }
    SDL_SetWindowPosition(m_window, newX, newY);
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

std::string App::SourceFormatLabel() const {
    std::string ext = std::filesystem::path(m_currentFilePath).extension().string();
    if (ext.empty() || ext[0] != '.') return "MP4";
    std::string lower(ext.begin() + 1, ext.end());
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    // MKV/WebM are remuxed to MP4 — must match Exporter::ExportThread.
    if (lower == "mkv" || lower == "webm") return "MP4";
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return lower;
}

bool App::IsGifSource() const {
    std::string ext = std::filesystem::path(m_currentFilePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return ext == ".gif";
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

// Scalar IEEE half -> float conversion for the FP16 scene-target readback.
static float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;  // +-0
        } else {
            // Subnormal half -> normalized float.
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);  // inf / NaN
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void App::TakeScreenshot(SDL_GPUCommandBuffer* cmd, int w, int h) {
    // Record a download of the scene target onto the frame's command buffer,
    // then submit it with a fence and wait — a screenshot is rare enough that
    // one stalled frame doesn't matter. Rows come back top-first, FP16 RGBA
    // (extended-sRGB encoded); clamped to 8-bit for the PNG, which matches
    // the SDR presentation of the frame.
    SDL_GPUTransferBufferCreateInfo tbInfo = {};
    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbInfo.size = static_cast<Uint32>(w) * h * 8;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(m_gpuDevice, &tbInfo);
    if (!tb) {
        LOG_ERROR("Screenshot: transfer buffer failed: %s", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = m_sceneTarget;
    src.w = static_cast<Uint32>(w);
    src.h = static_cast<Uint32>(h);
    src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = tb;
    dst.pixels_per_row = static_cast<Uint32>(w);
    dst.rows_per_layer = static_cast<Uint32>(h);
    SDL_DownloadFromGPUTexture(pass, &src, &dst);
    SDL_EndGPUCopyPass(pass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        LOG_ERROR("Screenshot: submit failed: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(m_gpuDevice, tb);
        return;
    }
    SDL_WaitForGPUFences(m_gpuDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(m_gpuDevice, fence);

    void* mapped = SDL_MapGPUTransferBuffer(m_gpuDevice, tb, false);
    if (!mapped) {
        LOG_ERROR("Screenshot: map failed: %s", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(m_gpuDevice, tb);
        return;
    }

    const uint16_t* halves = static_cast<const uint16_t*>(mapped);
    std::vector<uint8_t> rgba8(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < rgba8.size(); i++) {
        float v = HalfToFloat(halves[i]);
        v = std::clamp(v, 0.0f, 1.0f);
        rgba8[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
    }
    // Opaque alpha — the scene's alpha channel is meaningless after blending.
    for (size_t i = 3; i < rgba8.size(); i += 4)
        rgba8[i] = 255;

    auto dir = GetAppDataDir() / "screenshots";
    std::filesystem::create_directories(dir);
    m_screenshotCounter++;
    char name[64];
    snprintf(name, sizeof(name), "screenshot_%03d.png", m_screenshotCounter);
    auto path = (dir / name).string();

    if (stbi_write_png(path.c_str(), w, h, 4, rgba8.data(), w * 4))
        LOG_INFO("Screenshot saved: %s", path.c_str());
    else
        LOG_ERROR("Screenshot failed to write: %s", path.c_str());

    SDL_UnmapGPUTransferBuffer(m_gpuDevice, tb);
    SDL_ReleaseGPUTransferBuffer(m_gpuDevice, tb);
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

    // Remember whether we were maximized before going fullscreen, so we can
    // re-maximize on exit. Unlike the floating-window snapshots below, this
    // is captured on every entry — m_maximized is just a bool tracking SDL's
    // MAXIMIZED / RESTORED events, and by the time the user toggles F again
    // it accurately reflects whether they're maximized or not.
    if (fullscreen) {
        m_wasMaximizedBeforeFullscreen = m_maximized;
    }

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
        // Wait for the viewport to settle at its post-exit size before
        // restoring snapshot positions. Needed for macOS, which animates
        // the fullscreen exit and leaves the viewport at fullscreen size
        // for several frames; on Windows and Linux the wait completes on
        // the next frame, which is harmless. See the Render-loop settle
        // check for the matching consume.
        m_waitingForFullscreenExit = true;
    }

#ifdef _WIN32
    // SDL_SetWindowFullscreen uses exclusive fullscreen, which causes screen
    // flashing, slow Alt-Tab, and broken overlays. We want borderless fullscreen
    // instead, so set the Win32 window styles directly. (Originally this also
    // dodged a GL-era driver issue that promoted borderless windows to exclusive
    // fullscreen — https://github.com/libsdl-org/SDL/issues/12791 — and it
    // keeps the same proven behavior on the SDL_GPU/D3D12 backend.)
    SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));

    if (fullscreen) {
        // Refresh windowed geometry from SDL only if we're NOT maximized —
        // when maximized, m_windowedX/Y/W/H already holds the unmaximized
        // values from before maximizing, and SDL would report the maximized
        // bounds here (clobbering the restore size).
        if (!m_maximized) {
            SDL_GetWindowPosition(m_window, &m_windowedX, &m_windowedY);
            SDL_GetWindowSize(m_window, &m_windowedW, &m_windowedH);
        }

        // Clear the Win32 maximize state before changing window styles.
        // WINDOWPLACEMENT.showCmd persists across SetWindowLongPtr; without
        // this, our SetWindowPos to display bounds fights Windows' tracked
        // maximize-rect and the window ends up un-maximized at the work-area
        // size instead of going fullscreen. m_wasMaximizedBeforeFullscreen
        // already captured the bit we need on exit.
        //
        // Suppress the DWM maximize/restore animation just for this transition
        // — without it the user sees the window animate out of maximize then
        // pop to fullscreen, which feels janky. VLC achieves the "instant"
        // maximized → fullscreen look the same way. The animation is re-enabled
        // right after so the reverse (fullscreen → maximized) still animates.
        if (IsZoomed(hwnd)) {
            BOOL disable = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                                  &disable, sizeof(disable));
            ShowWindow(hwnd, SW_RESTORE);
        }

        SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
        SDL_Rect bounds;
        SDL_GetDisplayBounds(display, &bounds);

        SetWindowLongPtr(hwnd, GWL_STYLE, WS_OVERLAPPED | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowPos(hwnd, HWND_TOP, bounds.x, bounds.y, bounds.w, bounds.h,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

        // Re-enable DWM transitions so the next exit (fullscreen → windowed
        // or → maximized) animates normally.
        BOOL enable = FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                              &enable, sizeof(enable));

        // Mirror flag — ShowWindow(SW_RESTORE) above already cleared this
        // at the Win32 level (and fired SDL_EVENT_WINDOW_RESTORED), but the
        // event hasn't been dispatched yet during this call. Make sure our
        // state matches reality.
        m_maximized = false;
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
        UINT winDpi = GetDpiForWindow(hwnd);
        AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, winDpi);

        SetWindowPos(hwnd, HWND_NOTOPMOST, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

        // Re-maximize if we entered fullscreen from a maximized window.
        // Suppress the DWM maximize animation for the same reason as on
        // entry — the user expects an instant transition back to maximized,
        // not a window animating up from the windowed bounds we just set.
        // SDL_EVENT_WINDOW_MAXIMIZED will still fire and update m_maximized.
        if (m_wasMaximizedBeforeFullscreen) {
            BOOL disable = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                                  &disable, sizeof(disable));
            SDL_MaximizeWindow(m_window);
            BOOL enable = FALSE;
            DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                                  &enable, sizeof(enable));
            m_wasMaximizedBeforeFullscreen = false;
        }
    }
#elif defined(__APPLE__)
    // On macOS, SDL_SetWindowFullscreen works as expected. Maximized is a
    // non-concept on macOS (we treat fullscreen as the equivalent), so the
    // m_wasMaximizedBeforeFullscreen path above won't fire in practice.
    SDL_SetWindowFullscreen(m_window, fullscreen);
#else
    // Linux (X11/Wayland): mirror the Windows behaviour using SDL primitives.
    // SDL_SetWindowFullscreen doesn't reliably preserve maximize state across
    // the transition, so we handle it explicitly — un-maximize before going
    // fullscreen, re-maximize on exit if we came from maximized.
    if (fullscreen) {
        if (m_maximized) {
            SDL_RestoreWindow(m_window);
            m_maximized = false;
        }
        SDL_SetWindowFullscreen(m_window, true);
    } else {
        SDL_SetWindowFullscreen(m_window, false);
        if (m_wasMaximizedBeforeFullscreen) {
            SDL_MaximizeWindow(m_window);
            m_wasMaximizedBeforeFullscreen = false;
        }
    }
#endif
}
