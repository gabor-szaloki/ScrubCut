#include "core/Player.h"
#include "util/Log.h"
#include "util/Trace.h"

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>
static void SetCurrentThreadName(const wchar_t* name) {
    SetThreadDescription(GetCurrentThread(), name);
}
#else
static void SetCurrentThreadName(const wchar_t*) {}
#endif

Player::Player() = default;

Player::~Player() {
    Close();
}

bool Player::Open(const std::string& path) {
    Close();

    if (!m_demuxer.Open(path))
        return false;

    if (!m_videoDecoder.Open(m_demuxer.GetVideoCodecParams()))
        return false;

    m_chapters.clear();
    if (AVFormatContext* fmt = m_demuxer.GetFormatContext()) {
        m_chapters.reserve(fmt->nb_chapters);
        for (unsigned i = 0; i < fmt->nb_chapters; ++i) {
            AVChapter* c = fmt->chapters[i];
            double tb = av_q2d(c->time_base);
            Chapter ch;
            ch.startSec = c->start * tb;
            ch.endSec   = c->end   * tb;
            AVDictionaryEntry* t = av_dict_get(c->metadata, "title", nullptr, 0);
            if (t && t->value) ch.title = t->value;
            m_chapters.push_back(std::move(ch));
        }
    }

    // Independent demuxer + decoder for backward-step cache population.
    // Failure here is non-fatal — caching just falls back to no-op and
    // backward stepping does a full re-seek per step like before.
    if (!m_cacheDemuxer.Open(path) ||
        !m_cacheDecoder.Open(m_cacheDemuxer.GetVideoCodecParams())) {
        m_cacheDecoder.Close();
        m_cacheDemuxer.Close();
    }

    m_hasAudio = false;
    if (m_demuxer.GetAudioStreamIndex() >= 0) {
        if (m_audioDecoder.Open(m_demuxer.GetAudioCodecParams())) {
            int sr = m_audioDecoder.GetSampleRate();
            int ch = m_audioDecoder.GetChannels();
            if (m_audioOutput.Open(sr, ch)) {
                SetupResampler();
                m_hasAudio = true;
                m_clock.SetAudioOutput(&m_audioOutput);
                // Apply volume/mute state that may have been set BEFORE
                // audio existed (e.g. from saved preferences at launch).
                // Without this, a saved muted=true never reaches the audio
                // output until the user toggles mute again.
                SetMuted(m_muted);
            }
        }
    }

    if (!m_hasAudio) {
        m_clock.SetAudioOutput(nullptr);
    }

    m_hasMedia = true;
    m_eof = false;
    m_lastDisplayedPts = AV_NOPTS_VALUE;
    m_hasCachedFrame = false;
    m_clock.SetTime(0.0);
    m_clock.SetPaused(true);

    // Start seek thread
    m_stopSeekThread = false;
    m_seekRequest = -1.0;
    m_seekDone = false;
    m_seekShouldResume = false;
    m_seekBusy = false;
    m_wantsToPlay = false;
    m_seekThreadRunning = true;
    m_seekThread = std::thread(&Player::SeekThread, this);

    // Spawn pipeline workers in parked state. They'll be unparked by Play.
    SpawnPipelineThreads();

    // Decode first frame synchronously so we have something to show.
    // Pipeline is parked, so this owns the demuxer + video decoder.
    SyncDecodeNextFrame();

    return true;
}

void Player::Close() {
    // Stop seek thread first (it may park/unpark the pipeline).
    if (m_seekThreadRunning) {
        m_stopSeekThread = true;
        m_seekCv.notify_one();
        if (m_seekThread.joinable()) m_seekThread.join();
        m_seekThreadRunning = false;
    }

    StopPipelineThreads();
    m_frameCache.Clear();
    CloseResampler();

    m_audioOutput.Close();
    m_audioDecoder.Close();
    m_videoDecoder.Close();
    m_demuxer.Close();
    m_cacheDecoder.Close();
    m_cacheDemuxer.Close();

    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_videoFrameQueue.Flush();

    m_hasMedia = false;
    m_hasAudio = false;
    m_playing = false;
    m_eof = false;
    m_lastDisplayedPts = AV_NOPTS_VALUE;
    m_hasCachedFrame = false;
    m_chapters.clear();
}

void Player::Play() {
    TRACE_EVENT("Player::Play");
    if (!m_hasMedia) return;
    WaitForSeek();

    // m_eof gets set whenever any read loop hits EOF — including when the
    // user seeks/steps near (but not at) the end of the file, where the
    // decoder pipeline drained packets through EOF to retrieve buffered
    // frames. Don't treat that as "user at end". Only restart from the
    // beginning if the playback clock has actually reached the duration.
    double curTime = m_clock.GetTime();
    double duration = m_demuxer.GetDuration();
    bool atActualEnd = duration > 0.0 && curTime >= duration - 0.001;
    if (m_eof && atActualEnd) {
        // Restart from beginning. Pipeline is parked (Pause did that, or
        // playback ended which leaves it parked too).
        ParkPipeline();
        FlushPipelineState();
        m_demuxer.Seek(0.0);
        m_eof = false;
        m_clock.SetTime(0.0);
    }
    m_eof = false;

    double resumeTime = m_clock.GetTime();
    m_hasCachedFrame = false;

    // If the last operation left the decoder out of sync with what's
    // displayed (backward-step cache hit), re-seek now so the threads
    // produce frames matching m_lastDisplayedPts forward.
    if (m_needsResync) {
        ParkPipeline();  // idempotent if already parked
        FlushPipelineState();
        AVRational tb = m_demuxer.GetVideoTimeBase();
        double seekSec = (m_lastDisplayedPts + 0.5) * av_q2d(tb);
        SyncSeekAndDecode(seekSec);
        m_needsResync = false;
    }

    if (m_hasAudio) {
        m_audioOutput.Flush();
        m_audioOutput.ResetPosition(resumeTime);
    }

    // Don't start the clock yet — wait for the first frame at/after
    // resumeTime to land in the queue. This prevents fast-forward catch-up.
    m_clock.SetTime(resumeTime);
    m_waitingForResumeFrame = true;
    m_resumeTime = resumeTime;
    m_playing = true;
    m_pendingSeekTarget.store(-1.0, std::memory_order_relaxed);
    if (m_profileSeek) {
        m_playStartNS = SDL_GetTicksNS();
        m_playDroppedFrames = 0;
        LOG_INFO("Play: resume=%.3fs", resumeTime);
    }

    // Unpark workers — they were idle since the last Pause/Seek, with
    // queues + decoder state intact. No flush, no seek. TryGetVideoFrame
    // picks up the first frame at-or-past resumeTime.
    UnparkPipeline();
    // Clock stays paused; TryGetVideoFrame will unpause it when ready.
}

