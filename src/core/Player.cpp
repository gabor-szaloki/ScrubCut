#include "core/Player.h"
#include "util/Log.h"
#include "util/Trace.h"

#include <algorithm>

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

    m_hasAudio = false;
    if (m_demuxer.GetAudioStreamIndex() >= 0) {
        if (m_audioDecoder.Open(m_demuxer.GetAudioCodecParams())) {
            int sr = m_audioDecoder.GetSampleRate();
            int ch = m_audioDecoder.GetChannels();
            if (m_audioOutput.Open(sr, ch)) {
                SetupResampler();
                m_hasAudio = true;
                m_clock.SetAudioOutput(&m_audioOutput);
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

    // Decode first frame synchronously so we have something to show
    SyncDecodeNextFrame();

    return true;
}

void Player::Close() {
    StopThreads();
    m_frameCache.Clear();
    CloseResampler();

    m_audioOutput.Close();
    m_audioDecoder.Close();
    m_videoDecoder.Close();
    m_demuxer.Close();

    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_videoFrameQueue.Flush();

    m_hasMedia = false;
    m_hasAudio = false;
    m_playing = false;
    m_eof = false;
    m_lastDisplayedPts = AV_NOPTS_VALUE;
    m_hasCachedFrame = false;
}

void Player::Play() {
    TRACE_EVENT("Player::Play");
    if (!m_hasMedia) return;
    if (m_eof) {
        // Restart from beginning
        EnsureThreadsStopped();
        m_videoDecoder.Flush();
        if (m_hasAudio) m_audioDecoder.Flush();
        m_demuxer.Seek(0.0);
        m_clock.SetTime(0.0);
        m_eof = false;
    }

    double resumeTime = m_clock.GetTime();

    m_hasCachedFrame = false;

    // Start threads if not running
    if (!m_threadsRunning) {
        m_videoPacketQueue.Flush();
        m_audioPacketQueue.Flush();
        m_videoFrameQueue.Flush();
        m_videoDecoder.Flush();
        if (m_hasAudio) m_audioDecoder.Flush();

        m_demuxer.Seek(resumeTime);
        StartThreads();
    }

    if (m_hasAudio) {
        m_audioOutput.Flush();
        m_audioOutput.ResetPosition(resumeTime);
    }

    // Don't start the clock yet — wait for the first frame at/after resumeTime
    // to arrive from the decode threads. This prevents fast-forward catch-up.
    m_clock.SetTime(resumeTime);
    m_waitingForResumeFrame = true;
    m_resumeTime = resumeTime;
    m_playing = true;
    // Clock stays paused; TryGetVideoFrame will unpause it when ready.
}

void Player::Pause() {
    TRACE_EVENT("Player::Pause");
    bool wasPlaying = m_playing.load(std::memory_order_relaxed);
    m_playing = false;
    m_waitingForResumeFrame = false;
    m_clock.SetPaused(true);
    if (m_hasAudio) m_audioOutput.Pause();

    if (wasPlaying) {
        // Stop threads — we'll do synchronous decode while paused
        StopThreads();
        // Drain any frames left in the queue into decoder state
        // (so SyncDecodeNextFrame picks up where threads left off)
    }
}

void Player::TogglePlayPause() {
    if (m_playing) Pause();
    else Play();
}

void Player::SeekTo(double seconds) {
    TRACE_EVENT("Player::SeekTo");
    if (!m_hasMedia) return;
    double duration = m_demuxer.GetDuration();
    seconds = std::clamp(seconds, 0.0, duration);

    bool wasPlaying = m_playing.load(std::memory_order_relaxed);
    if (wasPlaying) Pause();

    EnsureThreadsStopped();
    SyncSeekAndDecode(seconds);

    if (wasPlaying) Play();
}

void Player::SeekRelative(double deltaSec) {
    double target = m_clock.GetTime() + deltaSec;
    SeekTo(target);
}

void Player::StepFrame(int direction) {
    TRACE_EVENT("Player::StepFrame");
    if (!m_hasMedia) return;

    bool wasPlaying = m_playing.load(std::memory_order_relaxed);
    if (wasPlaying) Pause();

    EnsureThreadsStopped();

    if (direction > 0) {
        // Forward: check if next frame is already in cache
        if (m_lastDisplayedPts != AV_NOPTS_VALUE) {
            AVRational tb = m_demuxer.GetVideoTimeBase();
            double frameDurSec = GetFrameDuration();
            int64_t nextPts = m_lastDisplayedPts + static_cast<int64_t>(frameDurSec / av_q2d(tb));
            const auto* cached = m_frameCache.FindNearest(nextPts);
            // Accept if within half a frame duration
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
                m_decoderDirty = true;
                goto stepDone;
            }
        }

        // If decoder is out of sync (e.g. after cache-hit backward step), resync it
        if (m_decoderDirty) {
            double currentSec = m_clock.GetTime();
            m_videoDecoder.Flush();
            m_videoFrameQueue.Flush();
            m_videoPacketQueue.Flush();
            m_demuxer.Seek(currentSec);
            // Decode forward to current position, then one more
            AVRational tb = m_demuxer.GetVideoTimeBase();
            int64_t currentPts = m_lastDisplayedPts;
            AVPacket* pkt = av_packet_alloc();
            AVFrame* tmpFrame = av_frame_alloc();
            int limit = 500;
            while (limit-- > 0) {
                int ret = m_demuxer.ReadPacket(pkt);
                if (ret < 0) break;
                if (pkt->stream_index != m_demuxer.GetVideoStreamIndex()) {
                    av_packet_unref(pkt);
                    continue;
                }
                m_videoDecoder.SendPacket(pkt);
                av_packet_unref(pkt);
                while (true) {
                    ret = m_videoDecoder.ReceiveFrame(tmpFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;
                    if (tmpFrame->pts > currentPts) {
                        // This is the next frame after current
                        double frameSec = static_cast<double>(tmpFrame->pts) * av_q2d(tb);
                        m_clock.SetTime(frameSec);
                        m_lastDisplayedPts = tmpFrame->pts;
                        m_cachedFrame = m_frameConverter.Convert(tmpFrame);
                        m_cachedWidth = m_frameConverter.GetWidth();
                        m_cachedHeight = m_frameConverter.GetHeight();
                        m_hasCachedFrame = (m_cachedFrame != nullptr);
                        if (m_hasCachedFrame)
                            m_frameCache.Put(tmpFrame->pts, m_cachedFrame, m_cachedWidth, m_cachedHeight);
                        av_frame_unref(tmpFrame);
                        av_frame_free(&tmpFrame);
                        av_packet_free(&pkt);
                        m_decoderDirty = false;
                        goto stepDone;
                    }
                    av_frame_unref(tmpFrame);
                }
            }
            av_frame_free(&tmpFrame);
            av_packet_free(&pkt);
            m_decoderDirty = false;
        } else {
            // Decoder is in sync — just decode next frame
            AVFrame* frame = av_frame_alloc();
            if (m_videoFrameQueue.TryPop(frame)) {
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
            } else {
                SyncDecodeNextFrame();
            }
            av_frame_free(&frame);
        }
    stepDone:;
    } else {
        // Backward: check frame cache first (instant)
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
                m_decoderDirty = true; // decoder is at wrong position
                cacheHit = true;
            }
        }

        if (!cacheHit) {
            // Cache miss — need to seek and decode
            TRACE_LOG("StepBack_CacheMiss");
            double frameDur = GetFrameDuration();
            double targetSec = m_clock.GetTime() - frameDur;
            if (targetSec < 0.0) targetSec = 0.0;
            SyncSeekAndDecode(targetSec);
        }
    }
}

