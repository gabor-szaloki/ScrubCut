#include "core/Player.h"
#include "util/Log.h"
#include "util/Profiler.h"

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static void SetCurrentThreadName(const wchar_t* name) {
    SetThreadDescription(GetCurrentThread(), name);
}
#else
static void SetCurrentThreadName(const wchar_t*) {}
#endif

namespace {

// Human-readable color-primaries (gamut) name for the timeline label. Never
// empty: unspecified/unrecognized values map to "Unknown" so the label always
// carries gamut information instead of silently dropping it.
const char* PrimariesLabel(int pri) {
    switch (pri) {
        case AVCOL_PRI_BT2020:    return "BT.2020";
        case AVCOL_PRI_BT709:     return "BT.709";
        case AVCOL_PRI_BT470BG:   // PAL / 625-line
        case AVCOL_PRI_SMPTE170M: // NTSC / 525-line
        case AVCOL_PRI_SMPTE240M: return "BT.601";
        case AVCOL_PRI_SMPTE432:  return "Display P3"; // P3-D65
        case AVCOL_PRI_SMPTE431:  return "DCI-P3";      // P3-DCI
        case AVCOL_PRI_BT470M:    return "BT.470M";
        case AVCOL_PRI_FILM:      return "Film";
        default:                  return "Unknown";
    }
}

// Human-readable transfer (EOTF/OETF) name, or "" when unspecified/unrecognized
// (the label omits the transfer in that case).
const char* TransferLabel(int trc) {
    switch (trc) {
        case AVCOL_TRC_SMPTE2084:    return "PQ";
        case AVCOL_TRC_ARIB_STD_B67: return "HLG";
        case AVCOL_TRC_BT709:        return "BT.709";
        case AVCOL_TRC_IEC61966_2_1: return "sRGB";
        case AVCOL_TRC_LINEAR:       return "Linear";
        case AVCOL_TRC_GAMMA22:      return "gamma 2.2";
        case AVCOL_TRC_GAMMA28:      return "gamma 2.8";
        case AVCOL_TRC_SMPTE170M:    return "BT.601";
        case AVCOL_TRC_SMPTE240M:    return "SMPTE 240M";
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12:    return "BT.2020";
        case AVCOL_TRC_SMPTE428:     return "SMPTE 428";
        case AVCOL_TRC_LOG:          return "Log";
        case AVCOL_TRC_LOG_SQRT:     return "Log sqrt";
        case AVCOL_TRC_BT1361_ECG:   return "BT.1361";
        case AVCOL_TRC_IEC61966_2_4: return "xvYCC";
        default:                     return "";
    }
}

} // namespace

Player::Player() = default;

Player::~Player() {
    Close();
}

bool Player::Open(const std::string& path) {
    PROFILE_SCOPE();
    Close();

    if (!m_demuxer.Open(path))
        return false;

    if (!m_videoDecoder.Open(m_demuxer.GetVideoCodecParams()))
        return false;

    // Determine HDR vs SDR from the stream's transfer characteristic so the
    // renderer can pick the right texture format before the first frame lands.
    if (AVCodecParameters* vpar = m_demuxer.GetVideoCodecParams()) {
        m_videoColorMode = FrameConverter::ColorModeForTransfer(vpar->color_trc);
        m_videoColorPrimaries = FrameConverter::PrimariesForTag(vpar->color_primaries);

        // Build the timeline colorspace label: "<primaries>[ <transfer>] (<tag>)".
        // Primaries are always present (Unknown if unspecified). The transfer is
        // appended when known and not already named by the primaries, so e.g.
        // BT.709 primaries + BT.709 transfer reads "BT.709 (SDR)" rather than
        // doubling up, while BT.709 + sRGB stays "BT.709 sRGB (SDR)".
        const char* prim = PrimariesLabel(vpar->color_primaries);
        const char* trc = TransferLabel(vpar->color_trc);
        std::string label = prim;
        if (trc[0] != '\0' && label != trc)
            label += std::string(" ") + trc;
        const bool hdr = (m_videoColorMode != VideoColorMode::SDR);
        label += hdr ? " (HDR)" : " (SDR)";
        m_videoColorSpaceLabel = std::move(label);
    }

    m_chapters.clear();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
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

        // Enumerate audio + subtitle streams for the Media menu. Same read-only
        // metadata pattern as chapters: built once here, cleared in Close().
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* st = fmt->streams[i];
            AVCodecParameters* par = st->codecpar;
            auto getTag = [&](const char* key) -> std::string {
                AVDictionaryEntry* e = av_dict_get(st->metadata, key, nullptr, 0);
                return (e && e->value) ? e->value : std::string();
            };

            if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
                AudioTrackInfo a;
                a.streamIndex = static_cast<int>(i);
                a.language = getTag("language");
                a.codecName = avcodec_get_name(par->codec_id);
                a.channels = par->ch_layout.nb_channels;
                std::string title = getTag("title");
                // Technical info (codec + channels) is always shown; language is
                // included when tagged. With a title, append it parenthetically;
                // without, use the language (or "Audio") as the name.
                std::string info = a.codecName;
                if (a.channels > 0) info += ", " + std::to_string(a.channels) + "ch";
                if (!title.empty()) {
                    std::string detail = a.language.empty() ? info : (a.language + ", " + info);
                    a.title = title + " (" + detail + ")";
                } else {
                    a.title = (a.language.empty() ? std::string("Audio") : a.language)
                              + " (" + info + ")";
                }
                m_audioTracks.push_back(std::move(a));
            } else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                SubtitleTrackInfo s;
                s.streamIndex = static_cast<int>(i);
                s.language = getTag("language");
                switch (par->codec_id) {
                    case AV_CODEC_ID_DVD_SUBTITLE:
                    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
                    case AV_CODEC_ID_DVB_SUBTITLE:
                    case AV_CODEC_ID_XSUB:
                        s.textBased = false;
                        break;
                    default:
                        s.textBased = true;
                        break;
                }
                std::string title = getTag("title");
                if (!title.empty()) {
                    s.title = title;
                } else {
                    s.title = s.language.empty()
                        ? ("Track " + std::to_string(m_subtitleTracks.size() + 1))
                        : s.language;
                }
                if (!s.textBased) s.title += " [bitmap]";
                m_subtitleTracks.push_back(std::move(s));
            }
        }
    }

    // Independent demuxer + decoder for backward-step cache population.
    // Failure here is non-fatal — caching just falls back to no-op and
    // backward stepping does a full re-seek per step like before.
    if (!m_cacheDemuxer.Open(path, "frame cache") ||
        !m_cacheDecoder.Open(m_cacheDemuxer.GetVideoCodecParams(), /*quiet=*/true)) {
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
                // Apply volume/mute state that may have been set BEFORE
                // audio existed (e.g. from saved preferences at launch).
                // Without this, a saved muted=true never reaches the audio
                // output until the user toggles mute again.
                SetMuted(m_muted);
            }
        }
    }

    m_hasMedia = true;
    m_eof = false;
    m_lastDisplayedPts = AV_NOPTS_VALUE;
    ClearStagedFrame();
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

    // Profiler phases: what's open, and the initial (paused) transport state.
    size_t nameAt = path.find_last_of("/\\");
    m_profMediaSectionId = PROFILE_SECTION_ENTER(Profiler::kSectionMedia, "Video: %s",
        path.c_str() + (nameAt == std::string::npos ? 0 : nameAt + 1));
    SetTransportSection("Paused");

    return true;
}

