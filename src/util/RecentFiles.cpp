#include "util/RecentFiles.h"
#include "util/Log.h"
#include "util/Profiler.h"

#include <algorithm>
#include <fstream>
#include <locale>
#include <sstream>
#include <utility>

// File format (UTF-8, INI-like; blank and '#' lines ignored):
//
//   [file]
//   path=/videos/clip.mp4
//   position=12.5
//   paused=1
//   [segment]
//   start=4.725
//   end=7.202
//   name=Intro
//   [frame]
//   time=8.169
//   ...
//
// [segment] / [frame] belong to the preceding [file]. Keys may come in any
// order; unknown keys are skipped and missing ones keep the default, so
// adding a property is one row in the field tables below. Values are
// backslash-escaped; numbers use the classic locale.

namespace {

std::string Escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default:   out += c; break;
        }
    }
    return out;
}

std::string Unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            out += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

template <class T>
std::string FormatNum(T v) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(17);
    out << v;
    return out.str();
}

// Leaves `v` untouched (i.e. at its default) if `s` doesn't parse.
template <class T>
void ParseNum(const std::string& s, T& v) {
    std::istringstream in(s);
    in.imbue(std::locale::classic());
    T parsed{};
    if (in >> parsed) v = parsed;
}

// One persisted property: its key, how to format it, how to parse it back.
template <class T>
struct Field {
    const char* key;
    std::string (*get)(const T&);
    void (*set)(T&, const std::string&);
};

const Field<RecentEntry> kFileFields[] = {
    {"path",     [](const RecentEntry& e) { return e.path; },
                 [](RecentEntry& e, const std::string& v) { e.path = v; }},
    {"position", [](const RecentEntry& e) { return FormatNum(e.positionSec); },
                 [](RecentEntry& e, const std::string& v) { ParseNum(v, e.positionSec); }},
    {"paused",   [](const RecentEntry& e) { return std::string(e.paused ? "1" : "0"); },
                 [](RecentEntry& e, const std::string& v) { e.paused = v != "0"; }},
};

const Field<TimeRange> kSegmentFields[] = {
    {"start", [](const TimeRange& r) { return FormatNum(r.startSec); },
              [](TimeRange& r, const std::string& v) { ParseNum(v, r.startSec); }},
    {"end",   [](const TimeRange& r) { return FormatNum(r.endSec); },
              [](TimeRange& r, const std::string& v) { ParseNum(v, r.endSec); }},
    {"name",  [](const TimeRange& r) { return r.name; },
              [](TimeRange& r, const std::string& v) { r.name = v; }},
    {"color", [](const TimeRange& r) { return FormatNum(r.colorIndex); },
              [](TimeRange& r, const std::string& v) { ParseNum(v, r.colorIndex); }},
    {"seq",   [](const TimeRange& r) { return FormatNum(r.addSeq); },
              [](TimeRange& r, const std::string& v) { ParseNum(v, r.addSeq); }},
    {"format", [](const TimeRange& r) { return std::string(r.mode == ExportMode::GIF ? "gif" : "source"); },
               [](TimeRange& r, const std::string& v) {
                   r.mode = v == "gif" ? ExportMode::GIF : ExportMode::SourceFormat; }},
    {"speed", [](const TimeRange& r) { return FormatNum(r.speed); },
              [](TimeRange& r, const std::string& v) { ParseNum(v, r.speed); }},
    {"audio", [](const TimeRange& r) { return std::string(r.keepAudio ? "1" : "0"); },
              [](TimeRange& r, const std::string& v) { r.keepAudio = v != "0"; }},
};

