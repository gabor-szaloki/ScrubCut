#pragma once

#include "ui/UIManager.h"
#include "core/Player.h"
#include "core/SegmentManager.h"
#include "core/WaveformExtractor.h"
#include "core/OcrIndexer.h"
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

    void CreateVideoTexture(int width, int height);
    void UploadFrame(const uint8_t* rgba, int width, int height);
    void SetFullscreen(bool fullscreen);
    void RestoreFloatingWindowSnapshots();
    void TakeScreenshot();

    // Initialize m_exportDir for an export-dialog open based on mode:
    //   SameAsVideo → opened video's parent
    //   Custom      → m_exportCustomDir, defaulting to the video's parent if empty
    void InitExportDir();

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
    OcrIndexer m_ocrIndex;

    // Text-search state. Indexer is started lazily on the first Cmd+F for a
    // given file (`m_lastIndexedFile` tracks which file the current index
    // corresponds to). m_activeMatchIndex is the result index navigated by
    // F3 / Shift+F3 and rendered with a brighter overlay.
    bool m_showSearchPanel = false;
    bool m_searchPanelJustOpened = false;
    char m_searchQuery[256] = "";
    int  m_activeMatchIndex = -1;
    std::string m_lastIndexedFile;
    // One-shot: forces the Search panel to its current default pos/size on
    // the first time it's rendered for a user upgrading from a build with a
    // different default. Persists `search_panel_layout_v2=1` in prefs once
    // applied so user-moved positions stick after that.
    bool m_searchPanelLayoutNeedsReset = false;

    // Build the list of indices into `occs` that match the current
    // m_searchQuery (or every index when the query is empty). Used by F3
    // nav and the panel/timeline/bbox renderers to stay in sync about
    // which occurrences are "matches" and which one is active.
    std::vector<int> BuildSearchMatchIdx(const std::vector<TextOccurrence>& occs) const;
    // Advance m_activeMatchIndex by ±1 (wrapping) and seek the player to
    // that match's startSec. No-op when there are no matches.
    void NavigateMatch(bool forward);

    // Headless harness for `-test-search`: walks through a scripted sequence
    // (auto-open panel → wait for indexer → screenshot → inject query →
    // screenshot → quit) so the search UI is exercised end-to-end from a
    // shell command. Phase + frame counter advance each Run iteration.
    enum class TestSearchPhase {
        Off,
        WaitingForIndex,
        ShotEmptyQuery,
        InjectQuery,
        SeekToMatch,
        WaitForSeek,
        ShotFilteredQuery,
        ClearQuery,
        NavF3,
        WaitForNavSeek,
        ShotAfterNav,
        Done
    };
    TestSearchPhase m_testSearchPhase = TestSearchPhase::Off;
    std::string m_testSearchQuery;
    int m_testSearchPhaseFrames = 0;
    void TickTestSearch();

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
    bool m_autoHideUI = true;

    // Returns the current DPI scale to apply to the UI. Wraps
    // SDL_GetWindowDisplayScale and returns 1.0 if DPI scaling is disabled
    // or on platforms where we don't support it.
    float GetEffectiveDpiScale() const;
    bool m_uiHidden = false;
    bool m_fullscreen = false;
    int m_windowedX = 0, m_windowedY = 0, m_windowedW = 1280, m_windowedH = 720;
    bool m_waitingForFullscreenExit = false;

    // Floating window geometry snapshot (position + size) taken when entering
    // fullscreen, so it can be restored exactly on exit.
    struct FloatingWindowSnap { ImVec2 pos; ImVec2 size; bool valid = false; };
    FloatingWindowSnap m_snapTimeline, m_snapSegments, m_snapHelp;
    float m_uiAlpha = 1.0f;
    uint64_t m_lastUIActivityNS = 0;
    ImVec2 m_prevViewportSize = {0, 0};
    ExportSettings m_pendingExport;
    std::vector<bool> m_exportChecked;
    std::vector<bool> m_frameExportChecked;
    bool m_showOverwriteConfirm = false;
    bool m_showOpenFileConfirm = false;
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