// Switch the profiler's transport section (nullptr = none). Main thread only;
// unchanged states are skipped so repeated Pause calls don't split it. The
// playback speed is part of the Playing label ("Playing 2x"), so a speed
// change during playback starts a new section segment.
void Player::SetTransportSection(const char* label) {
    const bool playing = label && strcmp(label, "Playing") == 0;
    const double speed = playing ? m_clock.GetSpeed() : 0.0;
    if (m_profTransportLabel && label && strcmp(m_profTransportLabel, label) == 0 &&
        speed == m_profTransportSpeed)
        return;
    m_profTransportLabel = label;
    m_profTransportSpeed = speed;
    PROFILE_SECTION_LEAVE(m_profTransportSectionId);
    if (!label) {
        m_profTransportSectionId = 0;
        return;
    }
    m_profTransportSectionId = (playing && speed != 1.0)
        ? PROFILE_SECTION_ENTER(Profiler::kSectionTransport, "Playing %gx", speed)
        : PROFILE_SECTION_ENTER(Profiler::kSectionTransport, "%s", label);
}

void Player::Close() {
    PROFILE_SCOPE();
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
    m_decodedFrameQueue.Flush();
    m_videoFrameQueue.Flush();

    SetTransportSection(nullptr);
    PROFILE_SECTION_LEAVE(m_profMediaSectionId);
    m_profMediaSectionId = 0;

    m_hasMedia = false;
    m_hasAudio = false;
    m_videoColorMode = VideoColorMode::SDR;
    m_videoColorPrimaries = VideoColorPrimaries::BT2020;
    m_videoColorSpaceLabel.clear();
    m_playing = false;
    m_eof = false;
    m_lastDisplayedPts = AV_NOPTS_VALUE;
    ClearStagedFrame();
    m_displayedFrame.reset();
    m_dropFramesBeforeSec.store(kNeverDrop, std::memory_order_relaxed);
    m_cacheWindowCenter.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
    m_cacheRequest = false;  // seek thread already joined above
    m_cacheBofKf = AV_NOPTS_VALUE;
    m_cacheEofSeen = false;
    m_audioHold = false;
    {
        std::lock_guard<std::mutex> lock(m_framePoolMutex);
        m_framePool.clear();
    }
    m_chapters.clear();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
}

void Player::Play() {
    PROFILE_SCOPE();
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
    ClearStagedFrame();

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
        // Resume in place. The stream still holds the decoded-ahead audio
        // that follows the pause point — flushing it here (the old behavior)
        // discarded that content and made audio resume up to the demux lead
        // (~2s of content) ahead of video, compounding every pause/resume
        // cycle. When the clock agrees with the audio position (plain
        // pause -> play), touch nothing and let the buffer play on.
        double audioPos = m_audioOutput.GetPlaybackPosition();
        double gap = resumeTime - audioPos;
        if (std::fabs(gap) > 0.040) {
            if (gap > 0.0 && m_audioOutput.DiscardUntil(resumeTime)) {
                // Clock moved forward within buffered content (frame steps):
                // the stream head was consumed up to resumeTime, the rest
                // plays on in sync.
            } else {
                // Content at resumeTime isn't in the stream (restart from 0,
                // or stepped past the buffered end): rebuild from the packet
                // queue, trimming decoded samples up to the resume point.
                m_audioDecoder.Flush();
                m_audioOutput.Flush();
                m_audioOutput.ResetPosition(resumeTime);
                m_audioPacketQueue.ClearInterrupt();
                FilterAudioQueueBefore(resumeTime);
                m_audioSkipUntil.store(resumeTime, std::memory_order_relaxed);
            }
        }
    }

    // Don't start the clock yet — wait for the first frame at/after
    // resumeTime to land in the queue. This prevents fast-forward catch-up.
    m_clock.SetTime(resumeTime);
    m_waitingForResumeFrame = true;
    m_resumeTime = resumeTime;
    // Frames before the resume point get dropped on arrival — let the
    // convert stage skip them (mirrors the resume gate in TryGetVideoFrame,
    // which keeps frames >= resumeTime - frameDur).
    m_dropFramesBeforeSec.store(resumeTime - GetFrameDuration(), std::memory_order_relaxed);
    m_audioHold = false;
    m_playing = true;
    m_pendingSeekTarget.store(-1.0, std::memory_order_relaxed);

    // Unpark workers — they were idle since the last Pause/Seek, with
    // queues + decoder state intact. No flush, no seek. TryGetVideoFrame
    // picks up the first frame at-or-past resumeTime.
    UnparkPipeline();
    // Clock stays paused; TryGetVideoFrame will unpause it when ready.

    SetTransportSection("Playing");
}

void Player::Pause(bool populateCache) {
    PROFILE_SCOPE();
    m_playing = false;
    m_dropFramesBeforeSec.store(kNeverDrop, std::memory_order_relaxed);
    m_audioHold = false;      // device gets paused below anyway
    m_wantsToPlay = false;
    m_waitingForResumeFrame = false;
    m_clock.SetPaused(true);
    if (m_hasAudio) m_audioOutput.Pause();

    // Park workers at the top of their loop. Queues + decoder state stay
    // intact — they'll resume on the next Play. Held packet inside
    // DemuxThread (if any) survives via the pipeline-flush-gen check.
    ParkPipeline();

    // Freeze exactly on the frame being shown. The playhead already tracks
    // delivered frames during playback (TryGetVideoFrame clamps the clock),
    // so this is at most a sub-frame alignment.
    if (m_hasMedia && m_lastDisplayedPts != AV_NOPTS_VALUE)
        m_clock.SetTime(static_cast<double>(m_lastDisplayedPts) *
                        av_q2d(m_demuxer.GetVideoTimeBase()));

    SetTransportSection("Paused");

    // Build the step cache window around the pause point on the seek thread.
    if (populateCache)
        PostCacheWindowRequest(m_lastDisplayedPts);
}

void Player::TogglePlayPause() {
    if (m_playing) Pause();
    else Play();
}

