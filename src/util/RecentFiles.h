#pragma once

#include "util/Types.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Most-recently-used files with their saved marks and playhead (recents.ini).
//
// Mutators update the in-memory list at once and queue the change for a
// worker thread, which re-reads the file, applies the queued changes and
// writes it back — so file I/O never runs on the UI thread, and an instance
// never overwrites other instances' changes with a stale copy.
struct RecentEntry {
    std::string path;
    std::vector<TimeRange> segments;
    std::vector<FrameMark> frames;
    // positionSec < 0: no playhead saved.
    double positionSec = -1.0;
    bool paused = false;

    int MarkCount() const { return static_cast<int>(segments.size() + frames.size()); }
};

class RecentFiles {
public:
    static constexpr int kDefaultMax = 10;
    static constexpr int kMinMax = 1;
    static constexpr int kMaxMax = 100;

    RecentFiles() = default;
    ~RecentFiles();
    RecentFiles(const RecentFiles&) = delete;
    RecentFiles& operator=(const RecentFiles&) = delete;

    // Reads the file and starts the save worker. Returns false if the file
    // doesn't exist yet.
    bool Load(const std::filesystem::path& path);
    // Writes any queued changes and joins the worker. Call before
    // Profiler::Shutdown() (the worker is instrumented).
    void Stop();
    // Re-read the file (synchronously), picking up other instances' changes.
    void Refresh();

    const std::vector<RecentEntry>& Entries() const { return m_entries; }

    // nullptr if `path` isn't in the list.
    const RecentEntry* Find(const std::string& path) const;
    // Move `path` to the front (adding it if new), keeping its saved data.
    void Touch(const std::string& path);
    // Replace the saved marks for `path`. An unlisted path (e.g. after Clear
    // Recents) is re-added only if there are marks to keep.
    void SetMarks(const std::string& path, const std::vector<TimeRange>& segments,
                  const std::vector<FrameMark>& frames);
    // Record the playhead for `path` (no-op if it isn't listed).
    void SetPlayhead(const std::string& path, double positionSec, bool paused);
    void Clear();

    int GetMax() const { return m_max; }
    // Clamped to [kMinMax, kMaxMax]; drops the oldest entries over the limit.
    void SetMax(int max);

private:
    using Op = std::function<void(std::vector<RecentEntry>&)>;
    void Apply(Op op);  // apply to m_entries now, queue for the worker
    void Worker();

    std::filesystem::path m_path;
    std::vector<RecentEntry> m_entries;  // UI thread only
    int m_max = kDefaultMax;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Op> m_queue;  // guarded by m_mutex; kept until written
    bool m_stop = false;      // guarded by m_mutex
};
