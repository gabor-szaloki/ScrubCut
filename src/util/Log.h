#pragma once

#include <cstdio>
#include <filesystem>
#include "util/AppPaths.h"

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

#define LOG_INFO(fmt, ...) do { \
    std::fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__); \
    if (LogFile::Get().File()) { std::fprintf(LogFile::Get().File(), "[INFO]  " fmt "\n", ##__VA_ARGS__); std::fflush(LogFile::Get().File()); } \
} while (0)

#define LOG_WARN(fmt, ...) do { \
    std::fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__); \
    if (LogFile::Get().File()) { std::fprintf(LogFile::Get().File(), "[WARN]  " fmt "\n", ##__VA_ARGS__); std::fflush(LogFile::Get().File()); } \
} while (0)

#define LOG_ERROR(fmt, ...) do { \
    std::fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
    if (LogFile::Get().File()) { std::fprintf(LogFile::Get().File(), "[ERROR] " fmt "\n", ##__VA_ARGS__); std::fflush(LogFile::Get().File()); } \
} while (0)
