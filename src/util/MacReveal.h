#pragma once

#ifdef __APPLE__

#include <string>
#include <vector>

// Open one Finder window with all `utf8Paths` selected (NSWorkspace
// activateFileViewerSelectingURLs — non-blocking, unlike shelling out to
// `open -R`, which can only select a single file). Returns false when no
// path could be converted to a URL; the caller should fall back to opening
// the folder. Implemented in MacReveal.mm.
bool MacRevealFilesInFinder(const std::vector<std::string>& utf8Paths);

#endif
