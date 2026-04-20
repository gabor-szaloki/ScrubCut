#pragma once

#include "ui/UIManager.h"
#include "core/Player.h"
#include "core/SegmentManager.h"
#include "export/Exporter.h"
#include "util/Settings.h"

#include <imgui.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <string>
#include <vector>

class App {
public:
    bool Init();
    void Run();
    void Shutdown();

    void OpenFile(const std::string& path);
    void RequestOpenFile(const std::string& path);

private:
    void ProcessEvents();
    void Render();

    void CreateVideoTexture(int width, int height);
    void UploadFrame(const uint8_t* rgba, int width, int height);
    void SetFullscreen(bool fullscreen);

    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    UIManager m_ui;
    Settings m_layoutSettings;
    Settings m_prefSettings;
    bool m_running = false;

    Player m_player;
    SegmentManager m_segments;
    Exporter m_exporter;

    // UI state — window visibility
    bool m_showTimeline = true;
    bool m_showSegments = false;
    bool m_segmentsClosedManually = false;
    bool m_showExportDialog = false;
    bool m_showHelpPanel = false;
    bool m_useDpiScaling = false;
    bool m_autoHideCursor = true;
    bool m_autoHideUI = true;

    // Returns the current DPI scale to apply to the UI. Wraps
    // SDL_GetWindowDisplayScale and returns 1.0 if DPI scaling is disabled
    // or on platforms where we don't support it.
    float GetEffectiveDpiScale() const;
    bool m_uiHidden = false;
    bool m_fullscreen = false;
    int m_windowedX = 0, m_windowedY = 0, m_windowedW = 1280, m_windowedH = 720;

    // Floating window geometry snapshot (position + size) taken when entering
    // fullscreen, so it can be restored exactly on exit.
    struct FloatingWindowSnap { ImVec2 pos; ImVec2 size; bool valid = false; };
    FloatingWindowSnap m_snapTimeline, m_snapSegments, m_snapHelp;
    float m_uiAlpha = 1.0f;
    uint64_t m_lastUIActivityNS = 0;
    ImVec2 m_prevViewportSize = {0, 0};
    ExportSettings m_pendingExport;
    std::vector<bool> m_exportChecked;
    bool m_showOverwriteConfirm = false;
    bool m_showOpenFileConfirm = false;
    bool m_pendingOpenImmediate = false;
    std::string m_pendingOpenFilePath;
    std::vector<std::string> m_conflictingFiles;
    char m_exportDir[512] = "";
    char m_exportName[256] = "";
    std::string m_currentFilePath;

    // Display
    GLuint m_videoTexture = 0;
    int m_videoWidth = 0;
    int m_videoHeight = 0;

    // Segment hover state (shared between timeline bar and segments panel)
    int m_hoveredSegment = -1;       // last frame's value, used for rendering
    int m_hoveredSegmentThisFrame = -1;  // being built this frame

    // Seek/scrub state
    double m_seekTarget = 0.0;
    uint64_t m_lastSeekTime = 0;
    bool m_isTimelineSeeking = false;
    bool m_wasPlayingBeforeTimelineSeek = false;
};
