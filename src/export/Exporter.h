#pragma once

#include "util/Types.h"
#include "util/FFmpegUtils.h"
#include "ui/VideoTonemap.h"

#include <SDL3/SDL_gpu.h>

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

    // Provide the shared GPU device so the export thread can run the HDR
    // tone-map pass (VideoTonemap) on its own command buffers. Owned by App;
    // call once after device creation. Without it, HDR re-encode exports fail
    // with an error rather than producing wrong output.
    void SetTonemapDevice(SDL_GPUDevice* device) { m_gpuDevice = device; }
    void ResetProgress() { if (!m_progress.running) m_progress.Reset(); }
    const Progress& GetProgress() const { return m_progress; }
    bool IsRunning() const { return m_progress.running; }

private:
    void ExportThread();

    // Lazily create the tone-map pipeline on the shared device. Returns true
    // if the tone-mapper is ready to use.
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

    // Shared GPU device for HDR tone-mapping on the export thread (set by App).
    SDL_GPUDevice* m_gpuDevice = nullptr;
    VideoTonemap m_tonemap;
};
