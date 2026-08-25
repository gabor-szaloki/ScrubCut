#pragma once

#include "core/ConvertedFrame.h"
#include "util/FFmpegUtils.h"
#include <algorithm>
#include <mutex>
#include <vector>

// One contiguous window of converted frames around the resting playhead,
// ordered by pts. Player::MaintainCacheWindow keeps it spanning a few GOPs
// (grow toward the stepping direction, trim the far side) on the seek thread
// while the main thread steps through it — hence the internal mutex. Entries
// share ownership of the pixel buffers (see ConvertedFrame), so Put is a
// refcount bump rather than a copy. Frames without a pts are not cached.
class FrameCache {
public:
    // Store a frame (sorted insert; an existing entry with the same pts is
    // replaced).
    void Put(ConvertedFramePtr frame) {
        if (!frame || frame->pts == AV_NOPTS_VALUE) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = LowerBound(frame->pts);
        if (it != m_frames.end() && (*it)->pts == frame->pts)
            *it = std::move(frame);
        else
            m_frames.insert(it, std::move(frame));
    }

    // Find the frame with the largest PTS that is < the given PTS.
    ConvertedFramePtr FindBefore(int64_t pts) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = LowerBound(pts);
        if (it == m_frames.begin()) return nullptr;
        return *std::prev(it);
    }

    // Find the frame closest to the given PTS.
    ConvertedFramePtr FindNearest(int64_t targetPts) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frames.empty()) return nullptr;
        auto it = LowerBound(targetPts);
        if (it == m_frames.end()) return m_frames.back();
        if (it == m_frames.begin()) return m_frames.front();
        auto prev = std::prev(it);
        return (targetPts - (*prev)->pts <= (*it)->pts - targetPts) ? *prev : *it;
    }

    // Find exact PTS match.
    ConvertedFramePtr FindExact(int64_t pts) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = LowerBound(pts);
        if (it != m_frames.end() && (*it)->pts == pts) return *it;
        return nullptr;
    }

    void DiscardBefore(int64_t pts) {  // remove entries with pts < given
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frames.erase(m_frames.begin(), LowerBound(pts));
    }

    void DiscardAfter(int64_t pts) {   // remove entries with pts > given
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frames.erase(LowerBound(pts + 1), m_frames.end());
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frames.clear();
    }

    int64_t StartPts() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_frames.empty() ? AV_NOPTS_VALUE : m_frames.front()->pts;
    }

    int64_t EndPts() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_frames.empty() ? AV_NOPTS_VALUE : m_frames.back()->pts;
    }

    // PTS of every cached keyframe, ascending. Small (a handful of GOPs).
    std::vector<int64_t> KeyframePts() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<int64_t> kfs;
        for (const auto& f : m_frames)
            if (f->keyframe) kfs.push_back(f->pts);
        return kfs;
    }

    int Count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_frames.size());
    }

private:
    // First entry with pts >= the given pts. Callers hold m_mutex.
    std::vector<ConvertedFramePtr>::const_iterator LowerBound(int64_t pts) const {
        return std::lower_bound(m_frames.begin(), m_frames.end(), pts,
            [](const ConvertedFramePtr& f, int64_t p) { return f->pts < p; });
    }
    std::vector<ConvertedFramePtr>::iterator LowerBound(int64_t pts) {
        return std::lower_bound(m_frames.begin(), m_frames.end(), pts,
            [](const ConvertedFramePtr& f, int64_t p) { return f->pts < p; });
    }

    mutable std::mutex m_mutex;
    std::vector<ConvertedFramePtr> m_frames;
};