void Player::Pause() {
    TRACE_EVENT("Player::Pause");
    uint64_t t0 = m_profileSeek ? SDL_GetTicksNS() : 0;
    m_playing = false;
    m_wantsToPlay = false;
    m_waitingForResumeFrame = false;
    m_clock.SetPaused(true);
    if (m_hasAudio) m_audioOutput.Pause();

    // Park workers at the top of their loop. Queues + decoder state stay
    // intact — they'll resume on the next Play. Held packet inside
    // DemuxThread (if any) survives via the pipeline-flush-gen check.
    ParkPipeline();
    if (m_profileSeek) {
        LOG_INFO("Pause: %.2fms", (SDL_GetTicksNS() - t0) / 1e6);
    }
}

void Player::TogglePlayPause() {
    if (m_playing) Pause();
    else Play();
}

void Player::SeekTo(double seconds, bool resumeAfter) {
    TRACE_EVENT("Player::SeekTo");
    if (!m_hasMedia) return;
    double duration = m_demuxer.GetDuration();
    seconds = std::clamp(seconds, 0.0, duration);

    // Don't resume playback if seeking to the very end
    bool atEnd = (seconds >= duration - 0.01);

    // Track intent: if we were playing (or already want to play), remember it.
    if (!atEnd && (m_playing.load(std::memory_order_relaxed) || resumeAfter))
        m_wantsToPlay = true;

    // Signal that we want to stop playing (the seek thread handles
    // actually stopping/starting threads to avoid races).
    m_playing = false;
    m_waitingForResumeFrame = false;
    m_clock.SetPaused(true);
    if (m_hasAudio) m_audioOutput.Pause();

    // Clear stale completion — a new seek supersedes any previous result.
    m_seekDone = false;
    m_pendingSeekTarget.store(seconds, std::memory_order_relaxed);

    // Post seek request to background thread (non-blocking)
    {
        std::lock_guard<std::mutex> lock(m_seekMutex);
        m_seekRequest = seconds;
    }
    m_seekCv.notify_one();
}

void Player::SeekThread() {
    SetCurrentThreadName(L"ScrubCut Seek");

    while (!m_stopSeekThread) {
        double target;
        {
            std::unique_lock<std::mutex> lock(m_seekMutex);
            m_seekCv.wait(lock, [&] {
                return m_seekRequest >= 0.0 || m_stopSeekThread;
            });
            if (m_stopSeekThread) break;
            target = m_seekRequest;
            m_seekRequest = -1.0;
        }

        m_seekBusy = true;

        // Drain any further seek requests that arrived while we were waking up
        // (take the latest one — skip intermediate positions)
        {
            std::lock_guard<std::mutex> lock(m_seekMutex);
            if (m_seekRequest >= 0.0) {
                target = m_seekRequest;
                m_seekRequest = -1.0;
            }
        }

        // Park pipeline so we own the demuxer + decoder. SyncSeekAndDecode
        // does its own flush (and bumps m_pipelineFlushGen) — we don't
        // pre-flush here so the REUSE fast-path below can preserve state.
        ParkPipeline();

        // m_wantsToPlay carries "was playing" or "resume-after" intent from
        // SeekTo. False means user is scrubbing while paused — small delta
        // can be a no-op (keep displayed frame, only update clock).
        bool wasPlayingOrResume = m_wantsToPlay.load(std::memory_order_relaxed);
        bool scrubbing = m_scrubbing.load(std::memory_order_relaxed);

        {
            double currentSec = m_clock.GetTime();
            double delta = target - currentSec;
            double frameDur = GetFrameDuration();
            if (!wasPlayingOrResume && delta > -frameDur && delta <= 0.0) {
                if (m_profileSeek) LOG_INFO("Seek: REUSE delta=%.3f", delta);
                goto seekDone;
            }
        }

        // Check if a newer request arrived — skip this expensive decode
        {
            std::lock_guard<std::mutex> lock(m_seekMutex);
            if (m_seekRequest >= 0.0) {
                if (m_profileSeek) LOG_INFO("Seek: SKIPPED (newer request)");
                m_seekBusy = false;
                continue;
            }
        }

        if (m_profileSeek) LOG_INFO("Seek: FULL scrub=%d", scrubbing);
        SyncSeekAndDecode(target);
        m_needsResync = false;

        if (m_profileSeek) {
            // Arm A/V sync diagnostics: AudioDecodeThread + VideoDecodeThread
            // log the next few packet/frame ptss vs this seek target.
            AVRational vtb = m_demuxer.GetVideoTimeBase();
            double videoFirstSec = (m_lastDisplayedPts != AV_NOPTS_VALUE)
                ? static_cast<double>(m_lastDisplayedPts) * av_q2d(vtb) : 0.0;
            LOG_INFO("AVSync: seek target=%.3fs video first frame pts=%.3fs (delta=%+.3fs)",
                     target, videoFirstSec, videoFirstSec - target);
            m_avsyncSeekTarget.store(target, std::memory_order_relaxed);
            m_avsyncLogPackets.store(5, std::memory_order_relaxed);
        }

    seekDone:
        // Check if a newer seek request arrived while we were decoding
        {
            std::lock_guard<std::mutex> lock(m_seekMutex);
            if (m_seekRequest >= 0.0) {
                m_seekBusy = false;
                continue;
            }
        }

        bool shouldResume = m_wantsToPlay.load(std::memory_order_relaxed);

        // Populate cache for backward stepping (skip during scrubbing).
        // Abort early if a fresh seek arrives — the displayable frame is
        // already set inside SyncSeekAndDecode, so the next seek can run
        // immediately rather than waiting out a stale cache build.
        if (!shouldResume && !scrubbing) {
            PopulateCacheAroundCurrent([this] {
                if (m_stopSeekThread.load(std::memory_order_relaxed)) return true;
                std::lock_guard<std::mutex> lock(m_seekMutex);
                return m_seekRequest >= 0.0;
            });
        }

        // Pipeline stays parked. PollSeekComplete on main thread will call
        // Play() if shouldResume — Play unparks. Otherwise we stay paused.
        m_seekShouldResume = shouldResume;
        m_seekDone = true;
        m_seekBusy = false;
    }
}

void Player::PollSeekComplete() {
    // Pause at end of video
    if (m_playing && m_eof && m_clock.GetTime() >= m_demuxer.GetDuration()) {
        m_clock.SetTime(m_demuxer.GetDuration());
        Pause();
    }

    if (!m_seekDone.load(std::memory_order_acquire)) return;
    m_seekDone = false;

    if (m_seekShouldResume.load(std::memory_order_relaxed)) {
        m_seekShouldResume = false;
        m_wantsToPlay = false;
        Play();
    }
}

