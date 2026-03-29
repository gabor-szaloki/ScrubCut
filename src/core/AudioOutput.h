#pragma once

#include "util/FFmpegUtils.h"
#include <SDL3/SDL.h>
#include <mutex>
#include <atomic>

class AudioOutput {
public:
    AudioOutput() = default;
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool Open(int sampleRate, int channels);
    void Close();

    // Write resampled S16 interleaved samples to the audio stream.
    void Write(const uint8_t* data, int size);

    void Flush();
    void Pause();
    void Resume();

    // Returns the estimated playback position in seconds.
    double GetPlaybackPosition() const;

    // Reset playback position tracking (call on seek).
    void ResetPosition(double startTime);

    void SetSpeed(float speed);

    int GetSampleRate() const { return m_sampleRate; }
    int GetChannels() const { return m_channels; }

private:
    SDL_AudioStream* m_stream = nullptr;
    int m_sampleRate = 0;
    int m_channels = 0;

    // Position tracking: total bytes fed into the stream
    mutable std::mutex m_mutex;
    int64_t m_totalBytesWritten = 0;
    double m_positionOffset = 0.0;
};