void Player::SeekTo(double seconds, bool resumeAfter) {
    PROFILE_SCOPE();
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
    m_dropFramesBeforeSec.store(kNeverDrop, std::memory_order_relaxed);
    m_waitingForResumeFrame = false;
    m_clock.SetPaused(true);
    if (m_hasAudio) m_audioOutput.Pause();
    // Stopped for the duration of the seek; a resume re-enters "Playing".
    SetTransportSection("Paused");

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
    PROFILE_THREAD("Player Seek");

    while (!m_stopSeekThread) {
        double target = 0.0;
        bool populateOnly = false;
        {
            std::unique_lock<std::mutex> lock(m_seekMutex);
            m_seekCv.wait(lock, [&] {
                return m_seekRequest >= 0.0 || m_cacheRequest || m_stopSeekThread;
            });
            if (m_stopSeekThread) break;
            if (m_seekRequest >= 0.0) {
                // Mark busy before consuming the request, still under the
                // lock — WaitForSeek checks m_seekRequest and m_seekBusy
                // under the same lock, so it never observes a gap.
                m_seekBusy = true;
                target = m_seekRequest;
                m_seekRequest = -1.0;
            } else {
                populateOnly = true;
            }
            m_cacheRequest = false;  // a seek's own flow reposts anyway
        }

        if (populateOnly) {
            // Cache-window maintenance. Deliberately does NOT set m_seekBusy:
            // stepping must not block on it (the cache is mutexed, and this
            // only touches the dedicated cache demuxer/decoder). A new center
            // posted mid-run just continues the loop; supersession aborts.
            auto abort = [this] {
                if (m_stopSeekThread.load(std::memory_order_relaxed)) return true;
                if (m_playing.load(std::memory_order_relaxed)) return true;
                std::lock_guard<std::mutex> lock(m_seekMutex);
                return m_seekRequest >= 0.0;
            };
            while (!abort() &&
                   MaintainCacheWindow(m_cacheWindowCenter.load(std::memory_order_relaxed),
                                       abort)) {
            }
            continue;
        }

        PROFILE_SCOPE_N("Player::SeekOp");
        Profiler::ScopedSection seekSection(Profiler::kSectionTransport, "Seeking");

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
                goto seekDone;
            }
        }

        // Check if a newer request arrived — skip this expensive decode
        {
            std::lock_guard<std::mutex> lock(m_seekMutex);
            if (m_seekRequest >= 0.0) {
                m_seekBusy = false;
                continue;
            }
        }

        SyncSeekAndDecode(target);
        m_needsResync = false;

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

        // Queue cache-window maintenance for stepping around the landing
        // point (skip during scrubbing — every drag move would repost it).
        if (!shouldResume && !scrubbing)
            PostCacheWindowRequest(m_lastDisplayedPts);

        // Pipeline stays parked. PollSeekComplete on main thread will call
        // Play() if shouldResume — Play unparks. Otherwise we stay paused.
        m_seekShouldResume = shouldResume;
        m_seekDone = true;
        m_seekBusy = false;
    }
}

void Player::PollSeekComplete() {
    SyncAudioToClock();

    // Pause at end of video. Set the clock AFTER Pause — its snap-to-
    // displayed-frame would otherwise undo the at-end position that makes
    // the next Play() restart from the beginning.
    if (m_playing && m_eof && m_clock.GetTime() >= m_demuxer.GetDuration()) {
        Pause();
        m_clock.SetTime(m_demuxer.GetDuration());
    }

    if (!m_seekDone.load(std::memory_order_acquire)) return;
    m_seekDone = false;

    if (m_seekShouldResume.load(std::memory_order_relaxed)) {
        m_seekShouldResume = false;
        m_wantsToPlay = false;
        Play();
    }
}

void Player::SyncAudioToClock() {
    if (!m_playing.load(std::memory_order_relaxed) || !m_hasAudio) return;
    if (m_waitingForResumeFrame || m_clock.IsPaused()) return;
    if (!m_audioOutput.HasQueuedData()) return; // dry stream: nothing to steer

    double clockSec = m_clock.GetTime();
    double err = m_audioOutput.GetPlaybackPosition() - clockSec;
    double baseSpeed = m_clock.GetSpeed();

    // Runaway brake: when video delivery can't sustain the requested speed,
    // the clamped clock falls behind the audio position without bound. Hold
    // the device until the clock catches up, then resume. At delivery-limited
    // speeds audio plays in bursts of up to the cap; at sustainable speeds
    // the brake never engages — the threshold sits well above the constant
    // ~300ms read-ahead offset of the queue-derived position measurement.
    if (m_audioHold) {
        if (err > 0.020) return;  // still catching up
        m_audioHold = false;
        m_audioOutput.Resume();
        return;
    }
    if (err > 0.75) {
        m_audioHold = true;
        m_audioOutput.Pause();
        return;
    }

    if (err < -0.25) {
        // Audio fell far behind the video clock (e.g. after a long decode
        // stall) — jump forward through the buffered content instead of
        // chirping the sample rate for seconds.
        m_audioOutput.DiscardUntil(clockSec);
        m_audioOutput.SetSpeed(static_cast<float>(baseSpeed));
        return;
    }

    // Rate servo: steer the device's consumption speed by up to ±0.5%
    // (inaudible) so the audio position converges on the clock. The deadband
    // avoids hunting on the ~10ms measurement granularity of device pulls.
    double corr = 0.0;
    if (std::fabs(err) > 0.010)
        corr = std::clamp(-err * 0.5, -0.005, 0.005);
    m_audioOutput.SetSpeed(static_cast<float>(baseSpeed * (1.0 + corr)));
}

