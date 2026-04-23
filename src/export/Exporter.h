#pragma once

#include "util/Types.h"
#include "util/FFmpegUtils.h"

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
    void ResetProgress() { if (!m_progress.running) m_progress.Reset(); }
    const Progress& GetProgress() const { return m_progress; }
    bool IsRunning() const { return m_progress.running; }

private:
    void ExportThread();

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
};
