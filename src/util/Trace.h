#pragma once

#include <cstdio>
#include <cstdint>
#include <SDL3/SDL.h>

// Lightweight trace logger. Writes timestamped events to a file.
// Usage: TRACE_EVENT("name") at the start of a scope — logs begin+end with duration.
// Usage: TRACE_LOG("name") for a single instant event.

class TraceFile {
public:
    static TraceFile& Get() {
        static TraceFile instance;
        return instance;
    }

    void Open(const char* path) {
        if (m_file) std::fclose(m_file);
        m_file = std::fopen(path, "w");
        if (m_file) {
            std::fprintf(m_file, "timestamp_us,duration_us,event\n");
            m_startNS = SDL_GetTicksNS();
        }
    }

    void Close() {
        if (m_file) { std::fclose(m_file); m_file = nullptr; }
    }

    void LogInstant(const char* name) {
        if (!m_file) return;
        uint64_t now = SDL_GetTicksNS() - m_startNS;
        std::fprintf(m_file, "%llu,0,%s\n",
                     static_cast<unsigned long long>(now / 1000), name);
    }

    void LogDuration(const char* name, uint64_t startNS) {
        if (!m_file) return;
        uint64_t endNS = SDL_GetTicksNS();
        uint64_t relStart = startNS - m_startNS;
        uint64_t dur = endNS - startNS;
        std::fprintf(m_file, "%llu,%llu,%s\n",
                     static_cast<unsigned long long>(relStart / 1000),
                     static_cast<unsigned long long>(dur / 1000),
                     name);
        std::fflush(m_file);
    }

    bool IsOpen() const { return m_file != nullptr; }

private:
    TraceFile() = default;
    ~TraceFile() { Close(); }
    std::FILE* m_file = nullptr;
    uint64_t m_startNS = 0;
};

struct ScopedTrace {
    const char* name;
    uint64_t startNS;
    ScopedTrace(const char* n) : name(n), startNS(SDL_GetTicksNS()) {}
    ~ScopedTrace() { TraceFile::Get().LogDuration(name, startNS); }
};

#define TRACE_EVENT(name) ScopedTrace _trace_##__LINE__(name)
#define TRACE_LOG(name) TraceFile::Get().LogInstant(name)