double Player::GetFrameDuration() const {
    double fps = m_demuxer.GetVideoFrameRate();
    return (fps > 0) ? 1.0 / fps : 1.0 / 30.0;
}

void Player::SetSpeed(double speed) {
    m_clock.SetSpeed(speed);
    if (m_hasAudio) {
        m_audioOutput.SetSpeed(static_cast<float>(speed));
    }
}

// --- Synchronous decode (used when paused) ---

bool Player::SyncDecodeNextFrame() {
    TRACE_EVENT("SyncDecodeNextFrame");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool found = false;
    int maxPackets = 200;

    while (!found && maxPackets-- > 0) {
        // First try to receive a frame (decoder may have buffered frames)
        int ret = m_videoDecoder.ReceiveFrame(frame);
        if (ret == 0) {
            found = true;
            break;
        }

        // Need more input — read packets
        ret = m_demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) {
            m_eof = true;
            break;
        }
        if (ret < 0) break;

        if (pkt->stream_index == m_demuxer.GetVideoStreamIndex()) {
            m_videoDecoder.SendPacket(pkt);
        }
        av_packet_unref(pkt);
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
    // Flush decoder state — we're seeking to a new position
    m_videoDecoder.Flush();
    if (m_hasAudio) m_audioDecoder.Flush();

    // Flush queues
    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_videoFrameQueue.Flush();

    if (m_hasAudio) {
        m_audioOutput.Flush();
        m_audioOutput.ResetPosition(targetSec);
    }

    // Seek demuxer to keyframe at or before target
    m_demuxer.Seek(targetSec);
    m_eof = false;
    m_decoderDirty = false;

    // Decode forward until we reach the target PTS
    AVRational tb = m_demuxer.GetVideoTimeBase();
    int64_t targetPts = static_cast<int64_t>(targetSec / av_q2d(tb));

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* bestFrame = nullptr;
    int maxPackets = 500;

    while (maxPackets-- > 0) {
        int ret = m_demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) {
            m_eof = true;
            break;
        }
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

            // Cache every intermediate frame for future backward stepping
            const uint8_t* rgba = m_frameConverter.Convert(frame);
            if (rgba) {
                m_frameCache.Put(frame->pts, rgba,
                                 m_frameConverter.GetWidth(),
                                 m_frameConverter.GetHeight());
            }

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

done:
    av_packet_free(&pkt);
    av_frame_free(&frame);

    if (bestFrame) {
        double frameSec = static_cast<double>(bestFrame->pts) * av_q2d(tb);
        m_clock.SetTime(frameSec);
        m_lastDisplayedPts = bestFrame->pts;

        // The final frame is already in the cache from the loop above.
        // Set cached frame from the cache entry to avoid double-convert.
        const auto* entry = m_frameCache.FindExact(bestFrame->pts);
        if (entry) {
            m_cachedFrame = entry->rgba.data();
            m_cachedWidth = entry->width;
            m_cachedHeight = entry->height;
            m_hasCachedFrame = true;
        }

        av_frame_free(&bestFrame);
        return m_hasCachedFrame;
    }

    m_clock.SetTime(targetSec);
    return false;
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
                m_clock.SetTime(frameSec);
                m_clock.SetPaused(false);
                if (m_hasAudio) m_audioOutput.Resume();
                break;
            }
            // Drop this frame — it's before our resume point
            if (!m_videoFrameQueue.TryPop(frame)) break;
            av_frame_unref(frame);
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
    av_frame_free(&bestFrame);

    if (!rgba) return false;

    *outRGBA = rgba;
    *outWidth = m_frameConverter.GetWidth();
    *outHeight = m_frameConverter.GetHeight();
    return true;
}

