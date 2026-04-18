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
#include <mutex>
#include <condition_variable>
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

    void SeekTo(double seconds, bool resumeAfter = false);
    void SeekRelative(double deltaSec);
    void StepFrame(int direction); // +1 forward, -1 backward

    // Populate frame cache around the current position (for backward stepping).
    // Called after a seek drag is released. Runs synchronously but only does
    // RGBA conversion of frames not already in cache.
    void PopulateCacheAroundCurrent();

    // Called from the main thread each render frame.
    bool TryGetVideoFrame(const uint8_t** outRGBA, int* outWidth, int* outHeight);

    // Called from the main thread each frame. Handles post-seek resume.
    void PollSeekComplete();

    bool IsPlaying() const { return m_playing.load(std::memory_order_relaxed); }
    bool IsSeekBusy() const { return m_seekBusy.load(std::memory_order_relaxed); }
    bool HasMedia() const { return m_hasMedia; }
    bool HasAudio() const { return m_hasAudio; }
    bool IsEOF() const { return m_eof.load(std::memory_order_relaxed); }
    double GetPlaybackTime() const { return m_clock.GetTime(); }
    double GetSeekTargetTime() const {
        double t = m_pendingSeekTarget.load(std::memory_order_relaxed);
        return t >= 0.0 ? t : m_clock.GetTime();
    }
    double GetDuration() const { return m_demuxer.GetDuration(); }
    double GetFrameRate() const { return m_demuxer.GetVideoFrameRate(); }
    double GetFrameDuration() const;
    int GetVideoWidth() const { return m_videoDecoder.GetWidth(); }
    int GetVideoHeight() const { return m_videoDecoder.GetHeight(); }
    const char* GetVideoCodecName() const;
    int64_t GetBitRate() const;
    int64_t GetFileSize() const;

    void SetSpeed(double speed);
    double GetSpeed() const { return m_clock.GetSpeed(); }

    void SetProfileSeek(bool enable) { m_profileSeek = enable; }

    void SetScrubbing(bool scrubbing) {
        m_scrubbing.store(scrubbing, std::memory_order_relaxed);
    }

    void SetVolume(float volume);
    float GetVolume() const { return m_volume; }
    void SetMuted(bool muted);
    bool IsMuted() const { return m_muted; }

private:
    void DemuxThread();
    void VideoDecodeThread();
    void AudioDecodeThread();

    void StopThreads();
    void StartThreads();

    // Ensure threads are stopped (for paused operations). No-op if already stopped.
    void EnsureThreadsStopped();

    // Wait for any pending async seek to complete.
    void WaitForSeek();

    // Synchronously decode the next video frame from the current demuxer/decoder state.
    // No seek, no flush. Returns true if a frame was decoded and cached.
    bool SyncDecodeNextFrame();

    // Seek to targetSec, then synchronously decode forward to the target frame.
    // Flushes decoder. Threads must be stopped.
    bool SyncSeekAndDecode(double targetSec);



    // Background seek thread
    void SeekThread();
    std::thread m_seekThread;
    std::mutex m_seekMutex;
    std::condition_variable m_seekCv;
    double m_seekRequest = -1.0;          // pending seek target, -1 = none
    std::atomic<bool> m_seekBusy{false};   // seek thread is working
    std::atomic<double> m_pendingSeekTarget{-1.0}; // for UI: immediate seek target display
    std::atomic<bool> m_seekDone{false};   // seek completed, main thread should check
    std::atomic<bool> m_seekShouldResume{false}; // main thread should call Play()
    std::atomic<bool> m_stopSeekThread{false};
    std::atomic<bool> m_scrubbing{false};    // true during timeline drag (keyframe-only mode)
    bool m_seekThreadRunning = false;
    bool m_profileSeek = false;

    // Sticky flag: true when the user intends playback to continue after seeking.
    // Set by SeekTo when playback was active, cleared only when Play() is called.
    std::atomic<bool> m_wantsToPlay{false};

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
    std::atomic<bool> m_threadsRunning{false};
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

    // Volume
    float m_volume = 1.0f;
    bool m_muted = false;

    // Resampler for audio
    SwrContext* m_swrCtx = nullptr;
    void SetupResampler();
    void CloseResampler();
};
