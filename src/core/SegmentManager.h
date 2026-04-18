#pragma once

#include "util/Types.h"
#include <vector>
#include <optional>
#include <algorithm>
#include <cstdio>

class SegmentManager {
public:
    int AddSegment(double startSec, double endSec) {
        if (startSec > endSec) std::swap(startSec, endSec);
        char nameBuf[8];
        snprintf(nameBuf, sizeof(nameBuf), "%03d", m_nextColorIndex + 1);
        m_segments.push_back({startSec, endSec, ExportMode::SourceFormat, nameBuf, m_nextColorIndex});
        m_nextColorIndex++;
        return static_cast<int>(m_segments.size()) - 1;
    }

    void RemoveSegment(int index) {
        if (index >= 0 && index < static_cast<int>(m_segments.size()))
            m_segments.erase(m_segments.begin() + index);
    }

    void UpdateSegment(int index, double startSec, double endSec) {
        if (index >= 0 && index < static_cast<int>(m_segments.size())) {
            if (startSec > endSec) std::swap(startSec, endSec);
            m_segments[index].startSec = startSec;
            m_segments[index].endSec = endSec;
        }
    }

    void SetSegmentMode(int index, ExportMode mode) {
        if (index >= 0 && index < static_cast<int>(m_segments.size()))
            m_segments[index].mode = mode;
    }

    void SetSegmentName(int index, const std::string& name) {
        if (index >= 0 && index < static_cast<int>(m_segments.size()))
            m_segments[index].name = name;
    }

    void Clear() {
        m_segments.clear();
        m_pendingMarkIn.reset();
        m_nextColorIndex = 0;
    }

    // Mark-in/mark-out workflow
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
    int GetCount() const { return static_cast<int>(m_segments.size()); }

private:
    std::vector<TimeRange> m_segments;
    std::optional<double> m_pendingMarkIn;
    int m_nextColorIndex = 0;
};
