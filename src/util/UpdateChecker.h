#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

// Checks GitHub for a release newer than SCRUBCUT_VERSION; drives the Help
// menu's update row and the silent check at startup.
//
// Each Start() runs one check on a fresh worker thread. Stop() (App::Shutdown,
// before Profiler::Shutdown) joins a finished worker; one still blocked in its
// request is detached and handed to the profiler instead, since joining could
// hang quit. Results live in a ref-counted block shared with the worker, so a
// detached worker can outlive the checker; each Start() allocates a new one.
//
// The worker fills the Result strings, then release-stores `state`; readers
// acquire-load `state` before touching the strings. The worker never logs (it
// may outlive static teardown); Poll() logs on the main thread.
class UpdateChecker {
public:
    enum class State { Idle, Checking, UpToDate, UpdateAvailable, Failed };

    static constexpr const char* kRepoUrl = "https://github.com/gabor-szaloki/ScrubCut";

    UpdateChecker() = default;
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    // Begin a check. No-op while one is already running.
    void Start();

    // Join a finished worker, or detach one still in flight. Idempotent.
    void Stop();

    // Main thread, once per frame: logs a finished check once.
    void Poll();

    State GetState() const;
    // Newest release, e.g. "0.10.0". Valid in UpToDate / UpdateAvailable.
    std::string GetLatestVersion() const;
    // Release page. Valid in UpdateAvailable.
    std::string GetLatestUrl() const;
    // Short reason. Valid in Failed.
    std::string GetError() const;

    // "v1.2.3-rc1" -> {1,2,3}; missing components are 0. False if no number.
    static bool ParseVersion(const std::string& text, int out[3]);

private:
    struct Result {
        std::atomic<State> state{State::Checking};
        std::string latestVersion;
        std::string latestUrl;
        std::string error;
    };
    static void Worker(std::shared_ptr<Result> result);

    std::thread m_thread;
    std::shared_ptr<Result> m_result;
    bool m_logged = false;
};
