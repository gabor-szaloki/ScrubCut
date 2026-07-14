#pragma once

#include <string>

// macOS file APIs (dialogs, drag-and-drop, argv) hand back paths in
// decomposed Unicode (NFD): "ö" arrives as "o" + combining U+0308. The ImGui
// font atlas carries precomposed glyphs but not combining marks, so NFD text
// renders with '?' in menus. Normalize to NFC (precomposed) before storing or
// displaying a path — HFS+/APFS accept either form when opening the file.
#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <cstring>

inline std::string NormalizeUtf8NFC(const std::string& s) {
    if (s.empty()) return s;
    CFStringRef cf = CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.data()),
        static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
    if (!cf) return s;
    CFMutableStringRef mut = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, cf);
    CFRelease(cf);
    if (!mut) return s;
    CFStringNormalize(mut, kCFStringNormalizationFormC);

    std::string out = s;
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(
        CFStringGetLength(mut), kCFStringEncodingUTF8) + 1;
    std::string buf(static_cast<size_t>(maxBytes), '\0');
    if (CFStringGetCString(mut, buf.data(), maxBytes, kCFStringEncodingUTF8)) {
        buf.resize(std::strlen(buf.c_str()));
        out = std::move(buf);
    }
    CFRelease(mut);
    return out;
}

#else

inline std::string NormalizeUtf8NFC(const std::string& s) { return s; }

#endif