void Player::FilterAudioQueueBefore(double cutoffSec) {
    PROFILE_SCOPE();
    double atb_d = av_q2d(m_demuxer.GetAudioTimeBase());
    int origCount = m_audioPacketQueue.Size();
    AVPacket* drainPkt = av_packet_alloc();
    std::vector<AVPacket*> keep;
    for (int i = 0; i < origCount; ++i) {
        if (!m_audioPacketQueue.Pop(drainPkt)) break;
        double pktSec = (drainPkt->pts != AV_NOPTS_VALUE)
            ? static_cast<double>(drainPkt->pts) * atb_d : 0.0;
        if (pktSec + 0.025 >= cutoffSec) {
            AVPacket* k = av_packet_alloc();
            av_packet_move_ref(k, drainPkt);
            keep.push_back(k);
        } else {
            av_packet_unref(drainPkt);
        }
    }
    for (AVPacket* k : keep) {
        if (!m_audioPacketQueue.Push(k)) av_packet_unref(k);
        av_packet_free(&k);
    }
    av_packet_free(&drainPkt);
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
    PROFILE_SCOPE();
    if (!m_hasMedia) return;

    WaitForSeek();
    m_pendingSeekTarget.store(-1.0, std::memory_order_relaxed);

    bool wasPlaying = m_playing.load(std::memory_order_relaxed);
    if (wasPlaying) Pause(/*populateCache=*/false);  // stepping manages the cache itself
    // Pipeline is now parked (Pause does that, or it was already parked).

    if (direction > 0) {
        AVRational tb = m_demuxer.GetVideoTimeBase();
        int64_t frameTicks = static_cast<int64_t>(GetFrameDuration() / av_q2d(tb));
        int64_t halfFrame = frameTicks / 2;

        // Forward: check if next frame is already in cache.
        if (m_lastDisplayedPts != AV_NOPTS_VALUE) {
            int64_t nextPts = m_lastDisplayedPts + frameTicks;
            ConvertedFramePtr cached = m_frameCache.FindNearest(nextPts);
            if (cached && cached->pts > m_lastDisplayedPts &&
                std::abs(cached->pts - nextPts) <= halfFrame) {
                double frameSec = static_cast<double>(cached->pts) * av_q2d(tb);
                m_clock.SetTime(frameSec);
                m_lastDisplayedPts = cached->pts;
                StageFrame(std::move(cached), /*putInCache=*/false);
                PostCacheWindowRequest(m_lastDisplayedPts);
                return;
            }
        }

        // The prefetched queue is only usable when its head is the immediate
        // next frame. During playback the convert stage may skip late frames,
        // so after a pause the queue (and the decoder behind it) can sit
        // ahead of the displayed frame — a gapped or stale head needs the
        // same decoder resync as a deferred backward-step cache hit.
        auto queueHeadGapped = [&]() -> bool {
            if (m_lastDisplayedPts == AV_NOPTS_VALUE) return false;
            int64_t headPts = m_videoFrameQueue.PeekPts();
            if (headPts == AV_NOPTS_VALUE) return false;
            return headPts <= m_lastDisplayedPts ||
                   headPts > m_lastDisplayedPts + frameTicks + halfFrame;
        };
        // Re-align the decoder with the displayed frame. SyncSeekAndDecode
        // stages the current frame; we want the NEXT one — clear it and let
        // the tick path below produce it.
        auto resyncToDisplayed = [&]() {
            FlushPipelineState();
            SyncSeekAndDecode((m_lastDisplayedPts + 0.5) * av_q2d(tb));
            m_needsResync = false;
            ClearStagedFrame();
        };

        if (m_needsResync || queueHeadGapped())
            resyncToDisplayed();

        auto stageFromQueue = [&]() -> bool {
            ConvertedFramePtr f = m_videoFrameQueue.TryPop();
            if (!f) return false;
            m_clock.SetTime(static_cast<double>(f->pts) * av_q2d(tb));
            m_lastDisplayedPts = f->pts;
            StageFrame(std::move(f), /*putInCache=*/true);
            PostCacheWindowRequest(m_lastDisplayedPts);
            return true;
        };
        if (stageFromQueue()) return;

        // Frame queue empty. Tick the pipeline once: unpark briefly so the
        // workers can produce the next frame, then re-park. If what arrives
        // is gapped (the decoder was ahead and the queue happened to be
        // drained), resync and tick once more.
        if (TickPipelineOneFrame(500) && queueHeadGapped()) {
            resyncToDisplayed();
            TickPipelineOneFrame(500);
        }
        stageFromQueue();
    } else {
        // Backward: check frame cache first (instant). Accept only the
        // immediate previous frame — FindBefore returns the nearest earlier
        // cached entry, which can be a stale frame from a much older
        // position (e.g. a previous seek landing) once the contiguous run
        // around the rest point is exhausted; that must fall through to the
        // decode path below instead of teleporting.
        bool cacheHit = false;
        if (m_lastDisplayedPts != AV_NOPTS_VALUE) {
            AVRational tb = m_demuxer.GetVideoTimeBase();
            int64_t frameTicks = static_cast<int64_t>(GetFrameDuration() / av_q2d(tb));
            ConvertedFramePtr cached = m_frameCache.FindBefore(m_lastDisplayedPts);
            if (cached &&
                std::abs(cached->pts - (m_lastDisplayedPts - frameTicks)) <= frameTicks / 2) {
                PROFILE_MARK("StepBack cache hit");
                double frameSec = static_cast<double>(cached->pts) * av_q2d(tb);
                m_clock.SetTime(frameSec);
                m_lastDisplayedPts = cached->pts;
                StageFrame(std::move(cached), /*putInCache=*/false);
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
            PostCacheWindowRequest(m_lastDisplayedPts);
            return;
        }

        // Cache miss — seek back and decode the frame immediately before
        // the current one.
        PROFILE_MARK("StepBack cache miss");
        FlushPipelineState();
        double frameDur = GetFrameDuration();
        double targetSec = m_clock.GetTime() - frameDur;
        if (targetSec < 0.0) targetSec = 0.0;
        int64_t maxPts = m_lastDisplayedPts;
        SyncSeekAndDecodeBefore(targetSec, maxPts);
        // SyncSeekAndDecodeBefore decodes several frames past maxPts (its
        // B-frame reorder margin), so the decoder no longer matches
        // m_lastDisplayedPts — even on failure (already at the first frame,
        // where nothing earlier exists but the attempt still flushed and
        // advanced the pipeline). Defer a reseek to the next forward
        // operation, same as the cache-hit path above; without this the
        // next StepFrame(+1) pops a frame ~reorder-depth ahead.
        m_needsResync = true;
        PostCacheWindowRequest(m_lastDisplayedPts);
    }
}

double Player::GetFrameDuration() const {
    double fps = m_demuxer.GetVideoFrameRate();
    return (fps > 0) ? 1.0 / fps : 1.0 / 30.0;
}

ConvertedFramePtr Player::AcquirePooledFrame() {
    std::lock_guard<std::mutex> lock(m_framePoolMutex);
    for (auto& f : m_framePool) {
        if (f.use_count() == 1) {  // only the pool holds it — free to reuse
            // Order the previous owner's final reads before our writes into
            // the recycled buffer (pairs with shared_ptr's release decrement).
            std::atomic_thread_fence(std::memory_order_acquire);
            return f;
        }
    }
    m_framePool.push_back(std::make_shared<ConvertedFrame>());
    return m_framePool.back();
}

ConvertedFramePtr Player::ConvertToPooledFrame(FrameConverter& conv, AVFrame* frame) {
    ConvertedFramePtr cf = AcquirePooledFrame();
    if (!conv.ConvertInto(frame, *cf))
        return nullptr;
    cf->pts = frame->pts;
    cf->keyframe = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
    return cf;
}

// Stage `frame` as the next frame TryGetVideoFrame hands to the renderer,
// optionally inserting it into the backward-step cache (cache hits must not
// re-Put). Returns false when `frame` is null (e.g. conversion failed).
bool Player::StageFrame(ConvertedFramePtr frame, bool putInCache) {
    if (!frame) return false;
    if (putInCache) m_frameCache.Put(frame);
    std::lock_guard<std::mutex> lock(m_stagedFrameMutex);
    m_stagedFrame = std::move(frame);
    return true;
}

ConvertedFramePtr Player::TakeStagedFrame() {
    std::lock_guard<std::mutex> lock(m_stagedFrameMutex);
    return std::move(m_stagedFrame);
}

void Player::ClearStagedFrame() {
    std::lock_guard<std::mutex> lock(m_stagedFrameMutex);
    m_stagedFrame.reset();
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
    // Re-label the transport section so the change shows up in traces.
    if (m_profTransportLabel)
        SetTransportSection(m_profTransportLabel);
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

void Player::SetAudioTrack(int streamIndex) {
    if (!m_hasMedia) return;
    if (streamIndex == m_demuxer.GetAudioStreamIndex()) return;

    AVFormatContext* fmt = m_demuxer.GetFormatContext();
    if (!fmt || streamIndex < 0 || streamIndex >= static_cast<int>(fmt->nb_streams))
        return;
    if (fmt->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return;

    bool wasPlaying = m_playing.load(std::memory_order_relaxed) ||
                      m_wantsToPlay.load(std::memory_order_relaxed);
    double curTime = m_clock.GetTime();

    // Pause() parks the pipeline (workers halt at top-of-loop, queues
    // interrupted) and pauses the audio output — the safe point to swap the
    // audio decoder/output without racing the decode threads. No cache fill:
    // the SeekTo below repopulates as part of the seek flow.
    Pause(/*populateCache=*/false);

    m_demuxer.SetAudioStreamIndex(streamIndex);
    m_audioDecoder.Close();
    if (m_audioDecoder.Open(m_demuxer.GetAudioCodecParams())) {
        int sr = m_audioDecoder.GetSampleRate();
        int ch = m_audioDecoder.GetChannels();
        if (sr != m_audioOutput.GetSampleRate() || ch != m_audioOutput.GetChannels()) {
            m_audioOutput.Close();
            m_audioOutput.Open(sr, ch);
        } else {
            m_audioOutput.Flush();
        }
        SetupResampler();
        m_hasAudio = true;
        SetMuted(m_muted);
        m_audioOutput.SetSpeed(static_cast<float>(m_clock.GetSpeed()));
    } else {
        m_hasAudio = false;
        LOG_WARN("SetAudioTrack: failed to open decoder for stream %d", streamIndex);
    }

    // Re-seek to where we were to flush stale packets/frames and refill from
    // the newly-selected audio stream (the DemuxThread filter now routes it).
    SeekTo(curTime, /*resumeAfter=*/wasPlaying);
}

// --- Synchronous decode (used when paused) ---

bool Player::SyncDecodeNextFrame() {
    PROFILE_SCOPE();
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

        StageFrame(ConvertToPooledFrame(m_frameConverter, frame), /*putInCache=*/true);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    return found;
}

bool Player::SyncSeekAndDecode(double targetSec) {
    PROFILE_SCOPE();

    // Flush decoder state — we're seeking to a new position
    m_videoDecoder.Flush();
    if (m_hasAudio) m_audioDecoder.Flush();

    // Flush queues and frame cache (stale frames from previous position)
    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_decodedFrameQueue.Flush();
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
        // Any pending resume-trim is stale — this path re-anchors itself.
        m_audioSkipUntil.store(kNoAudioSkip, std::memory_order_relaxed);
    }

    // Seek demuxer to keyframe at or before target
    m_demuxer.Seek(targetSec);
    m_pipelineFlushGen.fetch_add(1, std::memory_order_relaxed);
    m_eof = false;

    // Decode forward until we reach the target PTS
    AVRational tb = m_demuxer.GetVideoTimeBase();
    int64_t targetPts = static_cast<int64_t>(targetSec / av_q2d(tb));

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* bestFrame = nullptr;
    int maxPackets = 5000;
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
            FilterAudioQueueBefore(frameSec);
        }

        // Only convert the final target frame to RGBA (skip intermediates for speed)
        bool staged = StageFrame(ConvertToPooledFrame(m_frameConverter, bestFrame),
                                 /*putInCache=*/true);

        av_frame_free(&bestFrame);
        return staged;
    }

    m_clock.SetTime(targetSec);
    if (m_hasAudio) m_audioOutput.ResetPosition(targetSec);
    return false;
}

