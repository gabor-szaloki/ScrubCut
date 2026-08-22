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
    // ptsSec is the media timestamp of the first sample; pass NAN when the
    // source pts is unknown to continue from the previous write's end.
    void Write(const uint8_t* data, int size, double ptsSec);

    void Flush();
    void Pause();
    void Resume();

    // Media-time position of the sample the device is playing right now:
    // the pts at the end of the last written chunk, minus however much of
    // the written data is still queued in the stream. Because it's derived
    // from packet ptss rather than a running byte count, it stays truthful
    // across pauses, flushes, and dropped packets.
    double GetPlaybackPosition() const;

    // True while the stream still holds unplayed data. When it runs dry
    // (starvation or end of audio), GetPlaybackPosition freezes at the last
    // written pts, so there is no live position to steer audio sync against.
    bool HasQueuedData() const;

    // Seed the position after a seek/flush, before the first Write lands.
    void ResetPosition(double startTime);

    // Consume and discard buffered content from the stream's head up to
    // targetPts, so playback resumes exactly there (call with the device
    // paused). Used when the clock stepped forward while paused but the
    // needed content is still in the buffer. Returns false when targetPts
    // isn't within the buffered range — the caller must rebuild the stream
    // from the packet queue instead.
    bool DiscardUntil(double targetPts);

    void SetSpeed(float speed);
    void SetVolume(float volume); // 0.0 to 1.0
    float GetVolume() const { return m_volume; }

    int GetSampleRate() const { return m_sampleRate; }
    int GetChannels() const { return m_channels; }

private:
    SDL_AudioStream* m_stream = nullptr;
    int m_sampleRate = 0;
    int m_channels = 0;

    mutable std::mutex m_mutex;
    // Media pts one-past the last sample written into the stream.
    double m_lastWrittenEndPts = 0.0;
    float m_volume = 1.0f;
};
