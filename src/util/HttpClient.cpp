#include "util/HttpClient.h"

// macOS implementation: HttpClient.mm.
#ifdef _WIN32

#include "scrubcut_version.h"

#include <windows.h>
#include <winhttp.h>

#include <iterator>
#include <vector>

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const wchar_t* w, int len) {
    if (len <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}

// "<what>: <message> (code)"; WinHTTP's own messages live in winhttp.dll.
std::string DescribeError(const char* what, DWORD err) {
    wchar_t buf[512] = {};
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandleW(L"winhttp.dll"), err, 0, buf, static_cast<DWORD>(std::size(buf)), nullptr);
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L'.' || buf[n - 1] == L' '))
        --n;
    std::string msg = Narrow(buf, static_cast<int>(n));
    std::string out = what;
    out += ": ";
    out += msg.empty() ? "error" : msg;
    out += " (" + std::to_string(err) + ")";
    return out;
}

struct HandleCloser {
    HINTERNET h = nullptr;
    ~HandleCloser() { if (h) WinHttpCloseHandle(h); }
};

} // namespace

bool HttpGet(const std::string& url, std::string& outBody, std::string& outError, int timeoutMs) {
    outBody.clear();
    outError.clear();

    std::wstring wurl = Widen(url);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;      // includes the query string
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        outError = DescribeError("invalid URL", GetLastError());
        return false;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HandleCloser session;
    const std::wstring userAgent = Widen(std::string("ScrubCut/") + SCRUBCUT_VERSION);
    session.h = WinHttpOpen(userAgent.c_str(),
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        outError = DescribeError("WinHttpOpen", GetLastError());
        return false;
    }
    // Per-phase timeouts; the read loop below also caps the total.
    WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HandleCloser connect;
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    if (!connect.h) {
        outError = DescribeError("WinHttpConnect", GetLastError());
        return false;
    }

    HandleCloser request;
    request.h = WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) {
        outError = DescribeError("WinHttpOpenRequest", GetLastError());
        return false;
    }

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        outError = DescribeError("request failed", GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) {
        outError = DescribeError("no response", GetLastError());
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        outError = DescribeError("missing status", GetLastError());
        return false;
    }

    constexpr size_t kMaxBody = 4u << 20;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    std::vector<char> chunk;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) {
            outError = DescribeError("read failed", GetLastError());
            return false;
        }
        if (avail == 0) break;
        chunk.resize(avail);
        DWORD got = 0;
        if (!WinHttpReadData(request.h, chunk.data(), avail, &got)) {
            outError = DescribeError("read failed", GetLastError());
            return false;
        }
        outBody.append(chunk.data(), got);
        if (outBody.size() > kMaxBody) {
            outError = "response too large";
            return false;
        }
        if (GetTickCount64() > deadline) {
            outError = "timed out";
            return false;
        }
    }

    if (status < 200 || status >= 300) {
        outError = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

#elif !defined(__APPLE__)

bool HttpGet(const std::string&, std::string& outBody, std::string& outError, int) {
    outBody.clear();
    outError = "not supported on this platform";
    return false;
}

#endif