void Player::SeekRelative(double deltaSec) {
    // When a seek is already in flight (or pending), chain off its target
    // rather than off m_clock. Otherwise, two arrow presses fired before
    // the first seek's m_clock update both compute target = same_clock +
    // delta, collapsing into a single move instead of accumulating.
    double pending = m_pendingSeekTarget.load(std::memory_order_relaxed);
    double base = (pending >= 0.0) ? pending : m_clock.GetTime();
    SeekTo(base + deltaSec);
}

void Player::StepFrame(int direction) {
    TRACE_EVENT("Player::StepFrame");
    if (!m_hasMedia) return;
    uint64_t stepStartNS = m_profileSeek ? SDL_GetTicksNS() : 0;
    const char* stepKind = "unknown";

    WaitForSeek();
    m_pendingSeekTarget.store(-1.0, std::memory_order_relaxed);

    bool wasPlaying = m_playing.load(std::memory_order_relaxed);
    if (wasPlaying) Pause();
    // Pipeline is now parked (Pause does that, or it was already parked).

    if (direction > 0) {
        // Forward: check if next frame is already in cache.
        if (m_lastDisplayedPts != AV_NOPTS_VALUE) {
            AVRational tb = m_demuxer.GetVideoTimeBase();
            double frameDurSec = GetFrameDuration();
            int64_t nextPts = m_lastDisplayedPts + static_cast<int64_t>(frameDurSec / av_q2d(tb));
            const auto* cached = m_frameCache.FindNearest(nextPts);
            int64_t halfFrame = static_cast<int64_t>(frameDurSec / 2.0 / av_q2d(tb));
            if (cached && cached->pts > m_lastDisplayedPts &&
                std::abs(cached->pts - nextPts) <= halfFrame) {
                double frameSec = static_cast<double>(cached->pts) * av_q2d(tb);
                m_clock.SetTime(frameSec);
                m_lastDisplayedPts = cached->pts;
                m_cachedFrame = cached->rgba.data();
                m_cachedWidth = cached->width;
                m_cachedHeight = cached->height;
                m_hasCachedFrame = true;
                if (m_profileSeek) {
                    LOG_INFO("StepFrame +1 cache-hit %.2fms",
                             (SDL_GetTicksNS() - stepStartNS) / 1e6);
                }
                return;
            }
        }

        // If a prior backward-step cache hit deferred the decoder reseek,
        // do it now — we're about to produce the next frame from the
        // decoder pipeline and need it aligned with m_lastDisplayedPts.
        if (m_needsResync) {
            stepKind = "+1 resync";
            FlushPipelineState();
            AVRational tb = m_demuxer.GetVideoTimeBase();
            double seekSec = (m_lastDisplayedPts + 0.5) * av_q2d(tb);
            SyncSeekAndDecode(seekSec);
            m_needsResync = false;
            // SyncSeekAndDecode already populated m_cachedFrame for the
            // current pts; we want the NEXT frame instead. Fall through to
            // the tick path below to produce one.
            m_hasCachedFrame = false;
        }

        // Try the prefetched FrameQueue (filled by VideoDecodeThread before
        // pause). If a frame is sitting there, it's already display-ordered
        // immediately after m_lastDisplayedPts — use it.
        AVFrame* frame = av_frame_alloc();
        if (m_videoFrameQueue.TryPop(frame)) {
            stepKind = "+1 queue";
            AVRational tb = m_demuxer.GetVideoTimeBase();
            double frameSec = static_cast<double>(frame->pts) * av_q2d(tb);
            m_clock.SetTime(frameSec);
            m_lastDisplayedPts = frame->pts;
            m_cachedFrame = m_frameConverter.Convert(frame);
            m_cachedWidth = m_frameConverter.GetWidth();
            m_cachedHeight = m_frameConverter.GetHeight();
            m_hasCachedFrame = (m_cachedFrame != nullptr);
            if (m_hasCachedFrame)
                m_frameCache.Put(frame->pts, m_cachedFrame, m_cachedWidth, m_cachedHeight);
            av_frame_free(&frame);
            if (m_profileSeek) {
                LOG_INFO("StepFrame %s %.2fms", stepKind,
                         (SDL_GetTicksNS() - stepStartNS) / 1e6);
            }
            return;
        }
        av_frame_free(&frame);

        // Frame queue empty. Tick the pipeline once: unpark briefly so
        // VideoDecodeThread can produce one or more frames, then re-park.
        // This handles both "queue exhausted but decoder pipeline buffered"
        // and "needs new packets" cases without us touching demuxer/decoder
        // directly — the worker threads own that.
        if (strcmp(stepKind, "unknown") == 0) stepKind = "+1 tick";
        if (TickPipelineOneFrame(500)) {
            AVFrame* tickFrame = av_frame_alloc();
            if (m_videoFrameQueue.TryPop(tickFrame)) {
                AVRational tb = m_demuxer.GetVideoTimeBase();
                double frameSec = static_cast<double>(tickFrame->pts) * av_q2d(tb);
                m_clock.SetTime(frameSec);
                m_lastDisplayedPts = tickFrame->pts;
                m_cachedFrame = m_frameConverter.Convert(tickFrame);
                m_cachedWidth = m_frameConverter.GetWidth();
                m_cachedHeight = m_frameConverter.GetHeight();
                m_hasCachedFrame = (m_cachedFrame != nullptr);
                if (m_hasCachedFrame)
                    m_frameCache.Put(tickFrame->pts, m_cachedFrame, m_cachedWidth, m_cachedHeight);
            }
            av_frame_free(&tickFrame);
        }
        if (m_profileSeek) {
            LOG_INFO("StepFrame %s %.2fms", stepKind,
                     (SDL_GetTicksNS() - stepStartNS) / 1e6);
        }
    } else {
        // Backward: check frame cache first (instant).
        bool cacheHit = false;
        if (m_lastDisplayedPts != AV_NOPTS_VALUE) {
            const auto* cached = m_frameCache.FindBefore(m_lastDisplayedPts);
            if (cached) {
                TRACE_LOG("StepBack_CacheHit");
                AVRational tb = m_demuxer.GetVideoTimeBase();
                double frameSec = static_cast<double>(cached->pts) * av_q2d(tb);
                m_clock.SetTime(frameSec);
                m_lastDisplayedPts = cached->pts;
                m_cachedFrame = cached->rgba.data();
                m_cachedWidth = cached->width;
                m_cachedHeight = cached->height;
                m_hasCachedFrame = true;
                cacheHit = true;
            }
        }

        if (cacheHit) {
            // Decoder is now at the wrong position relative to
            // m_lastDisplayedPts, but we've shown the cached frame and
            // we're paused — there's no need to fix the decoder NOW.
            // Defer the reseek until the next forward-direction operation
            // (Play or StepFrame(+1)). Without this, every backward step
            // would cost a full GOP catchup decode (hundreds of ms on a
            // 4-second GOP), defeating the cache entirely.
            m_needsResync = true;
            if (m_profileSeek) {
                LOG_INFO("StepFrame -1 cache-hit %.2fms",
                         (SDL_GetTicksNS() - stepStartNS) / 1e6);
            }
            return;
        }

        // Cache miss — seek back and decode the frame immediately before
        // the current one.
        TRACE_LOG("StepBack_CacheMiss");
        FlushPipelineState();
        double frameDur = GetFrameDuration();
        double targetSec = m_clock.GetTime() - frameDur;
        if (targetSec < 0.0) targetSec = 0.0;
        int64_t maxPts = m_lastDisplayedPts;
        SyncSeekAndDecodeBefore(targetSec, maxPts);
        m_needsResync = false;
        PopulateCacheAroundCurrent();
        if (m_profileSeek) {
            LOG_INFO("StepFrame -1 cache-miss %.2fms",
                     (SDL_GetTicksNS() - stepStartNS) / 1e6);
        }
    }
}

