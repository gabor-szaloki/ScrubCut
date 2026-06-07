#pragma once

#include "util/FFmpegUtils.h"
#include "util/Types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Background worker that decodes one subtitle track — embedded or from an
// external subtitle file — once, into a time-sorted list of SubtitleEvents.
// This deliberately stays out of the live playback pipeline: subtitles are
// scanned once and then rendered by a simple time lookup, which scrubs and
// seeks for free. Modeled on WaveformExtractor.
//
// Lifetime:
//   Start(path, streamIndex) spawns a worker that runs to completion or until
//   Stop() (also called from the destructor / next Start / Reset). Calling
//   Start again replaces the previous run. streamIndex < 0 means "first
//   subtitle stream in the file" (used for external single-track files).
//
// Thread safety:
//   The worker appends to m_events under m_mutex; ActiveText() reads under the
//   same lock, so a still-running scan is safe to query.
class SubtitleExtractor {
public:
    SubtitleExtractor() = default;
    ~SubtitleExtractor();

    SubtitleExtractor(const SubtitleExtractor&) = delete;
    SubtitleExtractor& operator=(const SubtitleExtractor&) = delete;

    // Begin scanning the given track. Replaces any in-progress scan.
    void Start(const std::string& path, int streamIndex);

    // Stop the worker (joins) and drop all events. Idempotent.
    void Reset();

    bool IsRunning() const { return m_running.load(std::memory_order_relaxed); }
    bool HasEvents() const;

    // Return the cue text active at time t (empty if none). Lines are
    // '\n'-separated.
    std::string ActiveText(double t) const;

private:
    void Stop();
    void Worker(std::string path, int streamIndex);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};

    mutable std::mutex m_mutex;
    std::vector<SubtitleEvent> m_events; // sorted by startSec once scan completes
    // Longest cue duration, so ActiveText's binary search can bound how far back
    // it must look for an still-active (possibly overlapping) earlier cue.
    double m_maxDurationSec = 0.0;
};
