#include "core/OcrCache.h"
#include "util/AppPaths.h"
#include "util/Log.h"

#include <tesseract/baseapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace OcrCache {

namespace {

// Header tag that must match exactly for a cache hit. Bumping this string
// invalidates every existing cache file in one shot — useful when changing
// the indexer's sampling/dedup logic in a way that affects results.
// v4: switched to the tessdata_fast (integer LSTM) model — OCR output
// differs from the standard model enough that prior caches are stale.
constexpr const char* kSchemaTag = "v4";
constexpr int kSampleFps = 1;
constexpr int kFpDim = 16;

uint64_t Hash64(const std::string& s) {
    // FNV-1a 64. Fingerprint is hashed across (path + size + mtime); collisions
    // are tolerable because we validate the full source metadata on Load.
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

struct SourceFingerprint {
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
};

bool GetSourceFingerprint(const std::string& videoPath, SourceFingerprint& fp) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(videoPath, ec);
    if (ec) return false;
    auto t = std::filesystem::last_write_time(videoPath, ec);
    if (ec) return false;
    fp.size = sz;
    fp.mtime = static_cast<std::int64_t>(t.time_since_epoch().count());
    return true;
}

}  // namespace

std::filesystem::path FileFor(const std::string& videoPath) {
    SourceFingerprint fp;
    GetSourceFingerprint(videoPath, fp);
    std::string keyStr = videoPath + "|" + std::to_string(fp.size) + "|" + std::to_string(fp.mtime);
    uint64_t h = Hash64(keyStr);
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.txt", static_cast<unsigned long long>(h));
    return GetAppDataDir() / "index" / name;
}

bool Load(const std::string& videoPath, std::vector<TextOccurrence>& out) {
    auto path = FileFor(videoPath);
    std::ifstream in(path);
    if (!in.is_open()) return false;

    SourceFingerprint expectedFp;
    if (!GetSourceFingerprint(videoPath, expectedFp)) return false;

    std::string schemaTag, engine, sourcePath;
    int sampleFps = 0, fpDim = 0;
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
    std::vector<TextOccurrence> occs;
    TextOccurrence current;
    bool inOcc = false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "SCHEMA") {
            ss >> schemaTag;
        } else if (tag == "ENGINE") {
            ss >> engine;
        } else if (tag == "SAMPLE_FPS") {
            ss >> sampleFps;
        } else if (tag == "FP_DIM") {
            ss >> fpDim;
        } else if (tag == "SRC_SIZE") {
            ss >> size;
        } else if (tag == "SRC_MTIME") {
            ss >> mtime;
        } else if (tag == "SRC_PATH") {
            // Rest of the line (path may contain spaces).
            std::getline(ss, sourcePath);
            if (!sourcePath.empty() && sourcePath[0] == ' ')
                sourcePath.erase(0, 1);
        } else if (tag == "OCC") {
            if (inOcc) occs.push_back(std::move(current));
            current = {};
            inOcc = true;
            ss >> current.startSec >> current.endSec;
        } else if (tag == "WORD") {
            if (!inOcc) continue;
            TextSpan w;
            ss >> w.bx >> w.by >> w.bw >> w.bh;
            // Rest is the text; preserve internal spaces (rare in a single
            // OCR'd word, but harmless to keep).
            std::string text;
            std::getline(ss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            w.text = std::move(text);
            current.words.push_back(std::move(w));
        } else if (tag == "END") {
            break;
        }
    }
    if (inOcc) occs.push_back(std::move(current));

    if (schemaTag != kSchemaTag) {
        LOG_INFO("OCR cache: schema mismatch (have=%s want=%s) — re-indexing",
                 schemaTag.c_str(), kSchemaTag);
        return false;
    }
    if (engine != tesseract::TessBaseAPI::Version()) {
        LOG_INFO("OCR cache: engine version mismatch — re-indexing");
        return false;
    }
    if (sampleFps != kSampleFps || fpDim != kFpDim) {
        LOG_INFO("OCR cache: indexer config drift — re-indexing");
        return false;
    }
    if (size != expectedFp.size || mtime != expectedFp.mtime) {
        LOG_INFO("OCR cache: source file changed — re-indexing");
        return false;
    }
    LOG_INFO("OCR cache: hit (%zu occurrences) %s",
             occs.size(), path.string().c_str());
    out = std::move(occs);

    // Touch the cache file's mtime so it stays warm in the LRU on next eviction.
    std::error_code ec;
    auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(path, now, ec);
    return true;
}

void Save(const std::string& videoPath,
          const std::vector<TextOccurrence>& occs) {
    SourceFingerprint fp;
    if (!GetSourceFingerprint(videoPath, fp)) {
        LOG_WARN("OCR cache: cannot fingerprint source, not saving");
        return;
    }
    auto path = FileFor(videoPath);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN("OCR cache: failed to open %s for write", path.string().c_str());
        return;
    }
    out << "SCHEMA " << kSchemaTag << "\n";
    out << "ENGINE " << tesseract::TessBaseAPI::Version() << "\n";
    out << "SAMPLE_FPS " << kSampleFps << "\n";
    out << "FP_DIM " << kFpDim << "\n";
    out << "SRC_PATH " << videoPath << "\n";
    out << "SRC_SIZE " << fp.size << "\n";
    out << "SRC_MTIME " << fp.mtime << "\n";
    for (const auto& occ : occs) {
        out << "OCC " << occ.startSec << " " << occ.endSec << "\n";
        for (const auto& w : occ.words) {
            out << "WORD " << w.bx << " " << w.by << " " << w.bw << " " << w.bh
                << " " << w.text << "\n";
        }
    }
    out << "END\n";
    out.close();
    LOG_INFO("OCR cache: wrote %zu occurrences to %s",
             occs.size(), path.string().c_str());
}

void EvictLRU(int keep) {
    auto dir = GetAppDataDir() / "index";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;
    struct Entry { std::filesystem::path path; std::filesystem::file_time_type mtime; };
    std::vector<Entry> entries;
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".txt") continue;
        entries.push_back({ de.path(), de.last_write_time(ec) });
    }
    if (static_cast<int>(entries.size()) <= keep) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });
    for (int i = keep; i < static_cast<int>(entries.size()); i++) {
        std::filesystem::remove(entries[i].path, ec);
    }
}

}  // namespace OcrCache
