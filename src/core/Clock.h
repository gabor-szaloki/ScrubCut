#pragma once

#include <atomic>
#include <mutex>
#include <SDL3/SDL.h>

class AudioOutput;

// A/V sync clock. When audio is available, audio is the master.
// When no audio, falls back to wall clock.
class Clock {
public:
    Clock() = default;

    void SetAudioOutput(AudioOutput* audio) { m_audio = audio; }

    // Get the current playback time in seconds.
    double GetTime() const;

    // Set the time (used after seek or when starting playback).
    void SetTime(double t);

    void SetPaused(bool paused);
    bool IsPaused() const { return m_paused.load(std::memory_order_relaxed); }

    void SetSpeed(double speed);
    double GetSpeed() const { return m_speed; }

private:
    AudioOutput* m_audio = nullptr;

    mutable std::mutex m_mutex;
    double m_baseTime = 0.0;
    uint64_t m_baseTicksNS = 0;
    double m_speed = 1.0;
    std::atomic<bool> m_paused{true};
};
