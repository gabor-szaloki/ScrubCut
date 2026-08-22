#pragma once

// Instrumented profiling via the Tracy client, compiled into all builds.
// Double-gated: until profiling is enabled (-profile, or Help > Enable Tracy
// Instrumentation) the Tracy runtime doesn't exist at all
// (TRACY_MANUAL_LIFETIME) — no worker thread, no listen socket, and each
// PROFILE_* macro costs one relaxed atomic load. Once enabled, the client
// listens on localhost:8086 and records only while a viewer or tracy-capture
// is connected (TRACY_ON_DEMAND). Tools must match the client version pinned
// in vcpkg-overlays/tracy/.

#include <atomic>
#include <cstdint>

#include <tracy/Tracy.hpp>

// Tracy's FrameMark macro clobbers the FrameMark struct in util/Types.h;
// PROFILE_FRAME() calls the underlying function instead.
#ifdef FrameMark
#undef FrameMark
#endif

namespace Profiler {

// Program-phase section categories (Tracy timeline top bar). Named at
// profiler startup; the names also reach late-connecting viewers.
enum SectionCategory : uint16_t {
    kSectionMedia = 1,      // which video file is open
    kSectionTransport = 2,  // playing / paused / seeking
    kSectionJobs = 3,       // export + background scans
};

// Zone color for blocking waits, so they read as idle rather than busywork.
inline constexpr uint32_t kWaitColor = 0x404040;

inline std::atomic<bool> g_enabled{false};
inline std::atomic<bool> g_started{false};

inline bool IsEnabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

// The first enable boots the Tracy runtime; call from the main thread.
// Disabling only stops emission — the runtime stays alive until Shutdown()
// so zones still open on other threads end safely.
inline void SetEnabled(bool enabled) {
    if (enabled) {
        if (!g_started.exchange(true, std::memory_order_acq_rel)) {
            tracy::StartupProfiler();
            tracy::SetThreadName("Main");
            TracySectionSetup(kSectionMedia, "Media");
            TracySectionSetup(kSectionTransport, "Transport");
            TracySectionSetup(kSectionJobs, "Jobs");
        }
        g_enabled.store(true, std::memory_order_release);
    } else {
        g_enabled.store(false, std::memory_order_relaxed);
    }
}

// Tear down the Tracy runtime at process exit, after all instrumented
// threads have been joined. No PROFILE_* macro may run past this point.
inline void Shutdown() {
    if (g_started.load(std::memory_order_acquire)) {
        g_enabled.store(false, std::memory_order_relaxed);
        tracy::ShutdownProfiler();
        g_started.store(false, std::memory_order_release);
    }
}

// True while a viewer/capture is connected. The IsEnabled gate keeps
// GetProfiler() from being touched before the runtime exists.
inline bool IsConnected() {
    return IsEnabled() && tracy::GetProfiler().IsConnected();
}

// Mirror an already-formatted log line into Tracy's message log (used by the
// LOG_* macros in util/Log.h). The text is copied.
inline void LogMessage(tracy::MessageSeverity severity, const char* text, size_t len) {
    if (!IsEnabled()) return;
    TracyLogString(severity, /*color=*/0, /*depth=*/0, len, text);
}

// RAII program-phase section (labeled bars above the timeline) for phases
// with a natural scope; pair PROFILE_SECTION_ENTER/LEAVE manually otherwise.
// Sections entered while no viewer is connected are dropped (on-demand).
class ScopedSection {
public:
    ScopedSection(uint16_t category, const char* label)
        : m_id(IsEnabled() ? TracySectionEnterCategory(category, "%s", label) : 0) {}
    ~ScopedSection() {
        if (m_id != 0 && IsEnabled())
            TracySectionLeave(m_id);
    }
    ScopedSection(const ScopedSection&) = delete;
    ScopedSection& operator=(const ScopedSection&) = delete;

private:
    uint32_t m_id;
};

} // namespace Profiler

// Zone named after the enclosing function. The unique-per-line variable name
// allows multiple zones in one scope (each still ends at scope exit).
#define PROFILE_SCOPE() \
    ZoneNamed(TracyConcat(__scrubcut_zone_, TracyLine), Profiler::IsEnabled())

// Zone with an explicit name; `name` must be a string literal.
#define PROFILE_SCOPE_N(name) \
    ZoneNamedN(TracyConcat(__scrubcut_zone_, TracyLine), name, Profiler::IsEnabled())

// Gray-tinted zones for scopes that (mostly) block — queue backpressure,
// vsync, waiting for workers to park.
#define PROFILE_WAIT_SCOPE() \
    ZoneNamedC(TracyConcat(__scrubcut_zone_, TracyLine), Profiler::kWaitColor, Profiler::IsEnabled())
#define PROFILE_WAIT_SCOPE_N(name) \
    ZoneNamedNC(TracyConcat(__scrubcut_zone_, TracyLine), name, Profiler::kWaitColor, Profiler::IsEnabled())

// Frame boundary — call once per main-loop iteration.
#define PROFILE_FRAME() \
    do { if (Profiler::IsEnabled()) { tracy::Profiler::SendFrameMark(nullptr); } } while (0)

// Name the calling thread in traces; call at thread entry. Threads started
// before profiling was enabled show up by id only.
#define PROFILE_THREAD(name) \
    do { if (Profiler::IsEnabled()) { tracy::SetThreadName(name); } } while (0)

// Numeric time-series (queue depths, buffer levels); `name` must be a literal.
#define PROFILE_PLOT(name, value) \
    do { if (Profiler::IsEnabled()) { TracyPlot(name, value); } } while (0)

// Instant profiling-only marker; `msg` must be a string literal. Trace
// severity + gray keep it distinct from the LOG_* mirroring (util/Log.h),
// which arrives as Info/Warning/Error.
#define PROFILE_MARK(msg) \
    do { if (Profiler::IsEnabled()) { \
        tracy::Profiler::LogString(tracy::MessageSourceType::User, \
                                   tracy::MessageSeverity::Trace, \
                                   0x808080 /*gray*/, /*depth=*/0, msg); \
    } } while (0)

// Enter a program-phase section (printf-style label); returns the id for
// PROFILE_SECTION_LEAVE, or 0 when off/disconnected (leaving 0 is a no-op).
#define PROFILE_SECTION_ENTER(category, ...) \
    (Profiler::IsEnabled() ? TracySectionEnterCategory(category, __VA_ARGS__) : 0u)
#define PROFILE_SECTION_LEAVE(id) \
    do { if (Profiler::IsEnabled() && (id) != 0) { TracySectionLeave(id); } } while (0)