const Field<FrameMark> kFrameFields[] = {
    {"time",  [](const FrameMark& m) { return FormatNum(m.timeSec); },
              [](FrameMark& m, const std::string& v) { ParseNum(v, m.timeSec); }},
    {"name",  [](const FrameMark& m) { return m.name; },
              [](FrameMark& m, const std::string& v) { m.name = v; }},
    {"color", [](const FrameMark& m) { return FormatNum(m.colorIndex); },
              [](FrameMark& m, const std::string& v) { ParseNum(v, m.colorIndex); }},
    {"seq",   [](const FrameMark& m) { return FormatNum(m.addSeq); },
              [](FrameMark& m, const std::string& v) { ParseNum(v, m.addSeq); }},
};

template <class T, size_t N>
void WriteFields(std::ostream& out, const Field<T> (&fields)[N], const T& obj) {
    for (const auto& f : fields)
        out << f.key << '=' << Escape(f.get(obj)) << '\n';
}

template <class T, size_t N>
void ReadField(const Field<T> (&fields)[N], T& obj, const std::string& key, const std::string& value) {
    for (const auto& f : fields)
        if (key == f.key) { f.set(obj, value); return; }
}

// Entry list operations, shared by the UI-side copy and the worker. Each is
// idempotent, so re-applying an already-written op is harmless (see Refresh).
void Trim(std::vector<RecentEntry>& entries, int max) {
    if (static_cast<int>(entries.size()) > max)
        entries.resize(max);
}

std::vector<RecentEntry>::iterator FindEntry(std::vector<RecentEntry>& entries,
                                             const std::string& path) {
    return std::find_if(entries.begin(), entries.end(),
                        [&](const RecentEntry& e) { return e.path == path; });
}

void TouchEntry(std::vector<RecentEntry>& entries, const std::string& path, int max) {
    auto it = FindEntry(entries, path);
    if (it == entries.begin() && it != entries.end()) return;
    RecentEntry entry;
    if (it != entries.end()) {
        entry = std::move(*it);
        entries.erase(it);
    } else {
        entry.path = path;
    }
    entries.insert(entries.begin(), std::move(entry));
    Trim(entries, max);
}

// Missing file → empty list.
std::vector<RecentEntry> ReadEntries(const std::filesystem::path& path, int max) {
    PROFILE_SCOPE();
    std::vector<RecentEntry> entries;
    std::ifstream f(path);
    if (!f.is_open()) return entries;

    enum class Section { None, Unknown, File, Segment, Frame } section = Section::None;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            const std::string name = line.substr(1, line.size() - 2);
            if (name == "file") {
                entries.emplace_back();
                section = Section::File;
            } else if (name == "segment" && !entries.empty()) {
                entries.back().segments.emplace_back();
                section = Section::Segment;
            } else if (name == "frame" && !entries.empty()) {
                entries.back().frames.emplace_back();
                section = Section::Frame;
            } else {
                section = Section::Unknown;  // skip its keys
            }
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = Unescape(line.substr(eq + 1));
        switch (section) {
            case Section::File:
                ReadField(kFileFields, entries.back(), key, value);
                break;
            case Section::Segment:
                ReadField(kSegmentFields, entries.back().segments.back(), key, value);
                break;
            case Section::Frame:
                ReadField(kFrameFields, entries.back().frames.back(), key, value);
                break;
            default:
                break;
        }
    }

    // Drop entries without a path; sanitize values the app relies on.
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const RecentEntry& e) { return e.path.empty(); }),
                  entries.end());
    for (auto& e : entries) {
        for (auto& r : e.segments) {
            if (r.startSec > r.endSec) std::swap(r.startSec, r.endSec);
            if (!(r.speed > 0.0)) r.speed = 1.0;
            if (r.colorIndex < 0) r.colorIndex = 0;
        }
        for (auto& m : e.frames)
            if (m.colorIndex < 0) m.colorIndex = 0;
    }
    Trim(entries, max);
    return entries;
}

