#pragma once

#include "util/Log.h"
#include "util/Types.h"
#include <vector>
#include <optional>
#include <algorithm>
#include <cstdio>
#include <cstdint>

class SegmentManager {
public:
    int AddSegment(double startSec, double endSec) {
        if (startSec > endSec) std::swap(startSec, endSec);
        int colorIndex = m_nextNameIndex;
        char nameBuf[8];
        snprintf(nameBuf, sizeof(nameBuf), "%03d", colorIndex + 1);
        m_segments.push_back({startSec, endSec, ExportMode::SourceFormat, 1.0, true, nameBuf, colorIndex, m_nextAddSeq++});
        m_nextNameIndex++;
        int idx = static_cast<int>(m_segments.size()) - 1;
        LOG_INFO("AddSegment idx=%d t=%.3f-%.3fs name=%s", idx, startSec, endSec, nameBuf);
        return idx;
    }

    int AddFrame(double timeSec) {
        int colorIndex = m_nextNameIndex;
        char nameBuf[8];
        snprintf(nameBuf, sizeof(nameBuf), "%03d", colorIndex + 1);
        m_frames.push_back({timeSec, nameBuf, colorIndex, m_nextAddSeq++});
        m_nextNameIndex++;
        int idx = static_cast<int>(m_frames.size()) - 1;
        LOG_INFO("AddFrame   idx=%d t=%.3fs name=%s", idx, timeSec, nameBuf);
        return idx;
    }

    void RemoveSegment(int index) {
        if (index >= 0 && index < static_cast<int>(m_segments.size()))
            m_segments.erase(m_segments.begin() + index);
    }

    void RemoveFrame(int index) {
        if (index >= 0 && index < static_cast<int>(m_frames.size()))
            m_frames.erase(m_frames.begin() + index);
    }

    // Removes the most-recently-added mark of either type. No-op if empty.
    void RemoveLastMark() {
        int lastSegIdx = -1;
        int lastFrameIdx = -1;
        uint64_t segSeq = 0, frameSeq = 0;
        for (int i = 0; i < static_cast<int>(m_segments.size()); ++i) {
            if (lastSegIdx < 0 || m_segments[i].addSeq > segSeq) {
                segSeq = m_segments[i].addSeq;
                lastSegIdx = i;
            }
        }
        for (int i = 0; i < static_cast<int>(m_frames.size()); ++i) {
            if (lastFrameIdx < 0 || m_frames[i].addSeq > frameSeq) {
                frameSeq = m_frames[i].addSeq;
                lastFrameIdx = i;
            }
        }
        if (lastSegIdx < 0 && lastFrameIdx < 0) return;
        if (lastFrameIdx < 0 || (lastSegIdx >= 0 && segSeq > frameSeq)) {
            LOG_INFO("RemoveLastMark kind=Segment idx=%d", lastSegIdx);
            m_segments.erase(m_segments.begin() + lastSegIdx);
        } else {
            LOG_INFO("RemoveLastMark kind=Frame idx=%d", lastFrameIdx);
            m_frames.erase(m_frames.begin() + lastFrameIdx);
        }
    }

    void UpdateSegment(int index, double startSec, double endSec) {
        if (index >= 0 && index < static_cast<int>(m_segments.size())) {
            if (startSec > endSec) std::swap(startSec, endSec);
            m_segments[index].startSec = startSec;
            m_segments[index].endSec = endSec;
        }
    }

    void UpdateFrame(int index, double timeSec) {
        if (index >= 0 && index < static_cast<int>(m_frames.size()))
            m_frames[index].timeSec = timeSec;
    }

    void SetSegmentMode(int index, ExportMode mode) {
        if (index >= 0 && index < static_cast<int>(m_segments.size()))
            m_segments[index].mode = mode;
    }

    void SetSegmentSpeed(int index, double speed) {
        if (index >= 0 && index < static_cast<int>(m_segments.size()))
            m_segments[index].speed = speed;
    }

    void SetSegmentKeepAudio(int index, bool keepAudio) {
        if (index >= 0 && index < static_cast<int>(m_segments.size()))
            m_segments[index].keepAudio = keepAudio;
    }

    void SetSegmentName(int index, const std::string& name) {
        if (index >= 0 && index < static_cast<int>(m_segments.size()))
            m_segments[index].name = name;
    }

    void SetFrameName(int index, const std::string& name) {
        if (index >= 0 && index < static_cast<int>(m_frames.size()))
            m_frames[index].name = name;
    }

    void Clear() {
        m_segments.clear();
        m_frames.clear();
        m_pendingMarkIn.reset();
        m_nextNameIndex = 0;
        m_nextAddSeq = 0;
    }

    // Mark-in/mark-out workflow (segments only)
    void SetMarkIn(double sec) {
        m_pendingMarkIn = sec;
    }

    void SetMarkOut(double sec) {
        if (m_pendingMarkIn.has_value()) {
            AddSegment(m_pendingMarkIn.value(), sec);
            m_pendingMarkIn.reset();
        }
    }

    void ClearPendingMark() { m_pendingMarkIn.reset(); }
    bool HasPendingMarkIn() const { return m_pendingMarkIn.has_value(); }
    double GetPendingMarkIn() const { return m_pendingMarkIn.value_or(0.0); }

    const std::vector<TimeRange>& GetSegments() const { return m_segments; }
    const std::vector<FrameMark>& GetFrames() const { return m_frames; }
    int GetCount() const { return static_cast<int>(m_segments.size()); }
    int GetFrameCount() const { return static_cast<int>(m_frames.size()); }
    int GetTotalCount() const { return GetCount() + GetFrameCount(); }

private:
    std::vector<TimeRange> m_segments;
    std::vector<FrameMark> m_frames;
    std::optional<double> m_pendingMarkIn;
    int m_nextNameIndex = 0;
    uint64_t m_nextAddSeq = 0;
};