bool Player::SyncSeekAndDecodeBefore(double seekSec, int64_t maxPts) {
    PROFILE_SCOPE();

    // Flush state (same as SyncSeekAndDecode — we're repositioning). The
    // frame cache is left alone: the decoded predecessors are ADDED to it.
    m_videoDecoder.Flush();
    if (m_hasAudio) m_audioDecoder.Flush();
    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_decodedFrameQueue.Flush();
    m_videoFrameQueue.Flush();
    if (m_hasAudio) {
        m_audioOutput.Flush();
        m_audioOutput.ResetPosition(seekSec);
        m_audioSkipUntil.store(kNoAudioSkip, std::memory_order_relaxed);
    }

    int attempt = 0;
    double trySec = seekSec;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    // Keep decoding a few frames past the first frame with pts >= maxPts so
    // that any B-frames with pts < maxPts that emit after (decode order !=
    // display order) are still captured.
    const int kReorderDepth = 8;
    bool gotAny = false;

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
                    // Cache every predecessor we had to decode anyway —
                    // subsequent back-steps hit these instead of re-seeking.
                    if (ConvertedFramePtr cf = ConvertToPooledFrame(m_frameConverter, frame)) {
                        m_frameCache.Put(std::move(cf));
                        gotAny = true;
                    }
                } else {
                    framesAtOrAfterMax++;
                }
                av_frame_unref(frame);
            }
        }

        if (gotAny) break;

        // No frame with pts < maxPts found at this seek point — seek further
        // back and retry. Happens near the head of a GOP when the keyframe
        // itself has pts >= maxPts.
        attempt++;
        trySec -= 1.0;
        if (trySec < 0.0) { trySec = 0.0; attempt = 99; } // last try, then give up
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);

    if (!gotAny)
        return false;

    ConvertedFramePtr best = m_frameCache.FindBefore(maxPts);
    if (!best)
        return false;
    m_clock.SetTime(static_cast<double>(best->pts) * av_q2d(m_demuxer.GetVideoTimeBase()));
    m_lastDisplayedPts = best->pts;
    StageFrame(std::move(best), /*putInCache=*/false);
    return true;
}

void Player::PostCacheWindowRequest(int64_t centerPts) {
    if (!m_hasMedia || centerPts == AV_NOPTS_VALUE || !m_seekThreadRunning) return;
    m_cacheWindowCenter.store(centerPts, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_seekMutex);
        m_cacheRequest = true;
    }
    m_seekCv.notify_one();
}