double Player::GetFrameDuration() const {
    double fps = m_demuxer.GetVideoFrameRate();
    return (fps > 0) ? 1.0 / fps : 1.0 / 30.0;
}

const char* Player::GetVideoCodecName() const {
    AVCodecParameters* params = m_demuxer.GetVideoCodecParams();
    if (!params) return "unknown";
    const AVCodecDescriptor* desc = avcodec_descriptor_get(params->codec_id);
    return desc ? desc->name : "unknown";
}

int64_t Player::GetBitRate() const {
    AVFormatContext* fmt = m_demuxer.GetFormatContext();
    return fmt ? fmt->bit_rate : 0;
}

int64_t Player::GetFileSize() const {
    AVFormatContext* fmt = m_demuxer.GetFormatContext();
    if (!fmt || !fmt->pb) return 0;
    return avio_size(fmt->pb);
}

void Player::SetSpeed(double speed) {
    m_clock.SetSpeed(speed);
    if (m_hasAudio) {
        m_audioOutput.SetSpeed(static_cast<float>(speed));
    }
}

// Map linear slider position (m_volume, 0..1) to perceptual gain via a
// cubic taper. Loudness perception is roughly logarithmic, so a linear
// slider sounds "stuck near full" through most of its travel; cubing the
// position gives a much more useful range — 50% → ~-18 dB, 10% → ~-60 dB.
// m_volume itself stays linear so the UI shows the slider value verbatim
// and saved-prefs round-trip correctly.
static inline float VolumeToGain(float v) {
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v * v * v;
}

void Player::SetVolume(float volume) {
    m_volume = volume;
    if (m_hasAudio && !m_muted)
        m_audioOutput.SetVolume(VolumeToGain(volume));
}

void Player::SetMuted(bool muted) {
    m_muted = muted;
    if (m_hasAudio)
        m_audioOutput.SetVolume(muted ? 0.0f : VolumeToGain(m_volume));
}

// --- Synchronous decode (used when paused) ---

bool Player::SyncDecodeNextFrame() {
    TRACE_EVENT("SyncDecodeNextFrame");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool found = false;
    int maxPackets = 200;
    bool eofHit = false;

    while (!found && maxPackets-- > 0) {
        // First try to receive a frame (decoder may have buffered frames)
        int ret = m_videoDecoder.ReceiveFrame(frame);
        if (ret == 0) {
            found = true;
            break;
        }

        // Need more input — read packets from the demuxer.
        ret = m_demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) {
            m_eof = true;
            eofHit = true;
            break;
        }
        if (ret < 0) break;

        if (pkt->stream_index == m_demuxer.GetVideoStreamIndex()) {
            m_videoDecoder.SendPacket(pkt);
        }
        av_packet_unref(pkt);
    }

    // Hit EOF without producing a frame — the next frame may still be
    // sitting in the decoder pipeline. Drain it out so frame-step-forward
    // can reach the last few frames of the file instead of stalling
    // ~thread_count frames before the end.
    if (eofHit && !found) {
        m_videoDecoder.DrainAtEOF(frame, [&](AVFrame* /*f*/) {
            found = true;
            return false;  // grab the first one and keep its data in `frame`
        });
    }

    if (found) {
        AVRational tb = m_demuxer.GetVideoTimeBase();
        double frameSec = static_cast<double>(frame->pts) * av_q2d(tb);
        m_clock.SetTime(frameSec);
        m_lastDisplayedPts = frame->pts;

        m_cachedFrame = m_frameConverter.Convert(frame);
        m_cachedWidth = m_frameConverter.GetWidth();
        m_cachedHeight = m_frameConverter.GetHeight();
        m_hasCachedFrame = (m_cachedFrame != nullptr);

        if (m_hasCachedFrame)
            m_frameCache.Put(frame->pts, m_cachedFrame, m_cachedWidth, m_cachedHeight);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    return found;
}

