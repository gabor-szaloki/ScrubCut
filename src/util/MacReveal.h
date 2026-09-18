#pragma once

#ifdef __APPLE__

#include <string>
#include <vector>

// Open one Finder window with all `utf8Paths` selected (non-blocking, unlike
// `open -R`, which is also single-file only). Returns false when no path
// converts to a URL — fall back to opening the folder. Windows counterpart:
// RevealFilesInShell in App.cpp.
bool MacRevealFilesInFinder(const std::vector<std::string>& utf8Paths);

#endif
