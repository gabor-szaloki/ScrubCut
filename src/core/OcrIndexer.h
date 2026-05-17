#pragma once

#include "util/Types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Background worker that decodes a video's frames at 1 fps, runs Tesseract
// OCR on each (skipping near-identical frames), and emits TextOccurrence
// entries: a contiguous time range plus the OCR'd lines and their bounding
// boxes (normalized 0..1 in source-frame coords).
//
// Lifetime mirrors WaveformExtractor: Start spawns a thread, Stop joins,
// Reset clears occurrences. Destructor calls Stop. Snapshot returns a copy
// of the current occurrences vector — pushes happen infrequently (at most
// once per sampled second on dedup break) so the mutex contention is trivial.
class OcrIndexer {
public:
    OcrIndexer();
    ~OcrIndexer();

    OcrIndexer(const OcrIndexer&) = delete;
    OcrIndexer& operator=(const OcrIndexer&) = delete;

    void Start(const std::string& path, double durationSec);
    void Stop();
    void Reset();

    int  GetProcessedSeconds() const { return m_processedSeconds.load(std::memory_order_acquire); }
    int  GetTotalSeconds()     const { return m_totalSeconds.load(std::memory_order_relaxed); }
    bool IsRunning()           const { return m_running.load(std::memory_order_relaxed); }

    std::vector<TextOccurrence> Snapshot() const;

    // Verifies Tesseract can be initialized with the bundled eng.traineddata.
    // Cheap (no per-pixel work); used as a M1 link-and-load check at app
    // startup. Returns true on success.
    static bool SelfTest();

private:
    void Worker(std::string path, double durationSec);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<int>  m_processedSeconds{0};
    std::atomic<int>  m_totalSeconds{0};

    mutable std::mutex m_mu;
    std::vector<TextOccurrence> m_occurrences;
};