// --- Thread management ---

void Player::EnsureThreadsStopped() {
    if (m_threadsRunning) {
        StopThreads();
    }
}

void Player::StartThreads() {
    TRACE_EVENT("StartThreads");
    m_stopThreads = false;
    m_videoPacketQueue.Reset();
    m_audioPacketQueue.Reset();
    m_videoFrameQueue.Reset();

    m_demuxThread = std::thread(&Player::DemuxThread, this);
    m_videoDecodeThread = std::thread(&Player::VideoDecodeThread, this);
    if (m_hasAudio) {
        m_audioDecodeThread = std::thread(&Player::AudioDecodeThread, this);
    }
    m_threadsRunning = true;
}

void Player::StopThreads() {
    TRACE_EVENT("StopThreads");
    if (!m_threadsRunning) return;

    m_stopThreads = true;
    m_videoPacketQueue.Abort();
    m_audioPacketQueue.Abort();
    m_videoFrameQueue.Abort();

    if (m_demuxThread.joinable()) m_demuxThread.join();
    if (m_videoDecodeThread.joinable()) m_videoDecodeThread.join();
    if (m_audioDecodeThread.joinable()) m_audioDecodeThread.join();

    m_threadsRunning = false;
}

void Player::DemuxThread() {
    SetCurrentThreadName(L"ScrubCut Demux");
    AVPacket* pkt = av_packet_alloc();
    while (!m_stopThreads) {
        int ret = m_demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) {
            m_eof = true;
            m_videoPacketQueue.Abort();
            m_audioPacketQueue.Abort();
            break;
        }
        if (ret < 0) break;

        if (pkt->stream_index == m_demuxer.GetVideoStreamIndex()) {
            if (!m_videoPacketQueue.Push(pkt)) {
                av_packet_unref(pkt);
                break;
            }
        } else if (pkt->stream_index == m_demuxer.GetAudioStreamIndex()) {
            if (!m_audioPacketQueue.Push(pkt)) {
                av_packet_unref(pkt);
                break;
            }
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
}

void Player::VideoDecodeThread() {
    SetCurrentThreadName(L"ScrubCut VideoDecode");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    while (!m_stopThreads) {
        if (!m_videoPacketQueue.Pop(pkt)) break;

        int ret = m_videoDecoder.SendPacket(pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (!m_stopThreads) {
            ret = m_videoDecoder.ReceiveFrame(frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            if (!m_videoFrameQueue.Push(frame)) {
                av_frame_unref(frame);
                goto done;
            }
        }
    }

done:
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

void Player::AudioDecodeThread() {
    SetCurrentThreadName(L"ScrubCut AudioDecode");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint8_t* outBuf = nullptr;
    int outBufSize = 0;

    while (!m_stopThreads) {
        if (!m_audioPacketQueue.Pop(pkt)) break;

        int ret = m_audioDecoder.SendPacket(pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (!m_stopThreads) {
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
