#pragma once

#include "core/Demuxer.h"
#include "core/VideoDecoder.h"
#include "core/AudioDecoder.h"
#include "core/AudioOutput.h"
#include "core/FrameConverter.h"
#include "core/Clock.h"
#include "core/PacketQueue.h"
#include "core/FrameQueue.h"
#include "core/FrameCache.h"

#include <thread>
#include <atomic>
#include <string>

class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool Open(const std::string& path);
    void Close();

    void Play();
    void Pause();
    void TogglePlayPause();

    void SeekTo(double seconds);
    void SeekRelative(double deltaSec);
    void StepFrame(int direction); // +1 forward, -1 backward

    // Called from the main thread each render frame.
    bool TryGetVideoFrame(const uint8_t** outRGBA, int* outWidth, int* outHeight);

    bool IsPlaying() const { return m_playing.load(std::memory_order_relaxed); }
    bool HasMedia() const { return m_hasMedia; }
    bool HasAudio() const { return m_hasAudio; }
    bool IsEOF() const { return m_eof.load(std::memory_order_relaxed); }
    double GetPlaybackTime() const { return m_clock.GetTime(); }
    double GetDuration() const { return m_demuxer.GetDuration(); }
    double GetFrameRate() const { return m_demuxer.GetVideoFrameRate(); }
    double GetFrameDuration() const;
    int GetVideoWidth() const { return m_videoDecoder.GetWidth(); }
    int GetVideoHeight() const { return m_videoDecoder.GetHeight(); }

    void SetSpeed(double speed);
    double GetSpeed() const { return m_clock.GetSpeed(); }

private:
    void DemuxThread();
    void VideoDecodeThread();
    void AudioDecodeThread();

    void StopThreads();
    void StartThreads();

    // Ensure threads are stopped (for paused operations). No-op if already stopped.
    void EnsureThreadsStopped();

    // Synchronously decode the next video frame from the current demuxer/decoder state.
    // No seek, no flush. Returns true if a frame was decoded and cached.
    bool SyncDecodeNextFrame();

    // Seek to targetSec, then synchronously decode forward to the target frame.
    // Flushes decoder. Threads must be stopped.
    bool SyncSeekAndDecode(double targetSec);

    Demuxer m_demuxer;
    VideoDecoder m_videoDecoder;
    AudioDecoder m_audioDecoder;
    AudioOutput m_audioOutput;
    FrameConverter m_frameConverter;
    Clock m_clock;

    PacketQueue m_videoPacketQueue{64};
    PacketQueue m_audioPacketQueue{64};
    FrameQueue m_videoFrameQueue{4};

    std::thread m_demuxThread;
    std::thread m_videoDecodeThread;
    std::thread m_audioDecodeThread;

    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_stopThreads{false};
    std::atomic<bool> m_eof{false};
    bool m_threadsRunning = false;
    bool m_hasMedia = false;
    bool m_hasAudio = false;

    int64_t m_lastDisplayedPts = AV_NOPTS_VALUE;

    // Cached RGBA frame from synchronous decode (single frame, consumed on read)
    const uint8_t* m_cachedFrame = nullptr;
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;
    bool m_hasCachedFrame = false;

    // Frame cache for instant backward stepping
    FrameCache m_frameCache{128};

    // True when decoder/demuxer position doesn't match m_lastDisplayedPts
    // (e.g. after a cache-hit backward step). Next forward decode must seek first.
    bool m_decoderDirty = false;

    // When true, the clock is held paused until the first frame from decode
    // threads arrives at or after m_resumeTime. Prevents fast-forward on Play.
    bool m_waitingForResumeFrame = false;
    double m_resumeTime = 0.0;

    // Resampler for audio
    SwrContext* m_swrCtx = nullptr;
    void SetupResampler();
    void CloseResampler();
};