int64_t Player::ProbeCacheKeyframeBefore(double sec) {
    if (!m_cacheDemuxer.Seek(std::max(0.0, sec)))
        return AV_NOPTS_VALUE;
    AVPacket* pkt = av_packet_alloc();
    int64_t kf = AV_NOPTS_VALUE;
    for (int i = 0; i < 256; ++i) {
        if (m_cacheDemuxer.ReadPacket(pkt) < 0) break;
        bool video = pkt->stream_index == m_cacheDemuxer.GetVideoStreamIndex();
        int64_t pts = pkt->pts;
        av_packet_unref(pkt);
        if (video) { kf = pts; break; }  // first video packet after a seek is the keyframe
    }
    av_packet_free(&pkt);
    return kf;
}

bool Player::MaintainCacheWindow(int64_t centerPts, const std::function<bool()>& shouldAbort) {
    PROFILE_SCOPE();
    if (!m_hasMedia || centerPts == AV_NOPTS_VALUE) return false;
    // Cache decoder failed to open — no window, stepping uses the sync paths.
    if (m_cacheDemuxer.GetVideoStreamIndex() < 0) return false;

    const double tbSec = av_q2d(m_cacheDemuxer.GetVideoTimeBase());
    const double eps = GetFrameDuration() * 0.5;

    // Decode forward from `seekSec` on the dedicated cache pipeline (the main
    // decoder stays warm for Play), caching frames in [cacheMinPts,
    // cacheMaxPtsExcl). Stops when `kfLimit` keyframes strictly past
    // `pastPts` are reached (the limit-th one is not cached), at
    // cacheMaxPtsExcl, at EOF, on abort, or at the frame-count safety cap.
    auto decodeChunk = [&](double seekSec, int64_t cacheMinPts, int64_t cacheMaxPtsExcl,
                           int64_t pastPts, int kfLimit) {
        PROFILE_SCOPE_N("CacheWindowChunk");
        m_cacheDecoder.Flush();
        if (!m_cacheDemuxer.Seek(std::max(0.0, seekSec))) return;

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        int kfsPast = 0;
        bool stop = false, eofHit = false;
        int maxPackets = 2000;

        auto handleFrame = [&](AVFrame* f) -> bool {
            if (f->pts != AV_NOPTS_VALUE) {
                if (f->pts >= cacheMaxPtsExcl) return false;
                if ((f->flags & AV_FRAME_FLAG_KEY) && f->pts > pastPts &&
                    ++kfsPast >= kfLimit)
                    return false;
                if (f->pts >= cacheMinPts) {
                    if (ConvertedFramePtr cf = ConvertToPooledFrame(m_cacheConverter, f))
                        m_frameCache.Put(std::move(cf));
                }
            }
            return m_frameCache.Count() < kMaxCacheFrames;
        };

        while (!stop && maxPackets-- > 0) {
            if (shouldAbort && shouldAbort()) break;
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
                bool keepGoing = handleFrame(frame);
                av_frame_unref(frame);
                if (!keepGoing) { stop = true; break; }
            }
        }
        if (eofHit) {
            m_cacheEofSeen = true;
            m_cacheDecoder.DrainAtEOF(frame, handleFrame);
        }
        av_packet_free(&pkt);
        av_frame_free(&frame);
    };

    // Rebuild from scratch when the window doesn't contain the playhead.
    int64_t startPts = m_frameCache.StartPts();
    if (startPts == AV_NOPTS_VALUE || centerPts < startPts ||
        centerPts > m_frameCache.EndPts()) {
        m_frameCache.Clear();
        m_cacheBofKf = AV_NOPTS_VALUE;
        m_cacheEofSeen = false;

        // Walk back kCacheGopsBehind keyframes from the playhead's GOP.
        double t = static_cast<double>(centerPts) * tbSec;
        int64_t kf = AV_NOPTS_VALUE;
        for (int i = 0; i <= kCacheGopsBehind; ++i) {
            int64_t k = ProbeCacheKeyframeBefore(t);
            if (k == AV_NOPTS_VALUE) break;
            if (kf != AV_NOPTS_VALUE && k >= kf) { m_cacheBofKf = kf; break; }
            kf = k;
            t = static_cast<double>(k) * tbSec - eps;
        }
        if (kf == AV_NOPTS_VALUE) return false;
        decodeChunk(static_cast<double>(kf) * tbSec, INT64_MIN, INT64_MAX,
                    centerPts, kCacheGopsAhead + 1);
        return true;  // re-evaluate the policy on the next iteration
    }

    // Trim to the target window around the playhead's GOP.
    std::vector<int64_t> kfs = m_frameCache.KeyframePts();
    int g = -1;  // index of the playhead's GOP
    for (size_t i = 0; i < kfs.size(); ++i)
        if (kfs[i] <= centerPts) g = static_cast<int>(i);
    if (g < 0) {
        // Window no longer starts at a keyframe covering the center — rebuild.
        m_frameCache.Clear();
        return true;
    }
    m_frameCache.DiscardBefore(kfs[std::max(0, g - kCacheGopsBehind)]);
    if (g + kCacheGopsAhead + 1 < static_cast<int>(kfs.size()))
        m_frameCache.DiscardAfter(kfs[g + kCacheGopsAhead + 1] - 1);

    if (m_frameCache.Count() >= kMaxCacheFrames) return false;

    // Grow one GOP toward whichever edge violates the policy.
    startPts = m_frameCache.StartPts();
    if (g < kCacheGopsBehind && m_cacheBofKf != startPts) {
        int64_t prevKf = ProbeCacheKeyframeBefore(static_cast<double>(startPts) * tbSec - eps);
        if (prevKf == AV_NOPTS_VALUE || prevKf >= startPts) {
            m_cacheBofKf = startPts;  // the file starts here — nothing earlier
        } else {
            decodeChunk(static_cast<double>(prevKf) * tbSec, INT64_MIN, startPts,
                        centerPts, 1 << 30);
            return true;
        }
    }
    if (static_cast<int>(kfs.size()) - 1 - g < kCacheGopsAhead && !m_cacheEofSeen) {
        int64_t endPts = m_frameCache.EndPts();
        decodeChunk(static_cast<double>(endPts) * tbSec, endPts + 1, INT64_MAX,
                    endPts, 2);
        return true;
    }
    return false;  // window satisfies the policy
}

// --- TryGetVideoFrame (used during playback) ---

