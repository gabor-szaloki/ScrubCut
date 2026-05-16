#pragma once

#include "util/FFmpegUtils.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// Background worker that decodes a file's audio track once and downsamples
// it to a fixed-size peak array. Used to overlay a waveform on the timeline.
//
// Lifetime:
//   Start(path, duration) spawns a worker thread that runs to completion or
//   until Stop() is called (also called from the destructor / next Start).
//   Calling Start on a fresh path replaces the previous run.
//
// Thread safety:
//   The worker only writes m_peaks[0..m_filledBuckets-1], and only ever
//   advances m_filledBuckets monotonically. The main thread reads filled
//   buckets via acquire-load and reads peaks below that index — values
//   are stable once exposed, so no per-bucket locks are needed.
class WaveformExtractor {
public:
    static constexpr int kBuckets = 4096;

    WaveformExtractor();
    ~WaveformExtractor();

    WaveformExtractor(const WaveformExtractor&) = delete;
    WaveformExtractor& operator=(const WaveformExtractor&) = delete;

    // Begin scanning. Replaces any in-progress scan.
    void Start(const std::string& path, double durationSec);

    // Stop the worker (joins). Idempotent.
    void Stop();

    // Stop the worker and clear all stored peaks. Used when switching to a
    // file we don't want to scan (waveform disabled) so stale data from the
    // previously-open file doesn't leak through if the user re-enables it.
    void Reset();

    int GetFilledBuckets() const { return m_filledBuckets.load(std::memory_order_acquire); }
    float GetPeak(int bucket) const { return m_peaks[bucket]; }

    bool IsRunning() const { return m_running.load(std::memory_order_relaxed); }

private:
    void Worker(std::string path, double durationSec);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_filledBuckets{0};
    std::vector<float> m_peaks;
};
