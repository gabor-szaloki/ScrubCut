#pragma once

#include "util/Types.h"
#include "util/FFmpegUtils.h"
#include "ui/VideoTonemap.h"

#include <SDL3/SDL_video.h>

#include <string>
#include <thread>
#include <atomic>
#include <mutex>

class Exporter {
public:
    struct Progress {
        std::atomic<float> fraction{0.0f};
        std::atomic<int> currentItem{0};
        std::atomic<int> totalItems{0};
        std::atomic<bool> running{false};
        std::atomic<bool> finished{false};
        std::atomic<bool> error{false};

        std::mutex messageMutex;
        std::string errorMessage;

        void SetError(const std::string& msg) {
            {
                std::lock_guard<std::mutex> lock(messageMutex);
                errorMessage = msg;
            }
            error = true;
        }

        std::string GetError() const {
            std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(messageMutex));
            return errorMessage;
        }

        void Reset() {
            fraction = 0.0f;
            currentItem = 0;
            totalItems = 0;
            running = false;
            finished = false;
            error = false;
            std::lock_guard<std::mutex> lock(messageMutex);
            errorMessage.clear();
        }
    };

    Exporter() = default;
    ~Exporter();

    Exporter(const Exporter&) = delete;
    Exporter& operator=(const Exporter&) = delete;

    void Start(const std::string& inputPath, const ExportSettings& settings);
    void Cancel();

    // Provide an offscreen GL window+context so the export thread can run the
    // HDR tone-map shader (VideoTonemap). Owned by App; call once after the main
    // GL context is created. Without it, HDR re-encode exports fail with an
    // error rather than producing wrong output.
    void SetTonemapContext(SDL_Window* window, SDL_GLContext context) {
        m_glWindow = window;
        m_glContext = context;
    }
    void ResetProgress() { if (!m_progress.running) m_progress.Reset(); }
    const Progress& GetProgress() const { return m_progress; }
    bool IsRunning() const { return m_progress.running; }

private:
    void ExportThread();

    // Lazily compile the tone-map shader (requires the export GL context to be
    // current). Returns true if the tone-mapper is ready to use.
    bool EnsureTonemap();

    bool ExportSegmentStreamCopy(const std::string& inputPath,
                                  const TimeRange& range,
                                  const std::string& outputPath);

    bool ExportSegmentGIF(const std::string& inputPath,
                          const TimeRange& range,
                          const std::string& outputPath,
                          int gifWidth, double gifFps);

    bool ExportFramePNG(const std::string& inputPath,
                        const FrameMark& frame,
                        const std::string& outputPath);

    std::string BuildOutputPath(const std::string& basePath, const std::string& markName,
                                int fallbackIndex, const std::string& extension) const;

    std::thread m_thread;
    Progress m_progress;
    std::string m_inputPath;
    ExportSettings m_settings;
    std::atomic<bool> m_cancel{false};

    // Offscreen GL context for HDR tone-mapping on the export thread (set by App).
    SDL_Window* m_glWindow = nullptr;
    SDL_GLContext m_glContext = nullptr;
    VideoTonemap m_tonemap;
    bool m_glCurrent = false;  // is the export context current on this thread
};