bool Player::TryGetVideoFrame(const uint8_t** outRGBA, int* outWidth, int* outHeight) {
    if (!m_hasMedia) return false;
    PROFILE_SCOPE();

    // Return the frame staged by a synchronous decode (seek/step)
    if (ConvertedFramePtr staged = TakeStagedFrame()) {
        m_displayedFrame = std::move(staged);
        *outRGBA = m_displayedFrame->data();
        *outWidth = m_displayedFrame->width;
        *outHeight = m_displayedFrame->height;
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
        while (true) {
            int64_t pts = m_videoFrameQueue.PeekPts();
            if (pts == AV_NOPTS_VALUE) {
                return false; // no frames yet, keep waiting
            }
            double frameSec = static_cast<double>(pts) * av_q2d(tb);
            if (frameSec >= m_resumeTime - frameDur) {
                // This frame is at or near our resume point — start playback
                m_waitingForResumeFrame = false;
                // Start the clock where the audio content resumes rather
                // than at this frame's pts — the first queued frame can be
                // up to one frame ahead of the pause point, and anchoring
                // there would fast-forward the clock relative to the audio
                // on every resume, leaving the rate servo a ~frame of lag
                // to grind away each time.
                double startSec = frameSec;
                if (m_hasAudio) {
                    double audioPos = m_audioOutput.GetPlaybackPosition();
                    if (std::fabs(audioPos - frameSec) < 0.150) startSec = audioPos;
                }
                m_clock.SetTime(startSec);
                m_clock.SetPaused(false);
                if (m_hasAudio) m_audioOutput.Resume();
                break;
            }
            // Drop this frame — it's before our resume point
            if (!m_videoFrameQueue.TryPop()) break;
        }
        if (m_waitingForResumeFrame) return false;
    }

    double clockSec = m_clock.GetTime();

    // Let the convert stage skip frames the catch-up loop below would drop
    // anyway.
    m_dropFramesBeforeSec.store(clockSec - frameDur, std::memory_order_relaxed);

    // Frames arrive already converted (ConvertThread does the RGBA
    // conversion) — pick the newest one that's due against the clock.
    ConvertedFramePtr best;

    while (true) {
        int64_t pts = m_videoFrameQueue.PeekPts();
        if (pts == AV_NOPTS_VALUE) break;

        double frameSec = static_cast<double>(pts) * av_q2d(tb);

        if (frameSec > clockSec + 0.005) {
            break; // too early
        }

        ConvertedFramePtr f = m_videoFrameQueue.TryPop();
        if (!f) break;
        best = std::move(f);

        double late = clockSec - frameSec;
        if (late <= frameDur) break;
    }

    if (!best) return false;

    m_lastDisplayedPts = best->pts;

    // The playhead tracks what is actually displayed: when delivery can't
    // keep up with the requested speed (heavy files at high multipliers),
    // pull the clock back to the displayed frame instead of letting it run
    // ahead of the picture.
    {
        double frameSec = static_cast<double>(best->pts) * av_q2d(tb);
        if (clockSec > frameSec + frameDur)
            m_clock.SetTime(frameSec);
    }

    // No cache Put here: the step-back cache is only populated at rest
    // (pause / post-seek / stepping), never during playback — playback can
    // drop or skip frames, which would leave holes in it.
    m_displayedFrame = std::move(best);

    *outRGBA = m_displayedFrame->data();
    *outWidth = m_displayedFrame->width;
    *outHeight = m_displayedFrame->height;
    return true;
}

// --- Thread management ---

void Player::WaitForSeek() {
    PROFILE_WAIT_SCOPE();
    // Spin-wait until no seek is pending or in flight. Checking m_seekBusy
    // alone is not enough: SeekTo posts m_seekRequest and returns before the
    // seek thread wakes and marks itself busy. Cache-window maintenance is
    // deliberately NOT awaited — the cache is mutexed, and stepping must not
    // block on background fills.
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_seekMutex);
            if (m_seekRequest < 0.0 && !m_seekBusy.load(std::memory_order_acquire))
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Player::SpawnPipelineThreads() {
    PROFILE_SCOPE();
    {
        std::lock_guard<std::mutex> lk(m_pipelineMutex);
        m_pipelineActive = false;
        m_pipelineParkedCount = 0;
        m_pipelineExit = false;
        m_pipelineThreadCount = m_hasAudio ? 4 : 3;
    }
    m_videoPacketQueue.Reset();
    m_audioPacketQueue.Reset();
    m_decodedFrameQueue.Reset();
    m_videoFrameQueue.Reset();

    m_demuxThread = std::thread(&Player::DemuxThread, this);
    m_videoDecodeThread = std::thread(&Player::VideoDecodeThread, this);
    m_convertThread = std::thread(&Player::ConvertThread, this);
    if (m_hasAudio) {
        m_audioDecodeThread = std::thread(&Player::AudioDecodeThread, this);
    }
}

void Player::StopPipelineThreads() {
    PROFILE_SCOPE();
    {
        std::lock_guard<std::mutex> lk(m_pipelineMutex);
        m_pipelineExit = true;
        m_pipelineActive = false;
    }
    m_pipelineCv.notify_all();
    m_videoPacketQueue.Abort();
    m_audioPacketQueue.Abort();
    m_decodedFrameQueue.Abort();
    m_videoFrameQueue.Abort();

    if (m_demuxThread.joinable()) m_demuxThread.join();
    if (m_videoDecodeThread.joinable()) m_videoDecodeThread.join();
    if (m_convertThread.joinable()) m_convertThread.join();
    if (m_audioDecodeThread.joinable()) m_audioDecodeThread.join();
}

void Player::ParkPipeline() {
    PROFILE_SCOPE();
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
    m_decodedFrameQueue.Interrupt();
    m_videoFrameQueue.Interrupt();

    // Wait for all workers to reach their park gate.
    PROFILE_WAIT_SCOPE_N("WaitWorkersParked");
    std::unique_lock<std::mutex> lk(m_pipelineMutex);
    m_pipelineParkedCv.wait(lk, [&] {
        return m_pipelineParkedCount == m_pipelineThreadCount || m_pipelineExit;
    });
}

