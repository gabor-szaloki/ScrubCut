#pragma once

#include "core/Demuxer.h"
#include "core/VideoDecoder.h"
#include "core/AudioDecoder.h"
#include "core/AudioOutput.h"
#include "core/FrameConverter.h"
#include "core/Clock.h"
#include "core/PacketQueue.h"
#include "core/DecodedFrameQueue.h"
#include "core/FrameQueue.h"
#include "core/FrameCache.h"
#include "util/Types.h"

#include <thread>
#include <atomic>
#include <functional>
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
    // Pause playback. By default also kicks off an async fill of the
    // step-back frame cache around the pause point (on the seek thread, same
    // as after a seek). Internal callers that immediately seek or manage the
    // cache themselves pass populateCache = false.
    void Pause(bool populateCache = true);
    void TogglePlayPause();

    void SeekTo(double seconds, bool resumeAfter = false);
    void SeekRelative(double deltaSec);
    void StepFrame(int direction); // +1 forward, -1 backward

    // Called from the main thread each render frame.
    bool TryGetVideoFrame(const uint8_t** outRGBA, int* outWidth, int* outHeight);

    // Called from the main thread each frame. Handles post-seek resume.
    void PollSeekComplete();

    // Per-frame queue-depth plots for the profiler (no-op unless enabled).
    void EmitProfilerPlots();

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
    // Color mode of the open video (SDR / HDR PQ / HDR HLG), derived from the
    // stream's transfer characteristic at Open(). Drives texture format + GPU
    // tone-mapping in App.
    VideoColorMode GetVideoColorMode() const { return m_videoColorMode; }
    // Source gamut, for the tone-map shader's gamut->BT.709 conversion.
    VideoColorPrimaries GetVideoColorPrimaries() const { return m_videoColorPrimaries; }
    // Short human-readable colorspace label for the timeline, e.g.
    // "BT.2020 PQ (HDR)" or "BT.709 (SDR)". Empty when no media is open.
    const std::string& GetColorSpaceLabel() const { return m_videoColorSpaceLabel; }
    const char* GetVideoCodecName() const;
    int64_t GetBitRate() const;
    int64_t GetFileSize() const;

    const std::vector<Chapter>& GetChapters() const { return m_chapters; }

    // Read-only track lists extracted from the source file at Open().
    const std::vector<AudioTrackInfo>& GetAudioTracks() const { return m_audioTracks; }
    const std::vector<SubtitleTrackInfo>& GetSubtitleTracks() const { return m_subtitleTracks; }
    int GetActiveAudioStreamIndex() const { return m_demuxer.GetAudioStreamIndex(); }

    // Switch the active audio stream mid-playback. Parks the pipeline, reopens
    // the audio decoder/output/resampler for the new stream, then re-seeks to
    // the current position to refill. No-op if streamIndex is already active or
    // isn't a valid audio stream. Safe to call from the main thread.
    void SetAudioTrack(int streamIndex);

    void SetSpeed(double speed);
    double GetSpeed() const { return m_clock.GetSpeed(); }

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
    void ConvertThread();
    void AudioDecodeThread();

    // Spawn worker threads (parked). Called from Open.
    void SpawnPipelineThreads();

    // Signal exit + join all worker threads. Called from Close.
    void StopPipelineThreads();

    // Park: signal threads to wait at top-of-loop, wake any blocked queue ops,
    // and wait for the parked count to match the thread count. After this
    // returns, queues + decoder may be safely flushed/reseeked.
    void ParkPipeline();

    // Unpark: clear queue interrupts, set pipeline active, notify_all.
    void UnparkPipeline();

    // True between Park() and Unpark(). Threads use this at the top of their
    // loop to decide whether to do work or park.
    bool IsPipelineActiveLocked() const { return m_pipelineActive; }

    // Top-of-loop park gate, called by each worker thread. Returns false if
    // the pipeline is exiting (thread should return).
    bool WorkerParkOrExit();

    // Wait for any pending async seek to complete.
    void WaitForSeek();

    // Per-frame A/V sync auto-correction (called from PollSeekComplete).
    // Video (the wall clock) is the master: measure how far the audio
    // device's actual playout position is from the clock and steer audio to
    // follow — a fraction-of-a-percent device-rate servo for small errors
    // (inaudible), DiscardUntil for large audio-behind jumps, and a hold
    // brake (device paused until the clock catches up) for large audio-ahead
    // drift when video delivery can't sustain the requested speed. Skipped
    // while the stream is dry (audio starved or ended past the audio track).
    void SyncAudioToClock();
    bool m_audioHold = false;  // runaway brake: device paused until the clock catches up

    // Drop queued audio packets whose content ends before cutoffSec (with
    // the same 25ms straddle margin used when capturing them). Pipeline must
    // be parked and the audio queue's interrupt cleared — the pop/re-push
    // cycle assumes no concurrent consumer.
    void FilterAudioQueueBefore(double cutoffSec);

    // Recycling pool for converted-frame buffers (full-res 4 B/px, so worth
    // reusing). A frame is free for reuse when the pool holds the only
    // reference. Shared between ConvertThread (playing), the seek thread's
    // cache maintenance, and the sync decode paths (parked), hence the mutex.
    ConvertedFramePtr AcquirePooledFrame();
    ConvertedFramePtr ConvertToPooledFrame(FrameConverter& conv, AVFrame* frame);
    std::vector<ConvertedFramePtr> m_framePool;
    std::mutex m_framePoolMutex;

    // Staged-frame handoff: the sync decode/step paths (main or seek thread)
    // stage one frame; TryGetVideoFrame (main thread) consumes it. The mutex
    // makes the shared_ptr handoff safe across those threads.
    bool StageFrame(ConvertedFramePtr frame, bool putInCache);
    ConvertedFramePtr TakeStagedFrame();
    void ClearStagedFrame();
    ConvertedFramePtr m_stagedFrame;
    std::mutex m_stagedFrameMutex;

    // Synchronously decode the next video frame from the current demuxer/decoder state.
    // No seek, no flush. Returns true if a frame was decoded and cached.
    // Used only for the initial-frame decode in Open() — pipeline must be parked.
    bool SyncDecodeNextFrame();

    // Seek to targetSec, then synchronously decode forward to the target frame.
    // Flushes decoder. Pipeline must be parked.
    bool SyncSeekAndDecode(double targetSec);

    // Like SyncSeekAndDecode, but lands on the frame with the largest pts that
    // is STRICTLY LESS than maxPts. Used for backward frame stepping — avoids
    // snapping back to the current frame when B-frame reordering or keyframe
    // alignment causes the first decoded pts to already be >= the target.
    bool SyncSeekAndDecodeBefore(double seekSec, int64_t maxPts);

    // GOP-window frame cache. The cache holds one contiguous run spanning
    // kCacheGopsBehind + kCacheGopsAhead full GOPs around the resting
    // playhead. Every rest-point change (pause, seek landing, step) posts a
    // maintenance request; the seek thread rebuilds/extends the window one
    // GOP at a time toward the violated edge and trims the far side, so
    // stepping rarely hits a synchronous decode.
    static constexpr int kCacheGopsBehind = 1;
    static constexpr int kCacheGopsAhead = 1;
    static constexpr int kMaxCacheFrames = 256;  // hard memory safety cap
    void PostCacheWindowRequest(int64_t centerPts);
    // One increment of maintenance; returns true while more work may remain.
    bool MaintainCacheWindow(int64_t centerPts, const std::function<bool()>& shouldAbort);
    // PTS of the keyframe at/before `sec` (probes the cache demuxer without
    // decoding). AV_NOPTS_VALUE on failure.
    int64_t ProbeCacheKeyframeBefore(double sec);
    std::atomic<int64_t> m_cacheWindowCenter{AV_NOPTS_VALUE};
    int64_t m_cacheBofKf = AV_NOPTS_VALUE;  // window start is the file's first keyframe (seek thread only)
    bool m_cacheEofSeen = false;            // window end reached EOF (seek thread only)

    // Tick the pipeline once: unpark, wait until at least one frame lands in
    // m_videoFrameQueue, then re-park. Used by StepFrame(+1) when the queue
    // is empty. timeoutMs caps the wait.
    bool TickPipelineOneFrame(int timeoutMs);

    // Flush queues + decoder; bumps m_pipelineFlushGen so DemuxThread's
    // held packet (if any) is discarded as stale. Pipeline must be parked.
    void FlushPipelineState();


    // Background seek thread
    void SeekThread();
    std::thread m_seekThread;
    std::mutex m_seekMutex;
    std::condition_variable m_seekCv;
    double m_seekRequest = -1.0;          // pending seek target, -1 = none
    bool m_cacheRequest = false;          // cache-window maintenance pending
    std::atomic<bool> m_seekBusy{false};   // seek thread is working
    std::atomic<double> m_pendingSeekTarget{-1.0}; // for UI: immediate seek target display
    std::atomic<bool> m_seekDone{false};   // seek completed, main thread should check
    std::atomic<bool> m_seekShouldResume{false}; // main thread should call Play()
    std::atomic<bool> m_stopSeekThread{false};
    std::atomic<bool> m_scrubbing{false};    // true during timeline drag (keyframe-only mode)
    bool m_seekThreadRunning = false;

    // Profiler program-phase sections: the open video (Open/Close) and the
    // transport state (Playing/Paused). Main thread only.
    void SetTransportSection(const char* label);  // nullptr = leave only
    uint32_t m_profMediaSectionId = 0;
    uint32_t m_profTransportSectionId = 0;
    const char* m_profTransportLabel = nullptr;
    double m_profTransportSpeed = 0.0;  // speed baked into the current Playing label

    // When set (!= kNoAudioSkip), AudioDecodeThread discards/trims decoded
    // samples up to this media time before writing to the output — used when
    // Play() has to rebuild the audio stream at a resume point that packet
    // granularity alone can't hit exactly.
    static constexpr double kNoAudioSkip = -1.0e18;
    std::atomic<double> m_audioSkipUntil{kNoAudioSkip};

    // Sticky flag: true when the user intends playback to continue after seeking.
    // Set by SeekTo when playback was active, cleared only when Play() is called.
    std::atomic<bool> m_wantsToPlay{false};

    Demuxer m_demuxer;
    VideoDecoder m_videoDecoder;
    AudioDecoder m_audioDecoder;
    AudioOutput m_audioOutput;
    // Owned by ConvertThread while playing (frames convert off the main
    // thread); the parked sync paths reuse it — same exclusivity rule as the
    // decoder, guaranteed by ParkPipeline.
    FrameConverter m_frameConverter;
    Clock m_clock;

    // Cache-population uses its own demuxer + decoder so populating the
    // backward-step frame cache after a seek doesn't disturb the main
    // decoder's state. With a single shared decoder, PopulateCacheAround-
    // Current would seek the demuxer and flush the decoder, forcing the
    // next Play() to re-seek and decode a full GOP of catchup. With a
    // separate decoder, the main one stays warm at the seeked position
    // and Play() can resume instantly.
    Demuxer m_cacheDemuxer;
    VideoDecoder m_cacheDecoder;
    FrameConverter m_cacheConverter;

    // Read-only metadata extracted from the source file at Open().
    std::vector<Chapter> m_chapters;
    std::vector<AudioTrackInfo> m_audioTracks;
    std::vector<SubtitleTrackInfo> m_subtitleTracks;

    PacketQueue m_videoPacketQueue{64};
    PacketQueue m_audioPacketQueue{64};
    DecodedFrameQueue m_decodedFrameQueue{4};
    FrameQueue m_videoFrameQueue{4};

    std::thread m_demuxThread;
    std::thread m_videoDecodeThread;
    std::thread m_convertThread;
    std::thread m_audioDecodeThread;

    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_eof{false};
    bool m_hasMedia = false;
    bool m_hasAudio = false;
    VideoColorMode m_videoColorMode = VideoColorMode::SDR;
    VideoColorPrimaries m_videoColorPrimaries = VideoColorPrimaries::BT2020;
    std::string m_videoColorSpaceLabel;

    int64_t m_lastDisplayedPts = AV_NOPTS_VALUE;

    // Pipeline park state. The DemuxThread/VideoDecodeThread/AudioDecodeThread
    // workers run continuously between Open() and Close(); pause is
    // implemented by parking them at the top of their loop instead of
    // tearing down + respawning. Eliminates entire bug classes around
    // mid-abort lost packets and stale state. See ParkPipeline()/Unpark().
    mutable std::mutex m_pipelineMutex;
    std::condition_variable m_pipelineCv;        // wakes parked threads
    std::condition_variable m_pipelineParkedCv;  // wakes the parker
    bool m_pipelineActive = false;
    int  m_pipelineParkedCount = 0;
    int  m_pipelineThreadCount = 0;
    bool m_pipelineExit = false;

    // Generation counter bumped on every flush of the demuxer/decoder/queues.
    // DemuxThread captures this when it stashes a packet that couldn't be
    // pushed (queue full + park interrupted); on resume, if the gen has
    // changed, the held packet is stale (demuxer was repositioned) and
    // gets discarded. Without this, Pause would leak the packet across a
    // subsequent Seek.
    std::atomic<uint64_t> m_pipelineFlushGen{0};

    // True when m_lastDisplayedPts was changed without a matching decoder
    // reseek — i.e. backward-step cache hit. The next forward-direction
    // operation (Play, StepFrame(+1)) re-seeks the decoder before producing
    // frames. Cleared the moment a resync runs.
    bool m_needsResync = false;

    // Currently displayed frame — keeps the buffer TryGetVideoFrame handed to
    // the renderer alive until the next frame replaces it. Main thread only.
    ConvertedFramePtr m_displayedFrame;

    // ConvertThread skips frames whose content time is before this (they'd
    // be dropped by TryGetVideoFrame's catch-up anyway). Updated
    // by the main thread; kNeverDrop while paused/stepping so every frame
    // converts.
    static constexpr double kNeverDrop = -1.0e300;
    std::atomic<double> m_dropFramesBeforeSec{kNeverDrop};

    // Frame cache for instant stepping around the resting playhead
    FrameCache m_frameCache;

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