// Temp file + rename, so a crash mid-write can't truncate the file.
void WriteEntries(const std::filesystem::path& path, const std::vector<RecentEntry>& entries) {
    PROFILE_SCOPE();
    std::ostringstream out;
    {
        PROFILE_SCOPE_N("Serialize");
        out << "# ScrubCut recent files and their marks\n";
        for (const auto& e : entries) {
            out << "\n[file]\n";
            WriteFields(out, kFileFields, e);
            for (const auto& s : e.segments) {
                out << "[segment]\n";
                WriteFields(out, kSegmentFields, s);
            }
            for (const auto& m : e.frames) {
                out << "[frame]\n";
                WriteFields(out, kFrameFields, m);
            }
        }
    }

    PROFILE_SCOPE_N("Write + rename");
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            LOG_WARN("RecentFiles: can't write %s", tmp.string().c_str());
            return;
        }
        f << out.str();
        if (!f) {
            LOG_WARN("RecentFiles: write failed for %s", tmp.string().c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) LOG_WARN("RecentFiles: rename failed: %s", ec.message().c_str());
}

} // namespace

RecentFiles::~RecentFiles() {
    Stop();
}

bool RecentFiles::Load(const std::filesystem::path& path) {
    Stop();
    m_path = path;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    Refresh();
    m_stop = false;
    m_thread = std::thread(&RecentFiles::Worker, this);
    return exists;
}

void RecentFiles::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable()) m_thread.join();
}

void RecentFiles::Refresh() {
    PROFILE_SCOPE();
    if (m_path.empty()) return;
    // Snapshot the queue *before* reading: ops the worker writes in between
    // are then both in the file and re-applied, which is harmless.
    std::vector<Op> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending = m_queue;
    }
    m_entries = ReadEntries(m_path, m_max);
    for (const Op& op : pending) op(m_entries);
}

void RecentFiles::Apply(Op op) {
    op(m_entries);
    if (m_path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(op));
    }
    m_cv.notify_one();
}

void RecentFiles::Worker() {
    PROFILE_THREAD("Recents Save");
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
        if (m_queue.empty()) return;  // stopping, all written
        // Ops stay queued until written so Refresh can re-apply them.
        const std::vector<Op> batch = m_queue;
        lock.unlock();
        {
            PROFILE_SCOPE_N("Save recents");
            std::vector<RecentEntry> entries = ReadEntries(m_path, kMaxMax);
            for (const Op& op : batch) op(entries);
            WriteEntries(m_path, entries);
        }
        lock.lock();
        m_queue.erase(m_queue.begin(), m_queue.begin() + batch.size());
    }
}

const RecentEntry* RecentFiles::Find(const std::string& path) const {
    for (const auto& e : m_entries)
        if (e.path == path) return &e;
    return nullptr;
}

void RecentFiles::Touch(const std::string& path) {
    Apply([path, max = m_max](std::vector<RecentEntry>& entries) {
        TouchEntry(entries, path, max);
    });
}

void RecentFiles::SetMarks(const std::string& path, const std::vector<TimeRange>& segments,
                           const std::vector<FrameMark>& frames) {
    Apply([path, segments, frames, max = m_max](std::vector<RecentEntry>& entries) {
        auto it = FindEntry(entries, path);
        if (it == entries.end()) {
            if (segments.empty() && frames.empty()) return;
            TouchEntry(entries, path, max);
            it = entries.begin();
        }
        it->segments = segments;
        it->frames = frames;
    });
}

void RecentFiles::SetPlayhead(const std::string& path, double positionSec, bool paused) {
    Apply([path, positionSec, paused](std::vector<RecentEntry>& entries) {
        auto it = FindEntry(entries, path);
        if (it == entries.end()) return;
        it->positionSec = positionSec;
        it->paused = paused;
    });
}

void RecentFiles::Clear() {
    Apply([](std::vector<RecentEntry>& entries) { entries.clear(); });
}

void RecentFiles::SetMax(int max) {
    m_max = std::clamp(max, kMinMax, kMaxMax);
    Apply([max = m_max](std::vector<RecentEntry>& entries) { Trim(entries, max); });
}
