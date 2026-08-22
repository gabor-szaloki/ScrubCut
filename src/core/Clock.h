#pragma once

#include <atomic>
#include <mutex>
#include <SDL3/SDL.h>

// Playback clock, driven by the wall clock. This is the A/V sync master:
// video frames present against it directly, and Player::SyncAudioToClock
// steers the audio device to follow it.
class Clock {
public:
    Clock() = default;

    // Get the current playback time in seconds.
    double GetTime() const;

    // Set the time (used after seek or when starting playback).
    void SetTime(double t);

    void SetPaused(bool paused);
    bool IsPaused() const { return m_paused.load(std::memory_order_relaxed); }

    void SetSpeed(double speed);
    double GetSpeed() const { return m_speed; }

private:
    mutable std::mutex m_mutex;
    double m_baseTime = 0.0;
    uint64_t m_baseTicksNS = 0;
    double m_speed = 1.0;
    std::atomic<bool> m_paused{true};
};