bool Player::SyncSeekAndDecode(double targetSec) {
    TRACE_EVENT("SyncSeekAndDecode");
    uint64_t t0 = m_profileSeek ? SDL_GetTicksNS() : 0;

    // Flush decoder state — we're seeking to a new position
    m_videoDecoder.Flush();
    if (m_hasAudio) m_audioDecoder.Flush();

    // Flush queues and frame cache (stale frames from previous position)
    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_videoFrameQueue.Flush();
    m_frameCache.Clear();

    // Clear the audio queue's interrupt flag so we can push audio packets
    // captured during the forward decode below. ParkPipeline left the queues
    // interrupted to wake the workers; while we run here the workers are
    // parked at their gate, but Push still rejects on m_interrupt=true.
    // Workers stay parked because they wait on m_pipelineCv, not the queue.
    m_audioPacketQueue.ClearInterrupt();

    if (m_hasAudio) {
        m_audioOutput.Flush();
        // Position is reset to the actual landed video pts after decode
        // (long-GOP files can land hundreds of ms past target; aligning
        // audio to target instead of the actual frame would desync them).
    }

    uint64_t tFlush = m_profileSeek ? SDL_GetTicksNS() : 0;
    // Seek demuxer to keyframe at or before target
    m_demuxer.Seek(targetSec);
    m_pipelineFlushGen.fetch_add(1, std::memory_order_relaxed);
    m_eof = false;

    // Decode forward until we reach the target PTS
    AVRational tb = m_demuxer.GetVideoTimeBase();
    int64_t targetPts = static_cast<int64_t>(targetSec / av_q2d(tb));

    uint64_t tSeek = m_profileSeek ? SDL_GetTicksNS() : 0;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* bestFrame = nullptr;
    int maxPackets = 5000;
    int videoFrames = 0;
    bool eofHit = false;

    while (maxPackets-- > 0) {
        int ret = m_demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) {
            m_eof = true;
            eofHit = true;
            break;
        }
        if (ret < 0) break;

        if (pkt->stream_index == m_demuxer.GetAudioStreamIndex() && m_hasAudio) {
            // Capture audio packets covering target into the audio queue.
            // Without this, every packet read here gets dropped — by the
            // time SyncSeekAndDecode finishes, the demuxer is hundreds of
            // ms past target, and the audio packets near target are gone.
            // m_audioOutput.ResetPosition(target) would then claim the
            // first post-seek audio sample is at target, but the actual
            // content is from later, so audio plays ahead of video.
            AVRational atb = m_demuxer.GetAudioTimeBase();
            double pktSec = (pkt->pts != AV_NOPTS_VALUE)
                ? static_cast<double>(pkt->pts) * av_q2d(atb) : 0.0;
            // Include the packet straddling target (audio frames are
            // typically ~21ms; allow a small margin so we don't skip the
            // packet whose end is at target).
            if (pktSec + 0.025 >= targetSec &&
                m_audioPacketQueue.Size() < 60) {
                if (!m_audioPacketQueue.Push(pkt)) {
                    av_packet_unref(pkt);
                }
            } else {
                av_packet_unref(pkt);
            }
            continue;
        }

        if (pkt->stream_index != m_demuxer.GetVideoStreamIndex()) {
            av_packet_unref(pkt);
            continue;
        }

        ret = m_videoDecoder.SendPacket(pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (true) {
            ret = m_videoDecoder.ReceiveFrame(frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            videoFrames++;
            if (!bestFrame) {
                bestFrame = av_frame_alloc();
            } else {
                av_frame_unref(bestFrame);
            }
            av_frame_move_ref(bestFrame, frame);

            if (bestFrame->pts >= targetPts) {
                goto done;
            }
        }
    }

    // Drain frames the decoder pipeline still holds after EOF — without
    // this, scrubbing to a target inside the last GOP always returned the
    // pre-tail frame instead of the actual one.
    if (eofHit) {
        m_videoDecoder.DrainAtEOF(frame, [&](AVFrame* f) {
            videoFrames++;
            if (!bestFrame) bestFrame = av_frame_alloc();
            else av_frame_unref(bestFrame);
            av_frame_move_ref(bestFrame, f);
            // Stop at the first frame at or past the target — otherwise
            // we'd keep overwriting bestFrame with later flushed frames
            // and end up at the very last frame regardless of target.
            return bestFrame->pts < targetPts;
        });
    }

done:
    uint64_t tDecode = m_profileSeek ? SDL_GetTicksNS() : 0;

    av_packet_free(&pkt);
    av_frame_free(&frame);

    if (bestFrame) {
        double frameSec = static_cast<double>(bestFrame->pts) * av_q2d(tb);
        // If the user seeked to/past the file's end, set the clock to the
        // declared duration so Play() correctly recognises this as "at end"
        // and restarts from 0 — matching the playback-reaches-EOF behavior.
        // Without this, clock lands on the last frame's pts which is one
        // frame short of duration, so Play resumes from there and the first
        // PollSeekComplete tick instantly pauses (looking like nothing
        // happened).
        double duration = m_demuxer.GetDuration();
        if (duration > 0.0 && targetSec >= duration - 0.001) {
            frameSec = duration;
        }
        m_clock.SetTime(frameSec);
        m_lastDisplayedPts = bestFrame->pts;

        if (m_hasAudio) {
            // Align audio to the actual landed video frame rather than
            // the requested target — long-GOP files can land hundreds of
            // ms past target. Then drop captured audio packets whose pts
            // is earlier than the video frame: their content would play
            // before video catches up, manifesting as audio-leads-video
            // by up to a GOP duration.
            m_audioOutput.ResetPosition(frameSec);

            AVRational atb = m_demuxer.GetAudioTimeBase();
            double atb_d = av_q2d(atb);
            int origCount = m_audioPacketQueue.Size();
            int dropped = 0;
            AVPacket* drainPkt = av_packet_alloc();
            // Pop, filter, re-push. Pop won't block here — pipeline workers
            // are parked, no other consumer.
            std::vector<AVPacket*> keep;
            for (int i = 0; i < origCount; ++i) {
                if (!m_audioPacketQueue.Pop(drainPkt)) break;
                double pktSec = (drainPkt->pts != AV_NOPTS_VALUE)
                    ? static_cast<double>(drainPkt->pts) * atb_d : 0.0;
                if (pktSec + 0.025 >= frameSec) {
                    AVPacket* k = av_packet_alloc();
                    av_packet_move_ref(k, drainPkt);
                    keep.push_back(k);
                } else {
                    av_packet_unref(drainPkt);
                    dropped++;
                }
            }
            for (AVPacket* k : keep) {
                if (!m_audioPacketQueue.Push(k)) av_packet_unref(k);
                av_packet_free(&k);
            }
            av_packet_free(&drainPkt);
            if (m_profileSeek && dropped > 0) {
                LOG_INFO("AVSync: dropped %d audio packets (< video first frame %.3fs)",
                         dropped, frameSec);
            }
        }

        // Only convert the final target frame to RGBA (skip intermediates for speed)
        const uint8_t* rgba = m_frameConverter.Convert(bestFrame);
        if (rgba) {
            m_frameCache.Put(bestFrame->pts, rgba,
                             m_frameConverter.GetWidth(),
                             m_frameConverter.GetHeight());
            m_cachedFrame = m_frameCache.FindExact(bestFrame->pts)->rgba.data();
            m_cachedWidth = m_frameConverter.GetWidth();
            m_cachedHeight = m_frameConverter.GetHeight();
            m_hasCachedFrame = true;
        }

        if (m_profileSeek) {
            uint64_t tConvert = SDL_GetTicksNS();
            LOG_INFO("SyncSeek: flush=%.1fms seek=%.1fms decode=%.1fms(%d frm) rgba=%.1fms total=%.1fms",
                     (tFlush-t0)/1e6, (tSeek-tFlush)/1e6, (tDecode-tSeek)/1e6,
                     videoFrames, (tConvert-tDecode)/1e6, (tConvert-t0)/1e6);
        }

        av_frame_free(&bestFrame);
        return m_hasCachedFrame;
    }

    m_clock.SetTime(targetSec);
    if (m_hasAudio) m_audioOutput.ResetPosition(targetSec);
    return false;
}

bool Player::SyncSeekAndDecodeBefore(double seekSec, int64_t maxPts) {
    TRACE_EVENT("SyncSeekAndDecodeBefore");

    // Flush state (same as SyncSeekAndDecode — we're repositioning).
    m_videoDecoder.Flush();
    if (m_hasAudio) m_audioDecoder.Flush();
    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_videoFrameQueue.Flush();
    m_frameCache.Clear();
    if (m_hasAudio) {
        m_audioOutput.Flush();
        m_audioOutput.ResetPosition(seekSec);
    }

    int attempt = 0;
    double trySec = seekSec;
    AVFrame* bestFrame = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    // Keep decoding a few frames past the first frame with pts >= maxPts so
    // that any B-frames with pts < maxPts that emit after (decode order !=
    // display order) are still captured.
    const int kReorderDepth = 8;

    while (attempt < 4) {
        m_videoDecoder.Flush();
        m_demuxer.Seek(trySec);
        m_pipelineFlushGen.fetch_add(1, std::memory_order_relaxed);
        m_eof = false;

        int maxPackets = 500;
        int framesAtOrAfterMax = 0;

        while (maxPackets-- > 0 && framesAtOrAfterMax < kReorderDepth) {
            int ret = m_demuxer.ReadPacket(pkt);
            if (ret == AVERROR_EOF) { m_eof = true; break; }
            if (ret < 0) break;

            if (pkt->stream_index != m_demuxer.GetVideoStreamIndex()) {
                av_packet_unref(pkt);
                continue;
            }
            ret = m_videoDecoder.SendPacket(pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;

            while (true) {
                ret = m_videoDecoder.ReceiveFrame(frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                if (frame->pts < maxPts) {
                    if (!bestFrame) {
                        bestFrame = av_frame_alloc();
                        av_frame_move_ref(bestFrame, frame);
                    } else if (frame->pts > bestFrame->pts) {
                        av_frame_unref(bestFrame);
                        av_frame_move_ref(bestFrame, frame);
                    } else {
                        av_frame_unref(frame);
                    }
                } else {
                    framesAtOrAfterMax++;
                    av_frame_unref(frame);
                }
            }
        }

        if (bestFrame) break;

        // No frame with pts < maxPts found at this seek point — seek further
        // back and retry. Happens near the head of a GOP when the keyframe
        // itself has pts >= maxPts.
        attempt++;
        trySec -= 1.0;
        if (trySec < 0.0) { trySec = 0.0; attempt = 99; } // last try, then give up
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);

    if (bestFrame) {
        AVRational tb = m_demuxer.GetVideoTimeBase();
        double frameSec = static_cast<double>(bestFrame->pts) * av_q2d(tb);
        m_clock.SetTime(frameSec);
        m_lastDisplayedPts = bestFrame->pts;

        const uint8_t* rgba = m_frameConverter.Convert(bestFrame);
        if (rgba) {
            m_frameCache.Put(bestFrame->pts, rgba,
                             m_frameConverter.GetWidth(),
                             m_frameConverter.GetHeight());
            m_cachedFrame = m_frameCache.FindExact(bestFrame->pts)->rgba.data();
            m_cachedWidth = m_frameConverter.GetWidth();
            m_cachedHeight = m_frameConverter.GetHeight();
            m_hasCachedFrame = true;
        }
        av_frame_free(&bestFrame);
        return m_hasCachedFrame;
    }

    return false;
}

void Player::PopulateCacheAroundCurrent(std::function<bool()> shouldAbort) {
    TRACE_EVENT("PopulateCacheAroundCurrent");
    if (!m_hasMedia) return;
    if (m_playing) return; // only when paused
    // Cache decoder failed to open — skip cache population.
    if (m_cacheDemuxer.GetVideoStreamIndex() < 0) return;

    double currentSec = m_clock.GetTime();
    int64_t currentPts = m_lastDisplayedPts;

    // Use the dedicated cache demuxer + decoder so we don't touch the
    // main pipeline. The main decoder stays warm at lastDisplayedPts and
    // Play() can resume without a re-seek + GOP catchup.
    m_cacheDecoder.Flush();
    m_cacheDemuxer.Seek(currentSec);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int maxPackets = 500;
    bool eofHit = false;

    auto cacheFrame = [&](AVFrame* f) -> bool {
        if (f->pts > currentPts) return false;  // past current — stop
        if (!m_frameCache.FindExact(f->pts)) {
            const uint8_t* rgba = m_cacheConverter.Convert(f);
            if (rgba) {
                m_frameCache.Put(f->pts, rgba,
                                 m_cacheConverter.GetWidth(),
                                 m_cacheConverter.GetHeight());
            }
        }
        return true;
    };

    while (maxPackets-- > 0) {
        if (shouldAbort && shouldAbort()) {
            TRACE_LOG("PopulateCache_Aborted");
            goto cacheDone;
        }
        int ret = m_cacheDemuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) { eofHit = true; break; }
        if (ret < 0) break;

        if (pkt->stream_index != m_cacheDemuxer.GetVideoStreamIndex()) {
            av_packet_unref(pkt);
            continue;
        }

        m_cacheDecoder.SendPacket(pkt);
        av_packet_unref(pkt);

        while (true) {
            ret = m_cacheDecoder.ReceiveFrame(frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            bool keepGoing = cacheFrame(frame);
            av_frame_unref(frame);
            if (!keepGoing) goto cacheDone;
        }
    }

    // Flush the trailing frames the cache decoder pipeline has been
    // buffering into the cache too — otherwise frame-step-backward from
    // the last frame jumps over them.
    if (eofHit) {
        m_cacheDecoder.DrainAtEOF(frame, cacheFrame);
    }

cacheDone:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    // Main decoder is untouched.
}

// --- TryGetVideoFrame (used during playback) ---

bool Player::TryGetVideoFrame(const uint8_t** outRGBA, int* outWidth, int* outHeight) {
    if (!m_hasMedia) return false;

    // Return cached frame from synchronous decode
    if (m_hasCachedFrame) {
        *outRGBA = m_cachedFrame;
        *outWidth = m_cachedWidth;
        *outHeight = m_cachedHeight;
        m_hasCachedFrame = false;
        return true;
    }

    // Only consume from async queue when playing
    if (!m_playing.load(std::memory_order_relaxed)) {
        return false;
    }

    AVRational tb = m_demuxer.GetVideoTimeBase();
    double frameDur = GetFrameDuration();

    // If waiting for resume frame, silently drop frames before resume time
    if (m_waitingForResumeFrame) {
        AVFrame* frame = av_frame_alloc();
        while (true) {
            int64_t pts = m_videoFrameQueue.PeekPts();
            if (pts == AV_NOPTS_VALUE) {
                av_frame_free(&frame);
                return false; // no frames yet, keep waiting
            }
            double frameSec = static_cast<double>(pts) * av_q2d(tb);
            if (frameSec >= m_resumeTime - frameDur) {
                // This frame is at or near our resume point — start playback
                m_waitingForResumeFrame = false;
                if (m_profileSeek && m_playStartNS) {
                    double elapsedMs = (SDL_GetTicksNS() - m_playStartNS) / 1e6;
                    LOG_INFO("Play: ready in %.1fms (resume=%.3fs first=%.3fs catchup=%d frames)",
                             elapsedMs, m_resumeTime, frameSec, m_playDroppedFrames);
                    m_playStartNS = 0;
                }
                m_clock.SetTime(frameSec);
                m_clock.SetPaused(false);
                if (m_hasAudio) m_audioOutput.Resume();
                break;
            }
            // Drop this frame — it's before our resume point
            if (!m_videoFrameQueue.TryPop(frame)) break;
            av_frame_unref(frame);
            if (m_profileSeek) m_playDroppedFrames++;
        }
        av_frame_free(&frame);
        if (m_waitingForResumeFrame) return false;
    }

    double clockSec = m_clock.GetTime();

    AVFrame* frame = av_frame_alloc();
    AVFrame* bestFrame = nullptr;

    while (true) {
        int64_t pts = m_videoFrameQueue.PeekPts();
        if (pts == AV_NOPTS_VALUE) break;

        double frameSec = static_cast<double>(pts) * av_q2d(tb);

        if (frameSec > clockSec + 0.005) {
            break; // too early
        }

        if (!m_videoFrameQueue.TryPop(frame)) break;

        if (bestFrame) {
            av_frame_unref(bestFrame);
        } else {
            bestFrame = av_frame_alloc();
        }
        av_frame_move_ref(bestFrame, frame);

        double late = clockSec - frameSec;
        if (late <= frameDur) break;
    }

    av_frame_free(&frame);

    if (!bestFrame) return false;

    m_lastDisplayedPts = bestFrame->pts;

    const uint8_t* rgba = m_frameConverter.Convert(bestFrame);
    if (!rgba) {
        av_frame_free(&bestFrame);
        return false;
    }

    // Cache the frame for backward stepping
    m_frameCache.Put(bestFrame->pts, rgba,
                     m_frameConverter.GetWidth(),
                     m_frameConverter.GetHeight());
    av_frame_free(&bestFrame);

    *outRGBA = rgba;
    *outWidth = m_frameConverter.GetWidth();
    *outHeight = m_frameConverter.GetHeight();
    return true;
}

// --- Thread management ---

void Player::WaitForSeek() {
    // Spin-wait for the seek thread to finish its current operation.
    // This is only called for operations that need decoder consistency
    // (StepFrame, PopulateCacheAroundCurrent).
    while (m_seekBusy.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Player::SpawnPipelineThreads() {
    TRACE_EVENT("SpawnPipelineThreads");
    {
        std::lock_guard<std::mutex> lk(m_pipelineMutex);
        m_pipelineActive = false;
        m_pipelineParkedCount = 0;
        m_pipelineExit = false;
        m_pipelineThreadCount = m_hasAudio ? 3 : 2;
    }
    m_videoPacketQueue.Reset();
    m_audioPacketQueue.Reset();
    m_videoFrameQueue.Reset();

    m_demuxThread = std::thread(&Player::DemuxThread, this);
    m_videoDecodeThread = std::thread(&Player::VideoDecodeThread, this);
    if (m_hasAudio) {
        m_audioDecodeThread = std::thread(&Player::AudioDecodeThread, this);
    }
}

void Player::StopPipelineThreads() {
    TRACE_EVENT("StopPipelineThreads");
    {
        std::lock_guard<std::mutex> lk(m_pipelineMutex);
        m_pipelineExit = true;
        m_pipelineActive = false;
    }
    m_pipelineCv.notify_all();
    m_videoPacketQueue.Abort();
    m_audioPacketQueue.Abort();
    m_videoFrameQueue.Abort();

    if (m_demuxThread.joinable()) m_demuxThread.join();
    if (m_videoDecodeThread.joinable()) m_videoDecodeThread.join();
    if (m_audioDecodeThread.joinable()) m_audioDecodeThread.join();
}

void Player::ParkPipeline() {
    TRACE_EVENT("ParkPipeline");
    {
        std::lock_guard<std::mutex> lk(m_pipelineMutex);
        if (!m_pipelineActive && m_pipelineParkedCount == m_pipelineThreadCount) {
            // Already parked.
            return;
        }
        m_pipelineActive = false;
    }
    // Notify any thread in an EOF or I/O backoff cv wait so it re-evaluates
    // its predicate (which includes !m_pipelineActive) and parks promptly.
    m_pipelineCv.notify_all();
    // Wake any thread mid-Push/Pop so it returns false and falls through to
    // the top-of-loop park gate.
    m_videoPacketQueue.Interrupt();
    m_audioPacketQueue.Interrupt();
    m_videoFrameQueue.Interrupt();

    std::unique_lock<std::mutex> lk(m_pipelineMutex);
    m_pipelineParkedCv.wait(lk, [&] {
        return m_pipelineParkedCount == m_pipelineThreadCount || m_pipelineExit;
    });
}

void Player::UnparkPipeline() {
    TRACE_EVENT("UnparkPipeline");
    m_videoPacketQueue.ClearInterrupt();
    m_audioPacketQueue.ClearInterrupt();
    m_videoFrameQueue.ClearInterrupt();
    {
        std::lock_guard<std::mutex> lk(m_pipelineMutex);
        m_pipelineActive = true;
    }
    m_pipelineCv.notify_all();
}

bool Player::WorkerParkOrExit() {
    std::unique_lock<std::mutex> lk(m_pipelineMutex);
    if (m_pipelineExit) return false;
    if (m_pipelineActive) return true;

    m_pipelineParkedCount++;
    m_pipelineParkedCv.notify_all();
    m_pipelineCv.wait(lk, [&] { return m_pipelineActive || m_pipelineExit; });
    m_pipelineParkedCount--;
    return !m_pipelineExit;
}

void Player::DemuxThread() {
    SetCurrentThreadName(L"ScrubCut Demux");
    AVPacket* pkt = av_packet_alloc();
    AVPacket* held = nullptr;          // packet awaiting push (preserved across pause)
    uint64_t  heldGen = 0;

    while (true) {
        if (!WorkerParkOrExit()) break;

        // If a flush happened while we were parked, the held packet is from
        // the previous demuxer position — discard it.
        if (held && m_pipelineFlushGen.load(std::memory_order_relaxed) != heldGen) {
            av_packet_free(&held);
            held = nullptr;
        }

        // EOF idle. Poll: m_eof gets cleared by Seek's flush. Short timed
        // wait so we don't busy-loop and so Park can wake us promptly.
        if (m_eof.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lk(m_pipelineMutex);
            m_pipelineCv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return m_pipelineExit || !m_pipelineActive ||
                       !m_eof.load(std::memory_order_relaxed);
            });
            continue;
        }

        // Try to deliver any held packet first — preserves bitstream
        // continuity across pause (if Push got interrupted previously).
        if (held) {
            int idx = held->stream_index;
            bool pushed = false;
            if (idx == m_demuxer.GetVideoStreamIndex()) {
                pushed = m_videoPacketQueue.Push(held);
            } else if (idx == m_demuxer.GetAudioStreamIndex()) {
                pushed = m_audioPacketQueue.Push(held);
            } else {
                av_packet_unref(held);
                pushed = true;
            }
            if (pushed) {
                av_packet_free(&held);
                held = nullptr;
            } else {
                continue; // park requested; top-of-loop handles
            }
        }

        int ret = m_demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) {
            m_eof = true;
            continue;
        }
        if (ret < 0) {
            // I/O error — short backoff
            std::unique_lock<std::mutex> lk(m_pipelineMutex);
            m_pipelineCv.wait_for(lk, std::chrono::milliseconds(50));
            continue;
        }

        int idx = pkt->stream_index;
        bool isVideo = (idx == m_demuxer.GetVideoStreamIndex());
        bool isAudio = (idx == m_demuxer.GetAudioStreamIndex());
        if (!isVideo && !isAudio) {
            av_packet_unref(pkt);
            continue;
        }

        bool pushed = isVideo ? m_videoPacketQueue.Push(pkt)
                              : m_audioPacketQueue.Push(pkt);
        if (!pushed) {
            // Park requested while waiting on Push. Save for redelivery.
            held = av_packet_alloc();
            av_packet_move_ref(held, pkt);
            heldGen = m_pipelineFlushGen.load(std::memory_order_relaxed);
        }
    }

    if (held) av_packet_free(&held);
    av_packet_free(&pkt);
}

void Player::VideoDecodeThread() {
    SetCurrentThreadName(L"ScrubCut VideoDecode");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool drainedEof = false;

    while (true) {
        if (!WorkerParkOrExit()) break;

        // Re-arm EOF drain whenever m_eof goes back to false (post-seek).
        if (!m_eof.load(std::memory_order_relaxed)) drainedEof = false;

        // EOF idle: drain decoder once, then poll until state changes.
        if (m_eof.load(std::memory_order_relaxed) && m_videoPacketQueue.Empty()) {
            if (!drainedEof) {
                drainedEof = true;
                m_videoDecoder.DrainAtEOF(frame, [&](AVFrame* f) {
                    return m_videoFrameQueue.Push(f);
                });
            }
            std::unique_lock<std::mutex> lk(m_pipelineMutex);
            m_pipelineCv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return m_pipelineExit || !m_pipelineActive ||
                       !m_eof.load(std::memory_order_relaxed) ||
                       !m_videoPacketQueue.Empty();
            });
            continue;
        }

        // Use timed pop so we periodically re-check EOF and park state
        // even when DemuxThread is briefly behind.
        if (!m_videoPacketQueue.PopWithTimeout(pkt, 50)) continue;

        int ret = m_videoDecoder.SendPacket(pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (true) {
            ret = m_videoDecoder.ReceiveFrame(frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            if (!m_videoFrameQueue.Push(frame)) {
                av_frame_unref(frame);
                break;  // park requested; top-of-loop handles
            }
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
}

void Player::AudioDecodeThread() {
    SetCurrentThreadName(L"ScrubCut AudioDecode");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint8_t* outBuf = nullptr;
    int outBufSize = 0;

    while (true) {
        if (!WorkerParkOrExit()) break;

        if (!m_audioPacketQueue.PopWithTimeout(pkt, 50)) continue;

        if (m_avsyncLogPackets.load(std::memory_order_relaxed) > 0) {
            int n = m_avsyncLogPackets.fetch_sub(1, std::memory_order_relaxed);
            if (n > 0) {
                AVRational atb = m_demuxer.GetAudioTimeBase();
                double pktSec = (pkt->pts != AV_NOPTS_VALUE)
                    ? static_cast<double>(pkt->pts) * av_q2d(atb) : -1.0;
                double tgt = m_avsyncSeekTarget.load(std::memory_order_relaxed);
                double pos = m_audioOutput.GetPlaybackPosition();
                LOG_INFO("AVSync: audio pkt pts=%.3fs target=%.3fs (delta=%+.3fs) audio_clock=%.3fs",
                         pktSec, tgt, pktSec - tgt, pos);
            }
        }

        int ret = m_audioDecoder.SendPacket(pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (true) {
            ret = m_audioDecoder.ReceiveFrame(frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            if (m_swrCtx) {
                int outSamples = swr_get_out_samples(m_swrCtx, frame->nb_samples);
                int outBytes = outSamples * m_audioOutput.GetChannels() * 2;
                if (outBytes > outBufSize) {
                    av_free(outBuf);
                    outBufSize = outBytes;
                    outBuf = static_cast<uint8_t*>(av_malloc(outBufSize));
                }

                uint8_t* outPtrs[1] = { outBuf };
                int converted = swr_convert(m_swrCtx, outPtrs, outSamples,
                                             const_cast<const uint8_t**>(frame->data),
                                             frame->nb_samples);
                if (converted > 0) {
                    int size = converted * m_audioOutput.GetChannels() * 2;
                    m_audioOutput.Write(outBuf, size);
                }
            }

            av_frame_unref(frame);
        }
    }

    av_free(outBuf);
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

void Player::FlushPipelineState() {
    m_videoDecoder.Flush();
    if (m_hasAudio) m_audioDecoder.Flush();
    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_videoFrameQueue.Flush();
    m_pipelineFlushGen.fetch_add(1, std::memory_order_relaxed);
}

bool Player::TickPipelineOneFrame(int timeoutMs) {
    UnparkPipeline();
    bool got = m_videoFrameQueue.WaitForOne(timeoutMs);
    ParkPipeline();
    return got;
}

void Player::SetupResampler() {
    CloseResampler();

    AVCodecParameters* par = m_demuxer.GetAudioCodecParams();
    if (!par) return;

    int ret = swr_alloc_set_opts2(&m_swrCtx,
        &par->ch_layout, AV_SAMPLE_FMT_S16, par->sample_rate,
        &par->ch_layout, static_cast<AVSampleFormat>(par->format), par->sample_rate,
        0, nullptr);

    if (ret < 0 || !m_swrCtx) {
        LOG_ERROR("swr_alloc_set_opts2 failed");
        return;
    }

    ret = swr_init(m_swrCtx);
    if (ret < 0) {
        LOG_ERROR("swr_init failed: %s", ff::ErrorString(ret).c_str());
        CloseResampler();
    }
}

void Player::CloseResampler() {
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
}
