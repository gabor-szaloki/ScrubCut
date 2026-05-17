#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace lev {

// Lowercase + collapse internal whitespace runs into a single space, trim
// ends. Used to normalize OCR output before fuzzy comparison so trivial
// differences (case, double spaces) don't cost edit distance.
inline std::string Normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = true;  // suppress leading spaces
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc)) {
            if (!prevSpace) {
                out.push_back(' ');
                prevSpace = true;
            }
        } else {
            out.push_back(static_cast<char>(std::tolower(uc)));
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Classic Levenshtein edit distance, two-row DP.
inline int Distance(const std::string& a, const std::string& b) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<int> prev(m + 1), curr(m + 1);
    for (int j = 0; j <= m; j++) prev[j] = j;
    for (int i = 1; i <= n; i++) {
        curr[0] = i;
        for (int j = 1; j <= m; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({ prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

// Similarity in [0,1]; 1.0 means identical (after normalization).
inline double Similarity(const std::string& a, const std::string& b) {
    auto na = Normalize(a);
    auto nb = Normalize(b);
    int maxLen = std::max<int>(na.size(), nb.size());
    if (maxLen == 0) return 1.0;
    int d = Distance(na, nb);
    return 1.0 - static_cast<double>(d) / static_cast<double>(maxLen);
}

// Returns true if `needle` occurs (fuzzily) anywhere in `haystack`. Slides
// a window of needle's length over haystack and compares each window with
// edit distance, allowing up to ceil((1 - threshold) * needle.length) edits.
// Used for in-text search where the query is small relative to the line.
inline bool ContainsFuzzy(const std::string& haystack,
                          const std::string& needle,
                          double threshold = 0.7) {
    auto h = Normalize(haystack);
    auto n = Normalize(needle);
    if (n.empty()) return true;
    if (h.empty()) return false;
    const int nlen = static_cast<int>(n.size());
    const int hlen = static_cast<int>(h.size());
    int maxEdits = static_cast<int>((1.0 - threshold) * nlen + 0.999);
    if (maxEdits < 0) maxEdits = 0;
    // Quick exact-substring fast path (very common).
    if (h.find(n) != std::string::npos) return true;
    // Slide a window the size of the needle.
    int winLen = std::max(nlen - maxEdits, 1);
    for (int i = 0; i + winLen <= hlen; i++) {
        int len = std::min(nlen + maxEdits, hlen - i);
        std::string sub = h.substr(i, len);
        if (Distance(sub, n) <= maxEdits) return true;
    }
    return false;
}

}  // namespace lev
