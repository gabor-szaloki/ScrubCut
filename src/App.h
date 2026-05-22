#pragma once

#include "ui/UIManager.h"
#include "core/Player.h"
#include "core/SegmentManager.h"
#include "core/WaveformExtractor.h"
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

    // Recent files: persisted list, MRU on top, capped at kRecentMax entries.
    static constexpr int kRecentMax = 10;
    std::vector<std::string> m_recentFiles;
    void LoadRecentFiles();
    void SaveRecentFiles();
    void AddToRecent(const std::string& path);

    // Refresh the auto-hide UI timer (and unhide if currently hidden) —
    // called whenever the user does something we consider UI activity:
    // mouse movement, mark/seek shortcuts, window-toggle shortcuts, etc.
    void BumpUIActivity();

    // Toggle playback and trigger the center-of-video Play/Pause flash —
    // call this from any user-initiated toggle (Space, video click, the
    // transport-bar Play button) so the flash fires consistently.
    void TogglePlayPauseWithFlash();

    void CreateVideoTexture(int width, int height);
    void UploadFrame(const uint8_t* rgba, int width, int height);
    void SetFullscreen(bool fullscreen);
    void RestoreFloatingWindowSnapshots();
    void TakeScreenshot();

    // Initialize m_exportDir for an export-dialog open based on mode:
    //   SameAsVideo → opened video's parent
    //   Custom      → m_exportCustomDir, defaulting to the video's parent if empty
    void InitExportDir();

    // Uppercased label for SourceFormat exports of the currently-open file.
    // MKV/WebM are remuxed to MP4, so they label as "MP4"; everything else
    // labels as its source extension (e.g. "MOV", "GIF").
    std::string SourceFormatLabel() const;

    // True when the opened file is a GIF — the format toggle is locked to
    // GIF (both modes produce identical output) and GIF Settings are hidden.
    bool IsGifSource() const;

    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    UIManager m_ui;
    Settings m_layoutSettings;
    Settings m_prefSettings;
    bool m_running = false;

    Player m_player;
    SegmentManager m_segments;
    Exporter m_exporter;
    WaveformExtractor m_waveform;

    // UI state — window visibility
    bool m_showTimeline = true;
    bool m_showSegments = false;
    bool m_segmentsClosedManually = false;
    bool m_showExportDialog = false;
    bool m_showHelpPanel = false;
    // Reset Layout briefly forces Marks/Help visible so their Begin blocks
    // run with layoutCond = Always (re-applying their default Pos/Size).
    // After that frame this flag triggers re-hiding them. Avoids
    // duplicating window defaults in the reset code path.
    bool m_hideFloatingWindowsAfterReset = false;
    bool m_showChapters = true;
    bool m_showWaveform = false;
    bool m_showTooltips = true;
    bool m_useDpiScaling = false;
    bool m_autoHideCursor = true;
    bool m_autoHideUI = false;

    // Returns the current DPI scale to apply to the UI. Wraps
    // SDL_GetWindowDisplayScale and returns 1.0 if DPI scaling is disabled
    // or on platforms where we don't support it.
    float GetEffectiveDpiScale() const;
    bool m_uiHidden = false;
    bool m_fullscreen = false;
    int m_windowedX = 0, m_windowedY = 0, m_windowedW = 1280, m_windowedH = 720;
    bool m_maximized = false;
    bool m_wasMaximizedBeforeFullscreen = false;
    bool m_waitingForFullscreenExit = false;

    // Floating window geometry snapshot (position + size) taken when entering
    // fullscreen, so it can be restored exactly on exit.
    struct FloatingWindowSnap { ImVec2 pos; ImVec2 size; bool valid = false; };
    FloatingWindowSnap m_snapTimeline, m_snapSegments, m_snapHelp;

    // Center-of-video Play/Pause flash triggered by clicking the video.
    // m_playPauseFlashStartNS is the SDL_GetTicksNS when the click happened;
    // m_playPauseFlashIcon captures which icon to show (the new playback
    // state at the moment of the click).
    uint64_t m_playPauseFlashStartNS = 0;
    bool m_playPauseFlashIsPlaying = false;
    float m_uiAlpha = 1.0f;
    uint64_t m_lastUIActivityNS = 0;
    ImVec2 m_prevViewportSize = {0, 0};
    ExportSettings m_pendingExport;
    std::vector<bool> m_exportChecked;
    std::vector<bool> m_frameExportChecked;
    bool m_showOverwriteConfirm = false;
    bool m_showOpenFileConfirm = false;
    // Cmd+E from the global keyboard handler — resolved in Render where
    // ImGui state is current. (The handler runs before BeginFrame, when
    // ImGui's CurrentWindow is null and calls like IsPopupOpen crash.)
    bool m_pendingExportToggle = false;
    // Self-tracked "Export modal currently open" — updated from the result of
    // BeginPopupModal each frame, used to gate the open path in the toggle
    // resolution.
    bool m_exportDialogOpen = false;
    // Previous frame's "any item active" state inside the Export dialog. Used
    // so an Esc-press that just deactivated an InputText/InputFloat doesn't
    // also fire the dialog's Esc-closes-dialog shortcut.
    bool m_exportDialogItemActiveLast = false;
    bool m_pendingOpenImmediate = false;
    std::string m_pendingOpenFilePath;
    std::vector<std::string> m_conflictingFiles;
    char m_exportDir[512] = "";
    char m_exportName[256] = "";

    // Export output-directory mode. SameAsVideo (default) auto-fills the
    // export dir with the opened video's parent on each open. Custom uses
    // m_exportCustomDir, persisted across sessions.
    enum class ExportDirMode { SameAsVideo = 0, Custom = 1 };
    ExportDirMode m_exportDirMode = ExportDirMode::SameAsVideo;
    char m_exportCustomDir[512] = "";
    std::string m_currentFilePath;

    // Display
    GLuint m_videoTexture = 0;
    int m_videoWidth = 0;
    int m_videoHeight = 0;

    // Segment hover state (shared between timeline bar and segments panel)
    int m_hoveredSegment = -1;       // last frame's value, used for rendering
    int m_hoveredSegmentThisFrame = -1;  // being built this frame

    // Frame-mark hover state (same two-phase pattern)
    int m_hoveredFrame = -1;
    int m_hoveredFrameThisFrame = -1;

    // Screenshot request flag — consumed right before the frame's SwapWindow.
    bool m_screenshotPending = false;
    int m_screenshotCounter = 0;

    // Seek/scrub state
    double m_seekTarget = 0.0;
    uint64_t m_lastSeekTime = 0;
    bool m_isTimelineSeeking = false;
    bool m_wasPlayingBeforeTimelineSeek = false;

    // Precision-scrub anchor (active while any timeline drag is in progress).
    // Holding Alt mid-drag switches to 0.1× sensitivity, rebased around the
    // current mouse/target so there's no visual jump.
    float  m_scrubAnchorX = 0.0f;
    double m_scrubAnchorTime = 0.0;
    bool   m_scrubAltWasHeld = false;
    double ComputeScrubTarget(float mouseX, float barWidth, double duration,
                              double initialTime, bool justActivated);

    // Ctrl+click/drag on the timeline bar creates marks: click = Frame,
    // drag = Segment. State captured at drag start, finalised on release.
    bool   m_barCtrlMode = false;
    float  m_barCtrlStartX = 0.0f;
    double m_barCtrlStartTime = 0.0;
    int    m_barCtrlSegIdx = -1;
};