void Player::UnparkPipeline() {
    PROFILE_SCOPE();
    m_videoPacketQueue.ClearInterrupt();
    m_audioPacketQueue.ClearInterrupt();
    m_decodedFrameQueue.ClearInterrupt();
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
    PROFILE_THREAD("Player Demux");
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
    PROFILE_THREAD("Player VideoDecode");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* held = nullptr;  // frame awaiting push (preserved across park)
    uint64_t heldGen = 0;
    bool drainedEof = false;

    while (true) {
        if (!WorkerParkOrExit()) break;

        // A flush while we were parked makes the held frame stale.
        if (held && m_pipelineFlushGen.load(std::memory_order_relaxed) != heldGen)
            av_frame_free(&held);

        // Deliver any held frame first — dropping it would punch a gap into
        // the frame stream, which StepFrame relies on being contiguous.
        if (held) {
            if (!m_decodedFrameQueue.Push(held)) continue;  // park requested
            av_frame_free(&held);
        }

        // Re-arm EOF drain whenever m_eof goes back to false (post-seek).
        if (!m_eof.load(std::memory_order_relaxed)) drainedEof = false;

        // EOF idle: drain decoder once, then poll until state changes.
        if (m_eof.load(std::memory_order_relaxed) && m_videoPacketQueue.Empty()) {
            if (!drainedEof) {
                drainedEof = true;
                m_videoDecoder.DrainAtEOF(frame, [&](AVFrame* f) {
                    return m_decodedFrameQueue.Push(f);
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

            if (!m_decodedFrameQueue.Push(frame)) {
                // Park requested while waiting on Push. Save for redelivery.
                held = av_frame_alloc();
                av_frame_move_ref(held, frame);
                heldGen = m_pipelineFlushGen.load(std::memory_order_relaxed);
                break;  // top-of-loop handles the park
            }
        }
    }

    if (held) av_frame_free(&held);
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

// Convert stage: decoded AVFrames in, display-ready ConvertedFrames out.
// Running this on its own thread keeps decode and sws_scale in parallel —
// together they can't sustain high-speed playback rates serially — and keeps
// both off the main thread.
void Player::ConvertThread() {
    SetCurrentThreadName(L"ScrubCut Convert");
    PROFILE_THREAD("Player Convert");
    AVFrame* frame = av_frame_alloc();
    ConvertedFramePtr held;  // frame awaiting push (preserved across park)
    uint64_t heldGen = 0;

    while (true) {
        if (!WorkerParkOrExit()) break;

        // A flush while we were parked makes the held frame stale.
        if (held && m_pipelineFlushGen.load(std::memory_order_relaxed) != heldGen)
            held.reset();

        // Deliver any held frame first — dropping it would punch a gap into
        // the frame stream, which StepFrame relies on being contiguous.
        if (held) {
            if (!m_videoFrameQueue.Push(held)) continue;  // park requested
            held.reset();
        }

        if (!m_decodedFrameQueue.PopWithTimeout(frame, 50)) continue;

        // Skip a frame only when it is both late against the clock AND a
        // newer decoded frame already waits behind it — the main thread
        // displays the newest due frame, so converting it would be wasted
        // work. With an empty queue this is the newest frame available:
        // convert it regardless of lateness, so the display always advances
        // (a decoder-bound stream shows every frame it manages to produce,
        // and starvation is structurally impossible).
        if (frame->pts != AV_NOPTS_VALUE && m_decodedFrameQueue.Size() > 0) {
            double frameSec = static_cast<double>(frame->pts) * av_q2d(m_demuxer.GetVideoTimeBase());
            if (frameSec < m_dropFramesBeforeSec.load(std::memory_order_relaxed)) {
                av_frame_unref(frame);
                continue;
            }
        }

        ConvertedFramePtr cf = ConvertToPooledFrame(m_frameConverter, frame);
        av_frame_unref(frame);
        if (!cf) continue;
        if (!m_videoFrameQueue.Push(cf)) {
            // Park requested while waiting on Push. Save for redelivery.
            held = std::move(cf);
            heldGen = m_pipelineFlushGen.load(std::memory_order_relaxed);
        }
    }

    av_frame_free(&frame);
}

void Player::AudioDecodeThread() {
    SetCurrentThreadName(L"ScrubCut AudioDecode");
    PROFILE_THREAD("Player AudioDecode");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint8_t* outBuf = nullptr;
    int outBufSize = 0;

    while (true) {
        if (!WorkerParkOrExit()) break;

        if (!m_audioPacketQueue.PopWithTimeout(pkt, 50)) continue;

        int ret = m_audioDecoder.SendPacket(pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (true) {
            ret = m_audioDecoder.ReceiveFrame(frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            // Media timestamp of the frame's first sample, for pts-anchored
            // position tracking in AudioOutput (NAN = unknown, continue).
            double ptsSec = NAN;
            if (frame->pts != AV_NOPTS_VALUE)
                ptsSec = static_cast<double>(frame->pts) * av_q2d(m_demuxer.GetAudioTimeBase());

            if (m_swrCtx) {
                PROFILE_SCOPE_N("AudioResampleWrite");
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
                    int bytesPerSample = m_audioOutput.GetChannels() * 2;
                    int size = converted * bytesPerSample;
                    const uint8_t* writePtr = outBuf;
                    double writePts = ptsSec;
                    bool drop = false;

                    // Resume-point trim: when Play() rebuilt the stream at a
                    // position packet granularity can't hit, discard/trim
                    // samples so the first written one lands exactly there.
                    double skip = m_audioSkipUntil.load(std::memory_order_relaxed);
                    if (skip != kNoAudioSkip && ptsSec == ptsSec) {
                        int rate = m_audioOutput.GetSampleRate();
                        if (ptsSec + static_cast<double>(converted) / rate <= skip) {
                            drop = true; // wholly before the resume point
                        } else {
                            if (ptsSec < skip) {
                                int trim = static_cast<int>(std::lround((skip - ptsSec) * rate));
                                trim = std::clamp(trim, 0, converted - 1);
                                writePtr += trim * bytesPerSample;
                                size -= trim * bytesPerSample;
                                writePts = ptsSec + static_cast<double>(trim) / rate;
                            }
                            m_audioSkipUntil.store(kNoAudioSkip, std::memory_order_relaxed);
                        }
                    }

                    if (!drop)
                        m_audioOutput.Write(writePtr, size, writePts);
                }
            }

            av_frame_unref(frame);
        }
    }

    av_free(outBuf);
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

void Player::EmitProfilerPlots() {
    if (!Profiler::IsEnabled() || !m_hasMedia) return;
    PROFILE_PLOT("Video packet queue", static_cast<int64_t>(m_videoPacketQueue.Size()));
    PROFILE_PLOT("Audio packet queue", static_cast<int64_t>(m_audioPacketQueue.Size()));
    PROFILE_PLOT("Decoded frame queue", static_cast<int64_t>(m_decodedFrameQueue.Size()));
    PROFILE_PLOT("Video frame queue", static_cast<int64_t>(m_videoFrameQueue.Size()));
    PROFILE_PLOT("Frame cache", static_cast<int64_t>(m_frameCache.Count()));
}

void Player::FlushPipelineState() {
    m_videoDecoder.Flush();
    if (m_hasAudio) m_audioDecoder.Flush();
    m_videoPacketQueue.Flush();
    m_audioPacketQueue.Flush();
    m_decodedFrameQueue.Flush();
    m_videoFrameQueue.Flush();
    m_pipelineFlushGen.fetch_add(1, std::memory_order_relaxed);
}

bool Player::TickPipelineOneFrame(int timeoutMs) {
    PROFILE_SCOPE();
    UnparkPipeline();
    bool got;
    {
        PROFILE_WAIT_SCOPE_N("WaitFrameReady");
        got = m_videoFrameQueue.WaitForOne(timeoutMs);
    }
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
