#pragma once

#include "ui/UIManager.h"
#include "core/Player.h"
#include "core/SegmentManager.h"
#include "core/WaveformExtractor.h"
#include "core/SubtitleExtractor.h"
#include "util/Types.h"
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

    // True if `path` has a subtitle-file extension (.srt/.ass/.ssa/.vtt/.sub).
    static bool IsSubtitlePath(const std::string& path);

private:
    void ProcessEvents();
    void Render();

    // --- Media: audio / subtitle track selection ---
    // Switch the active audio track and flash a status overlay. Used by both
    // the menu and the cycle hotkey.
    void SelectAudioTrack(int streamIndex);
    // Load an external subtitle file over the open video, append it to the
    // track list, and make it active.
    void OpenSubtitleFile(const std::string& path);
    // Select a subtitle track by index into m_subtitleTracks, or -1 for off.
    void SelectSubtitleTrack(int index);
    // Cycle the active audio / subtitle track by +1 or -1 (subtitles wrap
    // through "off"). No-op when there's nothing to cycle.
    void CycleAudioTrack(int dir);
    void CycleSubtitleTrack(int dir);
    // Adjust the subtitle delay by `deltaSec`.
    void NudgeSubtitleDelay(double deltaSec);
    // Reset per-file subtitle state and rebuild the track list from the player
    // (embedded tracks only); called from OpenFile.
    void ResetSubtitleState();
    // Draw the active subtitle cue over the video rect, if any.
    void RenderSubtitleOverlay(const ImVec2& imgMin, const ImVec2& imgMax);

    // Flash a short, fading status message in the top-right of the video
    // (track switches, delay/size nudges) — the counterpart to the centre
    // play/pause flash.
    void ShowStatus(const std::string& msg);
    void RenderStatusOverlay(const ImVec2& imgMin, const ImVec2& imgMax);

    // Combined subtitle track list: embedded tracks (copied from the Player on
    // open) followed by any externally-opened files. -1 = subtitles off.
    std::vector<SubtitleTrackInfo> m_subtitleTracks;
    int m_activeSubtitle = -1;
    double m_subtitleDelaySec = 0.0;
    // Subtitle text size multiplier (on top of the video-relative base size).
    // Persisted in preferences.ini.
    float m_subtitleScale = 1.0f;
    SubtitleExtractor m_subtitleExtractor;

    // Top-right status flash.
    std::string m_statusMsg;
    uint64_t m_statusMsgStartNS = 0;

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
    bool m_useDpiScaling = true;
    // Explicit user-set UI scale (0.5x–2.0x), applied as a multiplier on top of
    // the automatic DPI scale in GetEffectiveUiScale.
    float m_userUiScale = 1.0f;
    bool m_autoHideCursor = true;
    bool m_autoHideUI = false;

    // Returns the effective UI scale to apply: the SDL display (DPI) scale when
    // DPI scaling is enabled (1.0 otherwise, and on platforms we don't support),
    // multiplied by the user's explicit m_userUiScale.
    float GetEffectiveUiScale() const;

    // Grow the window by `ratio` (>1) about its center, clamped to the display's
    // usable bounds, so an enlarged UI isn't cramped. No-op for ratio <= 1 or
    // when fullscreen/maximized. The resulting viewport resize lets the
    // proportional-repositioning pass scale the Timeline width to match.
    void GrowWindowForUiScale(float ratio);
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
    // Subtitle file chosen via the Open dialog. The dialog callback can fire on
    // a non-main thread, so it stashes the path here and Run() applies it on the
    // main thread (same pattern as m_pendingOpenFilePath).
    std::string m_pendingSubtitlePath;
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
