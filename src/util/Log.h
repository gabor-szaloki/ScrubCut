#pragma once

#include <cstdio>
#include <filesystem>
#include "util/AppPaths.h"
#include "util/Profiler.h"

class LogFile {
public:
    static LogFile& Get() {
        static LogFile instance;
        return instance;
    }

    void Open() {
        if (m_file) return;
        auto logDir = GetAppDataDir() / "logs";
        std::filesystem::create_directories(logDir);
        m_file = std::fopen((logDir / "scrubcut.log").string().c_str(), "w");
    }

    void Close() {
        if (m_file) { std::fclose(m_file); m_file = nullptr; }
    }

    std::FILE* File() const { return m_file; }

private:
    LogFile() = default;
    ~LogFile() { Close(); }
    std::FILE* m_file = nullptr;
};

// Mirror the log line into Tracy's message log when profiling is enabled —
// no prefix/newline (the viewer tags severity); formats only when profiling.
#define LOG_TO_PROFILER(severity, fmt, ...) do { \
    if (Profiler::IsEnabled()) { \
        char _profBuf[1024]; \
        int _profLen = std::snprintf(_profBuf, sizeof(_profBuf), fmt, ##__VA_ARGS__); \
        if (_profLen > 0) { \
            if (_profLen > static_cast<int>(sizeof(_profBuf)) - 1) _profLen = static_cast<int>(sizeof(_profBuf)) - 1; \
            Profiler::LogMessage(tracy::MessageSeverity::severity, _profBuf, static_cast<size_t>(_profLen)); \
        } \
    } \
} while (0)

#define LOG_INFO(fmt, ...) do { \
    std::fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__); \
    if (LogFile::Get().File()) { std::fprintf(LogFile::Get().File(), "[INFO]  " fmt "\n", ##__VA_ARGS__); std::fflush(LogFile::Get().File()); } \
    LOG_TO_PROFILER(Info, fmt, ##__VA_ARGS__); \
} while (0)

#define LOG_WARN(fmt, ...) do { \
    std::fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__); \
    if (LogFile::Get().File()) { std::fprintf(LogFile::Get().File(), "[WARN]  " fmt "\n", ##__VA_ARGS__); std::fflush(LogFile::Get().File()); } \
    LOG_TO_PROFILER(Warning, fmt, ##__VA_ARGS__); \
} while (0)

#define LOG_ERROR(fmt, ...) do { \
    std::fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
    if (LogFile::Get().File()) { std::fprintf(LogFile::Get().File(), "[ERROR] " fmt "\n", ##__VA_ARGS__); std::fflush(LogFile::Get().File()); } \
    LOG_TO_PROFILER(Error, fmt, ##__VA_ARGS__); \
} while (0)
