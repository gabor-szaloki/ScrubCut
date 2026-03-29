#include "core/Clock.h"
#include "core/AudioOutput.h"

double Clock::GetTime() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_paused.load(std::memory_order_relaxed))
        return m_baseTime;
    double elapsed = static_cast<double>(SDL_GetTicksNS() - m_baseTicksNS) / 1e9;
    return m_baseTime + elapsed * m_speed;
}

void Clock::SetTime(double t) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_baseTime = t;
    m_baseTicksNS = SDL_GetTicksNS();
}

void Clock::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (paused && !m_paused.load(std::memory_order_relaxed)) {
        double elapsed = static_cast<double>(SDL_GetTicksNS() - m_baseTicksNS) / 1e9;
        m_baseTime += elapsed * m_speed;
    } else if (!paused && m_paused.load(std::memory_order_relaxed)) {
        m_baseTicksNS = SDL_GetTicksNS();
    }
    m_paused.store(paused, std::memory_order_relaxed);
}

void Clock::SetSpeed(double speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_paused.load(std::memory_order_relaxed)) {
        // Capture current time before changing speed
        double elapsed = static_cast<double>(SDL_GetTicksNS() - m_baseTicksNS) / 1e9;
        m_baseTime += elapsed * m_speed;
        m_baseTicksNS = SDL_GetTicksNS();
    }
    m_speed = speed;
}
