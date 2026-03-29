#pragma once

#include "util/FFmpegUtils.h"
#include <vector>
#include <cstring>

// Fixed-size ring buffer of decoded RGBA frames, indexed by PTS.
// Used for instant backward stepping without re-seeking.
class FrameCache {
public:
    explicit FrameCache(int capacity = 64) : m_capacity(capacity) {
        m_entries.resize(capacity);
    }

    struct Entry {
        int64_t pts = AV_NOPTS_VALUE;
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
    };

    // Store a frame. Overwrites oldest entry if full.
    void Put(int64_t pts, const uint8_t* rgba, int width, int height) {
        Entry& e = m_entries[m_writeIdx % m_capacity];
        e.pts = pts;
        e.width = width;
        e.height = height;
        int size = width * height * 4;
        e.rgba.resize(size);
        std::memcpy(e.rgba.data(), rgba, size);
        m_writeIdx++;
        if (m_count < m_capacity) m_count++;
    }

    // Find the frame with the largest PTS that is < the given PTS.
    // Returns nullptr if not found.
    const Entry* FindBefore(int64_t pts) const {
        const Entry* best = nullptr;
        for (int i = 0; i < m_count; i++) {
            int idx = ((m_writeIdx - 1 - i) % m_capacity + m_capacity) % m_capacity;
            const Entry& e = m_entries[idx];
            if (e.pts != AV_NOPTS_VALUE && e.pts < pts) {
                if (!best || e.pts > best->pts) {
                    best = &e;
                }
            }
        }
        return best;
    }

    // Find the frame closest to the given PTS.
    const Entry* FindNearest(int64_t targetPts) const {
        const Entry* best = nullptr;
        int64_t bestDist = INT64_MAX;
        for (int i = 0; i < m_count; i++) {
            int idx = ((m_writeIdx - 1 - i) % m_capacity + m_capacity) % m_capacity;
            const Entry& e = m_entries[idx];
            if (e.pts == AV_NOPTS_VALUE) continue;
            int64_t dist = std::abs(e.pts - targetPts);
            if (dist < bestDist) {
                bestDist = dist;
                best = &e;
            }
        }
        return best;
    }

    // Find exact PTS match.
    const Entry* FindExact(int64_t pts) const {
        for (int i = 0; i < m_count; i++) {
            int idx = ((m_writeIdx - 1 - i) % m_capacity + m_capacity) % m_capacity;
            const Entry& e = m_entries[idx];
            if (e.pts == pts) return &e;
        }
        return nullptr;
    }

    void Clear() {
        m_writeIdx = 0;
        m_count = 0;
        for (auto& e : m_entries) e.pts = AV_NOPTS_VALUE;
    }

    int Count() const { return m_count; }

private:
    std::vector<Entry> m_entries;
    int m_capacity;
    int m_writeIdx = 0;
    int m_count = 0;
};
