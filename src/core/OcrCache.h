#pragma once

#include "util/Types.h"

#include <filesystem>
#include <string>
#include <vector>

// On-disk cache for OCR indexes. Each indexed video gets a single file at
// `GetAppDataDir() / "index" / <fingerprint>.txt` containing a header (engine
// version, source size+mtime, sampling config) plus the occurrence list.
// Format is a hand-rolled line-oriented text format — small, debuggable,
// and avoids pulling in a JSON dependency for ~10 fields.
namespace OcrCache {

// Path the cache file *would* live at for this video (independent of whether
// it currently exists). Always under `GetAppDataDir()/"index"`.
std::filesystem::path FileFor(const std::string& videoPath);

// Try to load a cached index for this video. Returns true and populates
// `out` only when the file exists AND its header matches the current
// (engine, video-fingerprint, sampling-config). Otherwise returns false and
// leaves `out` untouched — caller should fall back to re-indexing.
bool Load(const std::string& videoPath,
          std::vector<TextOccurrence>& out);

// Persist `occs` to disk for this video. Best-effort: errors are logged
// but not surfaced (the runtime index is still valid in memory).
void Save(const std::string& videoPath,
          const std::vector<TextOccurrence>& occs);

// Trim the index directory to at most `keep` files (oldest mtime evicted
// first). Called after Save so the dir doesn't grow without bound across
// many opened videos.
void EvictLRU(int keep);

}  // namespace OcrCache
