#include "util/UpdateChecker.h"
#include "util/HttpClient.h"
#include "util/Log.h"
#include "util/Profiler.h"
#include "scrubcut_version.h"

#include <cctype>
#include <cstdlib>
#include <thread>

namespace {

constexpr const char* kLatestReleaseApi =
    "https://api.github.com/repos/gabor-szaloki/ScrubCut/releases/latest";

// First `"key": "..."` string value in a JSON document — enough for GitHub's
// flat release payload. False if absent or not a string.
bool ExtractJsonString(const std::string& json, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != ':') return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return true;
        if (c == '\\' && pos < json.size()) {
            char e = json[pos++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'u': pos += 4; out += '?'; break;   // no non-ASCII expected in a tag
            default:  out += e; break;               // \" \\ \/
            }
            continue;
        }
        out += c;
    }
    return false;   // unterminated string
}

} // namespace

bool UpdateChecker::ParseVersion(const std::string& text, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    size_t pos = 0;
    while (pos < text.size() && !std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size()) return false;
    for (int i = 0; i < 3; ++i) {
        const char* begin = text.c_str() + pos;
        char* end = nullptr;
        long v = std::strtol(begin, &end, 10);
        if (end == begin) break;
        out[i] = static_cast<int>(v);
        pos = static_cast<size_t>(end - text.c_str());
        if (pos >= text.size() || text[pos] != '.') break;
        ++pos;
    }
    return true;
}

UpdateChecker::~UpdateChecker() {
    Stop();
}

void UpdateChecker::Start() {
    if (m_result && m_result->state.load(std::memory_order_acquire) == State::Checking)
        return;
    Stop();   // reap the previous (finished) worker
    m_result = std::make_shared<Result>();
    m_logged = false;
    m_thread = std::thread(&UpdateChecker::Worker, m_result);
}

void UpdateChecker::Stop() {
    if (!m_thread.joinable()) return;
    // A terminal state is the worker's last store, so the join is instant.
    // Otherwise it's blocked in the request: detach and tell the profiler.
    if (m_result->state.load(std::memory_order_acquire) != State::Checking) {
        m_thread.join();
    } else {
        LOG_INFO("Update check still in flight at shutdown; abandoning its worker");
        m_thread.detach();
        Profiler::AbandonInstrumentedThread();
    }
}

void UpdateChecker::Worker(std::shared_ptr<Result> result) {
    PROFILE_THREAD("Update Check");
    PROFILE_SCOPE();
    Profiler::ScopedSection section(Profiler::kSectionJobs, "Update check");

    std::string body, error;
    bool ok;
    {
        PROFILE_WAIT_SCOPE_N("HttpGet");
        ok = HttpGet(kLatestReleaseApi, body, error);
    }
    if (!ok) {
        result->error = error;
        result->state.store(State::Failed, std::memory_order_release);
        return;
    }

    std::string tag;
    if (!ExtractJsonString(body, "tag_name", tag) || tag.empty()) {
        result->error = "unexpected response";
        result->state.store(State::Failed, std::memory_order_release);
        return;
    }

    int latest[3], current[3];
    if (!ParseVersion(tag, latest) || !ParseVersion(SCRUBCUT_VERSION, current)) {
        result->error = "could not parse version '" + tag + "'";
        result->state.store(State::Failed, std::memory_order_release);
        return;
    }

    result->latestVersion = tag;
    if (!result->latestVersion.empty() &&
        (result->latestVersion[0] == 'v' || result->latestVersion[0] == 'V'))
        result->latestVersion.erase(0, 1);
    // Equals the payload's html_url, without depending on key order.
    result->latestUrl = std::string(kRepoUrl) + "/releases/tag/" + tag;

    // First differing component decides.
    bool newer = false;
    for (int i = 0; i < 3; ++i) {
        if (latest[i] != current[i]) {
            newer = latest[i] > current[i];
            break;
        }
    }
    result->state.store(newer ? State::UpdateAvailable : State::UpToDate,
                        std::memory_order_release);
}

void UpdateChecker::Poll() {
    if (!m_result || m_logged) return;
    switch (m_result->state.load(std::memory_order_acquire)) {
    case State::UpdateAvailable:
        LOG_INFO("Update check: v%s is available (running v%s): %s",
                 m_result->latestVersion.c_str(), SCRUBCUT_VERSION, m_result->latestUrl.c_str());
        break;
    case State::UpToDate:
        LOG_INFO("Update check: up to date (latest release v%s, running v%s)",
                 m_result->latestVersion.c_str(), SCRUBCUT_VERSION);
        break;
    case State::Failed:
        LOG_WARN("Update check failed: %s", m_result->error.c_str());
        break;
    default:
        return;   // still running — keep polling
    }
    m_logged = true;
}

UpdateChecker::State UpdateChecker::GetState() const {
    return m_result ? m_result->state.load(std::memory_order_acquire) : State::Idle;
}

std::string UpdateChecker::GetLatestVersion() const {
    State s = GetState();
    return (s == State::UpToDate || s == State::UpdateAvailable) ? m_result->latestVersion : "";
}

std::string UpdateChecker::GetLatestUrl() const {
    return GetState() == State::UpdateAvailable ? m_result->latestUrl : "";
}

std::string UpdateChecker::GetError() const {
    return GetState() == State::Failed ? m_result->error : "";
}
