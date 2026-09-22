#include "export/Exporter.h"
#include "core/Demuxer.h"
#include "core/VideoDecoder.h"
#include "core/AudioDecoder.h"
#include "core/FrameConverter.h"
#include "util/Log.h"
#include "util/FFmpegUtils.h"
#include "util/Profiler.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
static void SetCurrentThreadName(const wchar_t* name) {
    SetThreadDescription(GetCurrentThread(), name);
}
#else
static void SetCurrentThreadName(const wchar_t*) {}
#endif

Exporter::~Exporter() {
    Cancel();
}

void Exporter::Start(const std::string& inputPath, const ExportSettings& settings) {
    Cancel(); // stop any previous export

    m_progress.Reset();
    m_cancel = false;
    m_inputPath = inputPath;
    m_settings = settings;
    {
        std::lock_guard<std::mutex> lock(m_outputPathsMutex);
        m_outputPaths.clear();
    }
    m_progress.totalItems = static_cast<int>(settings.segments.size() + settings.frames.size());
    m_progress.running = true;

    m_thread = std::thread(&Exporter::ExportThread, this);
}

void Exporter::Cancel() {
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

bool Exporter::EnsureTonemap() {
    if (m_tonemap.IsReady())
        return true;
    if (!m_gpuDevice)
        return false;  // no GPU device available
    return m_tonemap.Init(m_gpuDevice);
}

void Exporter::ExportThread() {
    SetCurrentThreadName(L"ScrubCut Export");
    PROFILE_THREAD("Export");
    PROFILE_SCOPE();

    int totalItems = static_cast<int>(m_settings.segments.size() + m_settings.frames.size());
    const uint32_t profSection = PROFILE_SECTION_ENTER(Profiler::kSectionJobs,
                                                       "Export: %d item(s)", totalItems);

    // Release the tone-mapper's GPU resources (deferred-safe from this thread).
    // Must run on every exit path; called before every `return` below.
    auto finish = [&]() {
        m_tonemap.Shutdown();
        m_progress.running = false;
        PROFILE_SECTION_LEAVE(profSection);
    };

    int itemsDone = 0;
    auto recordOutput = [&](const std::string& outPath) {
        std::lock_guard<std::mutex> lock(m_outputPathsMutex);
        m_outputPaths.push_back(outPath);
    };
    std::string srcExt = std::filesystem::path(m_inputPath).extension().string();

    // Matroska (.mkv/.webm) doesn't support edit lists, so stream-copy
    // exports show the keyframe pre-roll at the start (mark-in is not
    // frame-accurate). Re-mux the same packets into an MP4 container, which
    // supports edit lists. The bitstream is bit-identical — no quality loss.
    // Only safe for codecs MP4 also accepts; for arbitrary MKV-only codecs
    // (Vorbis audio, etc.) the muxer will fail and we'll fall back below.
    std::string srcExtLower = srcExt;
    std::transform(srcExtLower.begin(), srcExtLower.end(), srcExtLower.begin(), ::tolower);
    bool remuxToMp4 = (srcExtLower == ".mkv" || srcExtLower == ".webm");
    // GIF input has no muxer-level trim (no edit-list equivalent), and frames
    // are inter-frame deltas, so stream-copy trim is unsafe. SourceFormat on a
    // GIF source therefore re-encodes via the GIF path with source W/FPS.
    bool srcIsGif = (srcExtLower == ".gif");
    std::string sourceFormatExt = remuxToMp4 ? ".mp4" : srcExt;

    for (int i = 0; i < static_cast<int>(m_settings.segments.size()); i++) {
        if (m_cancel) break;

        m_progress.currentItem = itemsDone + 1;
        const auto& seg = m_settings.segments[i];

        bool ok = false;
        std::string outPath;
        if (srcIsGif) {
            // GIF source: output is always GIF at source W/FPS, regardless
            // of seg.mode (the UI locks the toggle, but old segments may
            // still carry mode==GIF; treat them as SourceFormat anyway).
            outPath = BuildOutputPath(m_settings.outputPath, seg.name, i, ".gif");
            LOG_INFO("Exporting segment %d/%d (GIF, source params) -> %s",
                     itemsDone + 1, totalItems, outPath.c_str());
            // 0/0.0 = "match source W/FPS" — see ExportSegmentGIF.
            ok = ExportSegmentGIF(m_inputPath, seg, outPath, 0, 0.0);
        } else if (seg.mode == ExportMode::SourceFormat) {
            outPath = BuildOutputPath(m_settings.outputPath, seg.name, i, sourceFormatExt);
            LOG_INFO("Exporting segment %d/%d (stream copy) -> %s",
                     itemsDone + 1, totalItems, outPath.c_str());
            ok = ExportSegmentStreamCopy(m_inputPath, seg, outPath);
            // Fallback: if MP4 mux failed (incompatible codec), retry with
            // original .mkv/.webm extension.
            if (!ok && !m_cancel && remuxToMp4) {
                LOG_WARN("MP4 remux failed; falling back to %s", srcExt.c_str());
                m_progress.Reset();
                m_progress.totalItems = totalItems;
                m_progress.currentItem = itemsDone + 1;
                m_progress.running = true;
                outPath = BuildOutputPath(m_settings.outputPath, seg.name, i, srcExt);
                ok = ExportSegmentStreamCopy(m_inputPath, seg, outPath);
            }
        } else {
            outPath = BuildOutputPath(m_settings.outputPath, seg.name, i, ".gif");
            LOG_INFO("Exporting segment %d/%d (GIF) -> %s",
                     itemsDone + 1, totalItems, outPath.c_str());
            ok = ExportSegmentGIF(m_inputPath, seg, outPath,
                                  m_settings.gifWidth, m_settings.gifFps);
        }

        if (!ok && !m_cancel) {
            finish();
            return;
        }
        if (ok) recordOutput(outPath);
        itemsDone++;
        m_progress.fraction = static_cast<float>(itemsDone) / static_cast<float>(totalItems);
    }

    for (int i = 0; i < static_cast<int>(m_settings.frames.size()); i++) {
        if (m_cancel) break;

        m_progress.currentItem = itemsDone + 1;
        const auto& f = m_settings.frames[i];
        std::string outPath = BuildOutputPath(m_settings.outputPath, f.name, i, ".png");
        LOG_INFO("Exporting frame %d/%d (PNG) -> %s",
                 itemsDone + 1, totalItems, outPath.c_str());
        bool ok = ExportFramePNG(m_inputPath, f, outPath);
        if (!ok && !m_cancel) {
            finish();
            return;
        }
        if (ok) recordOutput(outPath);
        itemsDone++;
        m_progress.fraction = static_cast<float>(itemsDone) / static_cast<float>(totalItems);
    }

    if (!m_cancel) {
        m_progress.fraction = 1.0f;
        m_progress.finished = true;
        LOG_INFO("Export complete");
    }
    finish();
}

std::string Exporter::BuildOutputPath(const std::string& basePath, const std::string& markName,
                                       int fallbackIndex, const std::string& extension) const {
    // basePath is "output dir + extension-less base name" — use filename(),
    // not stem(), so base names containing dots aren't truncated.
    std::filesystem::path base(basePath);
    std::string stem = base.filename().string();
    std::string dir = base.parent_path().string();

    std::string suffix;
    if (!markName.empty()) {
        suffix = m_settings.delimiter + markName;
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%03d", fallbackIndex + 1);
        suffix = m_settings.delimiter + buf;
    }

    std::filesystem::path result = std::filesystem::path(dir) / (stem + suffix + extension);
    return result.string();
}

// ---------------------------------------------------------------------------
// Frame boundaries of a mark range
// ---------------------------------------------------------------------------
// The player shows the first frame at or after a time
// (Player::SyncSeekAndDecode), so a segment runs from that frame at mark-in
// through that frame at mark-out, inclusive.
struct SegmentFrames {
    int64_t inPts = AV_NOPTS_VALUE;   // first frame at/after mark-in
    int64_t outPts = AV_NOPTS_VALUE;  // first frame at/after mark-out (last frame when none)
    int64_t outDts = AV_NOPTS_VALUE;  // frames decoded before it may be its references
    int64_t outDur = 0;               // display duration of the out frame, in ticks
};

// Scan packets (no decoding) from the keyframe before `sec` for the smallest
// video pts at or after `targetPts`. Packets come in decode order, so keep
// reading until a dts reaches the candidate: nothing after that can beat it
// (pts >= dts). Falls back to the largest pts seen when the target is past
// the last frame. Returns false if no timestamped video packet exists.
static bool FindFrameAtOrAfter(Demuxer& demuxer, double sec, int64_t targetPts,
                               int64_t& outPts, int64_t& outDts, int64_t& outDur) {
    if (!demuxer.Seek(sec)) return false;
    const int videoIdx = demuxer.GetVideoStreamIndex();
    // Streams without dts: give up a second past the candidate.
    const int64_t marginTs = ff::SecondsToPts(1.0, demuxer.GetVideoTimeBase());

    int64_t best = AV_NOPTS_VALUE, bestDts = AV_NOPTS_VALUE, bestDur = 0;
    int64_t last = AV_NOPTS_VALUE, lastDts = AV_NOPTS_VALUE, lastDur = 0;
    AVPacket* pkt = av_packet_alloc();
    while (demuxer.ReadPacket(pkt) >= 0) {
        if (pkt->stream_index != videoIdx || pkt->pts == AV_NOPTS_VALUE) {
            av_packet_unref(pkt);
            continue;
        }
        if (pkt->pts >= targetPts && (best == AV_NOPTS_VALUE || pkt->pts < best)) {
            best = pkt->pts;
            bestDts = pkt->dts;
            bestDur = pkt->duration;
        }
        if (last == AV_NOPTS_VALUE || pkt->pts > last) {
            last = pkt->pts;
            lastDts = pkt->dts;
            lastDur = pkt->duration;
        }
        bool past = false;
        if (best != AV_NOPTS_VALUE) {
            past = (pkt->dts != AV_NOPTS_VALUE) ? (pkt->dts >= best)
                                                : (pkt->pts > best + marginTs);
        }
        av_packet_unref(pkt);
        if (past) break;
    }
    av_packet_free(&pkt);

    if (best == AV_NOPTS_VALUE) {
        best = last;
        bestDts = lastDts;
        bestDur = lastDur;
    }
    if (best == AV_NOPTS_VALUE) return false;
    outPts = best;
    outDts = bestDts;
    outDur = bestDur;
    return true;
}

// Resolve both ends of `range` to frames. Leaves the demuxer positioned
// arbitrarily; seek again before reading. Returns false when the stream has
// no usable timestamps.
static bool ResolveSegmentFrames(Demuxer& demuxer, const TimeRange& range, SegmentFrames& f) {
    const AVRational tb = demuxer.GetVideoTimeBase();
    int64_t inDts = AV_NOPTS_VALUE, inDur = 0;
    if (!FindFrameAtOrAfter(demuxer, range.startSec, ff::SecondsToPts(range.startSec, tb),
                            f.inPts, inDts, inDur))
        return false;
    if (!FindFrameAtOrAfter(demuxer, range.endSec, ff::SecondsToPts(range.endSec, tb),
                            f.outPts, f.outDts, f.outDur))
        return false;
    if (f.outPts < f.inPts) {
        // Both marks past the last frame: a single-frame segment.
        f.outPts = f.inPts;
        f.outDts = inDts;
        f.outDur = inDur;
    }
    if (f.outDur <= 0) {
        // Containers without per-packet durations: assume the nominal rate.
        double fps = demuxer.GetVideoFrameRate();
        f.outDur = (fps > 0.0) ? ff::SecondsToPts(1.0 / fps, tb) : 0;
    }
    LOG_INFO("Segment export: marks %.3f-%.3fs -> frames %.3f..%.3fs",
             range.startSec, range.endSec,
             static_cast<double>(f.inPts) * av_q2d(tb),
             static_cast<double>(f.outPts) * av_q2d(tb));
    return true;
}

// ---------------------------------------------------------------------------
// MP4 edit-list trim
// ---------------------------------------------------------------------------
static uint32_t Rd32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint64_t Rd64(const uint8_t* p) { return (uint64_t(Rd32(p)) << 32) | Rd32(p + 4); }
static void Wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
static void Wr64(uint8_t* p, uint64_t v) { Wr32(p, uint32_t(v >> 32)); Wr32(p + 4, uint32_t(v)); }

// The copied segment carries reference frames after the out frame (see the
// copy loop) and the muxer's edit list runs to the end of the last sample,
// which would present them. Cap every track's edit list and the track/movie
// durations at `presentationSec`. Values are rewritten in place. Returns
// false, without modifying anything, if the layout isn't as expected.
static bool TrimMp4Presentation(const std::string& path, double presentationSec) {
    std::fstream f(std::filesystem::u8path(path), std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const int64_t fileSize = static_cast<int64_t>(f.tellg());

    // Top-level moov atom.
    int64_t moovPos = -1;
    uint64_t moovSize = 0;
    size_t moovHdr = 8;
    for (int64_t pos = 0; pos + 8 <= fileSize;) {
        uint8_t hdr[16];
        f.seekg(pos);
        f.read(reinterpret_cast<char*>(hdr), 8);
        if (!f) return false;
        uint64_t size = Rd32(hdr);
        size_t hdrLen = 8;
        if (size == 1) {
            f.read(reinterpret_cast<char*>(hdr + 8), 8);
            if (!f) return false;
            size = Rd64(hdr + 8);
            hdrLen = 16;
        } else if (size == 0) {
            size = static_cast<uint64_t>(fileSize - pos);
        }
        if (size < hdrLen) return false;
        if (memcmp(hdr + 4, "moov", 4) == 0) {
            moovPos = pos;
            moovSize = size;
            moovHdr = hdrLen;
            break;
        }
        pos += static_cast<int64_t>(size);
    }
    if (moovPos < 0 || moovSize > (uint64_t(64) << 20)) return false;

    std::vector<uint8_t> moov(static_cast<size_t>(moovSize));
    f.seekg(moovPos);
    f.read(reinterpret_cast<char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
    if (!f) return false;

    struct Atom { size_t pos; size_t hdr; size_t size; };
    auto children = [&](size_t begin, size_t end) {
        std::vector<Atom> out;
        for (size_t pos = begin; pos + 8 <= end;) {
            uint64_t size = Rd32(&moov[pos]);
            size_t hdr = 8;
            if (size == 1 && pos + 16 <= end) {
                size = Rd64(&moov[pos + 8]);
                hdr = 16;
            } else if (size == 0) {
                size = end - pos;
            }
            if (size < hdr || pos + size > end) break;
            out.push_back({pos, hdr, static_cast<size_t>(size)});
            pos += static_cast<size_t>(size);
        }
        return out;
    };
    auto is = [&](const Atom& a, const char* type) { return memcmp(&moov[a.pos + 4], type, 4) == 0; };
    // Byte 0 of each payload is the version; v1 widens the timestamp fields.

    const auto top = children(moovHdr, moov.size());
    uint32_t movieTimescale = 0;
    const Atom* mvhd = nullptr;
    for (const Atom& a : top) {
        if (!is(a, "mvhd")) continue;
        const uint8_t* p = &moov[a.pos + a.hdr];
        movieTimescale = Rd32(p + (p[0] == 0 ? 12 : 20));
        mvhd = &a;
        break;
    }
    if (!mvhd || movieTimescale == 0) return false;
    const uint64_t cap = static_cast<uint64_t>(std::llround(presentationSec * movieTimescale));

    auto capField = [&](uint8_t* p, bool wide) {
        uint64_t v = wide ? Rd64(p) : Rd32(p);
        if (v <= cap) return;
        if (wide) Wr64(p, cap);
        else Wr32(p, static_cast<uint32_t>(cap));
    };
    {
        uint8_t* p = &moov[mvhd->pos + mvhd->hdr];
        capField(p + (p[0] == 0 ? 16 : 24), p[0] != 0);
    }
    for (const Atom& trak : top) {
        if (!is(trak, "trak")) continue;
        for (const Atom& a : children(trak.pos + trak.hdr, trak.pos + trak.size)) {
            if (is(a, "tkhd")) {
                uint8_t* p = &moov[a.pos + a.hdr];
                capField(p + (p[0] == 0 ? 20 : 28), p[0] != 0);
            } else if (is(a, "edts")) {
                for (const Atom& e : children(a.pos + a.hdr, a.pos + a.size)) {
                    if (!is(e, "elst")) continue;
                    uint8_t* p = &moov[e.pos + e.hdr];
                    const bool wide = p[0] != 0;
                    const uint32_t count = Rd32(p + 4);
                    const size_t entrySize = wide ? 20 : 12;
                    if (e.hdr + 8 + static_cast<size_t>(count) * entrySize > e.size) return false;
                    // Segment durations are in movie timescale; cap the
                    // running total so presentation stops at `cap`.
                    uint64_t acc = 0;
                    for (uint32_t i = 0; i < count; i++) {
                        uint8_t* d = p + 8 + i * entrySize;
                        uint64_t dur = wide ? Rd64(d) : Rd32(d);
                        uint64_t allowed = (acc >= cap) ? 0 : cap - acc;
                        if (dur > allowed) {
                            dur = allowed;
                            if (wide) Wr64(d, dur);
                            else Wr32(d, static_cast<uint32_t>(dur));
                        }
                        acc += dur;
                    }
                }
            }
        }
    }

    f.seekp(moovPos);
    f.write(reinterpret_cast<const char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------
// Embedded frame-rate rewrite for speed-changed copies
// ---------------------------------------------------------------------------
// A speed change scales the container timestamps, but H.264/HEVC parameter
// sets still carry the encoder's frame rate, and FFmpeg-based readers prefer
// that when it disagrees with the container. Rewrite it to the output rate
// with the codec's metadata bitstream filter; slice data is untouched.
// Returns nullptr when the codec has no such filter.
static AVBSFContext* OpenTimingRewriteFilter(AVFormatContext* inFmt, int videoIdx, double speed) {
    AVStream* st = inFmt->streams[videoIdx];
    const char* filterName = nullptr;
    AVRational ticksPerFrame = {1, 1};
    switch (st->codecpar->codec_id) {
        case AV_CODEC_ID_H264:
            filterName = "h264_metadata";
            ticksPerFrame = {2, 1};  // H.264 VUI ticks count fields
            break;
        case AV_CODEC_ID_HEVC:
            filterName = "hevc_metadata";
            break;
        default:
            break;
    }
    if (!filterName) {
        LOG_INFO("Segment export: no frame-rate rewrite for %s",
                 avcodec_get_name(st->codecpar->codec_id));
        return nullptr;
    }
    AVRational srcFps = av_guess_frame_rate(inFmt, st, nullptr);
    if (srcFps.num <= 0 || srcFps.den <= 0) return nullptr;
    AVRational outFps = av_mul_q(srcFps, av_d2q(speed, 100000));
    AVRational tickRate = av_mul_q(outFps, ticksPerFrame);

    const AVBitStreamFilter* filter = av_bsf_get_by_name(filterName);
    if (!filter) {
        LOG_WARN("Segment export: %s unavailable; embedded frame rate left unchanged", filterName);
        return nullptr;
    }
    AVBSFContext* bsf = nullptr;
    if (av_bsf_alloc(filter, &bsf) < 0) return nullptr;
    avcodec_parameters_copy(bsf->par_in, st->codecpar);
    bsf->time_base_in = st->time_base;
    if (av_opt_set_q(bsf->priv_data, "tick_rate", tickRate, 0) < 0 || av_bsf_init(bsf) < 0) {
        LOG_WARN("Segment export: %s failed; embedded frame rate left unchanged", filterName);
        av_bsf_free(&bsf);
        return nullptr;
    }
    LOG_INFO("Segment export: embedded frame rate %.3f -> %.3f fps for %.4gx speed (%s)",
             av_q2d(srcFps), av_q2d(outFps), speed, filterName);
    return bsf;
}

// ---------------------------------------------------------------------------
// Stream copy export
// ---------------------------------------------------------------------------
bool Exporter::ExportSegmentStreamCopy(const std::string& inputPath,
                                        const TimeRange& range,
                                        const std::string& outputPath) {
    PROFILE_SCOPE();
    Demuxer demuxer;
    if (!demuxer.Open(inputPath, "export")) {
        m_progress.SetError("Failed to open input: " + inputPath);
        return false;
    }

    AVFormatContext* inFmt = demuxer.GetFormatContext();

    // Create output format context
    AVFormatContext* outFmt = nullptr;
    int ret = avformat_alloc_output_context2(&outFmt, nullptr, nullptr, outputPath.c_str());
    if (ret < 0 || !outFmt) {
        m_progress.SetError("Failed to create output context: " + ff::ErrorString(ret));
        return false;
    }

    // Map streams: copy video, and audio if effectively kept (GIF mode and
    // non-1× speed force audio off because stream-copy can't produce clean
    // audio output in those cases).
    int videoInIdx = demuxer.GetVideoStreamIndex();
    int audioInIdx = EffectiveKeepAudio(range) ? demuxer.GetAudioStreamIndex() : -1;
    int videoOutIdx = -1;
    int audioOutIdx = -1;
    int outStreamCount = 0;

    // Speed != 1: audio is already dropped (audioInIdx = -1), so only video
    // pts/dts/duration get divided by `speed`.
    const bool speedScaled = range.speed > 0.0 && std::abs(range.speed - 1.0) > 1e-6;
    const double speedInv = speedScaled ? (1.0 / range.speed) : 1.0;
    AVBSFContext* videoBsf = nullptr;  // embedded frame-rate rewrite, speed-changed copies only

    for (int i = 0; i < static_cast<int>(inFmt->nb_streams); i++) {
        AVStream* inStream = inFmt->streams[i];
        if (i == videoInIdx || i == audioInIdx) {
            AVStream* outStream = avformat_new_stream(outFmt, nullptr);
            if (!outStream) {
                m_progress.SetError("Failed to create output stream");
                av_bsf_free(&videoBsf);
                avformat_free_context(outFmt);
                return false;
            }
            avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
            if (i == videoInIdx && speedScaled) {
                videoBsf = OpenTimingRewriteFilter(inFmt, videoInIdx, range.speed);
                // Carries the rewritten extradata into the container header.
                if (videoBsf) avcodec_parameters_copy(outStream->codecpar, videoBsf->par_out);
            }
            outStream->codecpar->codec_tag = 0;

            if (i == videoInIdx) videoOutIdx = outStreamCount;
            if (i == audioInIdx) audioOutIdx = outStreamCount;
            outStreamCount++;
        }
    }

    // Open output file
    if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outFmt->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            m_progress.SetError("Failed to open output file: " + ff::ErrorString(ret));
            av_bsf_free(&videoBsf);
            avformat_free_context(outFmt);
            return false;
        }
    }

    ret = avformat_write_header(outFmt, nullptr);
    if (ret < 0) {
        m_progress.SetError("Failed to write header: " + ff::ErrorString(ret));
        av_bsf_free(&videoBsf);
        avio_closep(&outFmt->pb);
        avformat_free_context(outFmt);
        return false;
    }

    // Snap the range to the frames shown at the marks (see SegmentFrames):
    // the edit list then starts on a frame boundary and the out frame is
    // included.
    SegmentFrames sf;
    bool snapped = (videoInIdx >= 0) && ResolveSegmentFrames(demuxer, range, sf);
    if (!snapped)
        LOG_WARN("Segment export: no video timestamps to snap to; cutting at raw mark times");

    // Seek to start of range. av_seek_frame with BACKWARD lands on the
    // keyframe at or before range.startSec — stream copy can only start at a
    // keyframe.
    demuxer.Seek(range.startSec);

    // Start/end PTS per stream come from the in/out frames, not the first
    // keyframe read. Packets between the pre-roll keyframe and the in frame
    // flow through with negative PTS; MP4/MOV muxers turn that into an edit
    // list so playback starts exactly on the in frame. MKV has no equivalent,
    // so the pre-roll plays as leading content there.
    // User seconds map 1:1 to raw pts seconds (the Player's clock is
    // `frame->pts * tb`, no start_time subtraction).
    // videoEndPts is the last video packet kept (inclusive); the segment ends
    // where that frame's display ends (videoEndTs), which is where audio cuts.
    AVRational vtb = (videoInIdx >= 0) ? inFmt->streams[videoInIdx]->time_base
                                       : AVRational{1, AV_TIME_BASE};
    int64_t videoStartPts = snapped ? sf.inPts  : ff::SecondsToPts(range.startSec, vtb);
    int64_t videoEndPts   = snapped ? sf.outPts : ff::SecondsToPts(range.endSec, vtb);
    int64_t videoEndTs    = snapped ? sf.outPts + sf.outDur : videoEndPts;
    int64_t audioStartPts = 0;
    int64_t audioEndPts   = 0;
    if (audioInIdx >= 0) {
        AVRational atb = inFmt->streams[audioInIdx]->time_base;
        audioStartPts = (videoInIdx >= 0) ? av_rescale_q(videoStartPts, vtb, atb)
                                          : ff::SecondsToPts(range.startSec, atb);
        audioEndPts   = (videoInIdx >= 0) ? av_rescale_q(videoEndTs, vtb, atb)
                                          : ff::SecondsToPts(range.endSec, atb);
    }

    double segDuration = static_cast<double>(videoEndTs - videoStartPts) * av_q2d(vtb);
    double effSegDuration = speedScaled ? segDuration / range.speed : segDuration;

    // Only MP4/MOV has an edit list to trim after muxing (see below).
    const bool isMp4 = outFmt->oformat && outFmt->oformat->name &&
                       (strcmp(outFmt->oformat->name, "mp4") == 0 ||
                        strcmp(outFmt->oformat->name, "mov") == 0);

    // Read and write packets
    AVPacket* pkt = av_packet_alloc();
    bool done = false;
    while (!done && !m_cancel) {
        ret = demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            m_progress.SetError("Read error: " + ff::ErrorString(ret));
            av_packet_free(&pkt);
            goto cleanup;
        }

        int inIdx = pkt->stream_index;
        int outIdx = -1;
        int64_t endPts = 0;

        if (inIdx == videoInIdx) {
            outIdx = videoOutIdx;
            endPts = videoEndPts;
        } else if (inIdx == audioInIdx) {
            outIdx = audioOutIdx;
            endPts = audioEndPts;
        } else {
            av_packet_unref(pkt);
            continue;
        }

        // Speed-changed copies: rewrite in-band parameter sets (see
        // OpenTimingRewriteFilter). The filter returns each packet immediately.
        if (inIdx == videoInIdx && videoBsf) {
            ret = av_bsf_send_packet(videoBsf, pkt);
            if (ret >= 0) ret = av_bsf_receive_packet(videoBsf, pkt);
            if (ret == AVERROR(EAGAIN)) continue;  // filter held the packet (never, for metadata filters)
            if (ret < 0) {
                m_progress.SetError("Frame-rate rewrite failed: " + ff::ErrorString(ret));
                av_packet_free(&pkt);
                goto cleanup;
            }
        }

        // Terminate via DTS (monotonic) with a margin past the out frame:
        // trailing B-frames arrive after it in decode order, and interleaved
        // audio may lag the video.
        if (inIdx == videoInIdx && pkt->dts != AV_NOPTS_VALUE) {
            int64_t marginTs = ff::SecondsToPts(1.0, vtb);
            if (pkt->dts > endPts + marginTs) {
                done = true;
                av_packet_unref(pkt);
                continue;
            }
        }

        // Drop packets past the segment, but keep reading — later packets in
        // decode order may still be within range. Video keeps everything up
        // to the out frame plus the frames decoded before it (references its
        // trailing B-frames may need); the MP4 edit list hides those, other
        // containers would show them. Audio ends at the segment's end.
        if (pkt->pts != AV_NOPTS_VALUE) {
            bool past;
            if (inIdx == videoInIdx) {
                bool decodedBeforeOut = snapped && isMp4 && pkt->dts != AV_NOPTS_VALUE &&
                                        sf.outDts != AV_NOPTS_VALUE && pkt->dts <= sf.outDts;
                past = pkt->pts > endPts && !decodedBeforeOut;
            } else {
                past = pkt->pts >= endPts;
            }
            if (past) {
                av_packet_unref(pkt);
                continue;
            }
        }

        // Rebase against the in frame; pre-roll packets get negative PTS/DTS,
        // which the MP4/MOV muxer turns into the edit list (see above).
        int64_t startPts = (inIdx == videoInIdx) ? videoStartPts : audioStartPts;
        AVStream* inStream = inFmt->streams[inIdx];
        AVStream* outStream = outFmt->streams[outIdx];

        pkt->stream_index = outIdx;
        if (pkt->pts != AV_NOPTS_VALUE) {
            pkt->pts = av_rescale_q(pkt->pts - startPts, inStream->time_base, outStream->time_base);
        }
        if (pkt->dts != AV_NOPTS_VALUE) {
            pkt->dts = av_rescale_q(pkt->dts - startPts, inStream->time_base, outStream->time_base);
        }
        pkt->duration = av_rescale_q(pkt->duration, inStream->time_base, outStream->time_base);
        pkt->pos = -1;

        // Per-segment speed: scale this packet's timestamps. Audio at
        // speed != 1 was already filtered out at stream-mapping time, so
        // only video reaches here.
        if (speedScaled) {
            if (pkt->pts != AV_NOPTS_VALUE)
                pkt->pts = static_cast<int64_t>(std::llround(static_cast<double>(pkt->pts) * speedInv));
            if (pkt->dts != AV_NOPTS_VALUE)
                pkt->dts = static_cast<int64_t>(std::llround(static_cast<double>(pkt->dts) * speedInv));
            pkt->duration = static_cast<int64_t>(std::llround(static_cast<double>(pkt->duration) * speedInv));
        }

        // Update progress within segment. Output timestamps are post-speed
        // scaled; compare to the effective (post-speed) duration so progress
        // tops out at 1.0 regardless of speed.
        if (effSegDuration > 0.0 && pkt->pts != AV_NOPTS_VALUE) {
            double pktTime = static_cast<double>(pkt->pts) * av_q2d(outStream->time_base);
            float segProgress = static_cast<float>(pktTime / effSegDuration);
            segProgress = std::max(0.0f, std::min(segProgress, 1.0f));

            int totalItems = std::max(1, m_progress.totalItems.load());
            float base = static_cast<float>(m_progress.currentItem - 1) / totalItems;
            m_progress.fraction = base + segProgress / totalItems;
        }

        ret = av_interleaved_write_frame(outFmt, pkt);
        if (ret < 0) {
            m_progress.SetError("Write error: " + ff::ErrorString(ret));
            av_packet_free(&pkt);
            goto cleanup;
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_bsf_free(&videoBsf);
    av_write_trailer(outFmt);

    if (!(outFmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&outFmt->pb);
    avformat_free_context(outFmt);

    // The trailing reference frames extended the muxer's edit list; pull it
    // back to the segment (post-speed duration).
    if (snapped && isMp4 && !TrimMp4Presentation(outputPath, effSegDuration))
        LOG_WARN("Segment export: could not trim the MP4 edit list; trailing reference frames may show");
    return true;

cleanup:
    av_bsf_free(&videoBsf);
    av_write_trailer(outFmt);
    if (!(outFmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&outFmt->pb);
    avformat_free_context(outFmt);
    return false;
}

// ---------------------------------------------------------------------------
// GIF export
// ---------------------------------------------------------------------------
bool Exporter::ExportSegmentGIF(const std::string& inputPath,
                                 const TimeRange& range,
                                 const std::string& outputPath,
                                 int gifWidth, double gifFps) {
    PROFILE_SCOPE();
    // Open input
    Demuxer demuxer;
    if (!demuxer.Open(inputPath, "export")) {
        m_progress.SetError("GIF: Failed to open input");
        return false;
    }

    VideoDecoder decoder;
    if (!decoder.Open(demuxer.GetVideoCodecParams(), /*quiet=*/true)) {
        m_progress.SetError("GIF: Failed to open video decoder");
        return false;
    }

    int srcW = decoder.GetWidth();
    int srcH = decoder.GetHeight();
    AVPixelFormat srcFmt = decoder.GetPixelFormat();
    AVRational srcTimeBase = demuxer.GetVideoTimeBase();
    double srcFps = demuxer.GetVideoFrameRate();

    // Sentinel: caller passes 0 / <= 0 to mean "match the source". Used when
    // the source itself is a GIF and the user picked SourceFormat — we still
    // have to re-encode (the GIF muxer has no edit-list trim), but we want
    // dimensions and frame rate to match the input.
    if (gifWidth <= 0) gifWidth = srcW;
    if (gifFps <= 0.0) gifFps = (srcFps > 0.0) ? srcFps : 15.0;

    // Get color space info from codec params to avoid filter graph warnings
    AVCodecParameters* vpar = demuxer.GetVideoCodecParams();
    AVColorSpace colorspace = vpar ? vpar->color_space : AVCOL_SPC_UNSPECIFIED;
    AVColorRange colorrange = vpar ? vpar->color_range : AVCOL_RANGE_UNSPECIFIED;

    // HDR sources can't be fed to the SDR palette filter as-is — they'd read
    // washed-out. Tone-map each frame to SDR RGBA8 via the shader first, then
    // feed RGBA into the graph. Detect HDR here and set up the tone-mapper.
    VideoColorMode colorMode = vpar ? FrameConverter::ColorModeForTransfer(vpar->color_trc)
                                    : VideoColorMode::SDR;
    VideoColorPrimaries colorPrimaries = vpar ? FrameConverter::PrimariesForTag(vpar->color_primaries)
                                              : VideoColorPrimaries::BT2020;
    const bool hdr = (colorMode != VideoColorMode::SDR);
    if (hdr && !EnsureTonemap()) {
        m_progress.SetError("GIF: HDR export needs the GPU tone-mapper, which is unavailable");
        return false;
    }

    // Compute output height maintaining aspect ratio (must be even)
    int outH = (gifWidth * srcH / srcW) & ~1;
    if (outH < 2) outH = 2;

    // --- Build filter graph ---
    // buffer -> fps -> scale -> split [a][b]; [a] palettegen [p]; [b][p] paletteuse -> buffersink
    AVFilterGraph* filterGraph = avfilter_graph_alloc();
    if (!filterGraph) {
        m_progress.SetError("GIF: Failed to alloc filter graph");
        return false;
    }

    // Buffer source (include color space info to avoid filter warnings). For HDR
    // we feed already-tone-mapped RGBA frames, so the source format is rgba and
    // no colorspace metadata is needed.
    char bufSrcArgs[512];
    if (hdr) {
        snprintf(bufSrcArgs, sizeof(bufSrcArgs),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:frame_rate=%d/1",
                 srcW, srcH, static_cast<int>(AV_PIX_FMT_RGBA),
                 srcTimeBase.num, srcTimeBase.den,
                 static_cast<int>(std::round(srcFps)));
    } else {
        snprintf(bufSrcArgs, sizeof(bufSrcArgs),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:frame_rate=%d/1"
                 ":colorspace=%d:range=%d",
                 srcW, srcH, static_cast<int>(srcFmt),
                 srcTimeBase.num, srcTimeBase.den,
                 static_cast<int>(std::round(srcFps)),
                 static_cast<int>(colorspace), static_cast<int>(colorrange));
    }

    AVFilterContext* bufSrcCtx = nullptr;
    AVFilterContext* bufSinkCtx = nullptr;

    int ret = avfilter_graph_create_filter(&bufSrcCtx, avfilter_get_by_name("buffer"),
                                            "in", bufSrcArgs, nullptr, filterGraph);
    if (ret < 0) {
        m_progress.SetError("GIF: Failed to create buffer source: " + ff::ErrorString(ret));
        avfilter_graph_free(&filterGraph);
        return false;
    }

    ret = avfilter_graph_create_filter(&bufSinkCtx, avfilter_get_by_name("buffersink"),
                                        "out", nullptr, nullptr, filterGraph);
    if (ret < 0) {
        m_progress.SetError("GIF: Failed to create buffer sink: " + ff::ErrorString(ret));
        avfilter_graph_free(&filterGraph);
        return false;
    }

    // Build the filter chain via avfilter_graph_parse_ptr
    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();

    outputs->name = av_strdup("in");
    outputs->filter_ctx = bufSrcCtx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = bufSinkCtx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    // Sample the source at gifFps/speed source-frames per source-second
    // so the output GIF plays at a constant gifFps over a duration of
    // src_duration/speed: slow-mo gets MORE unique frames (smoother
    // motion) and fast-forward gets FEWER (no redundant duplicates).
    double srcSampleFps = (range.speed > 0.0) ? gifFps / range.speed : gifFps;
    // The fps resampler keeps the LAST frame that rounds into a slot. With
    // the in frame rebased to t=0 (see feedFrame) and round=up, slot 0 can
    // only hold the in frame and each later slot holds the frame on screen at
    // that instant. eof_action=pass plus the out frame's stretched duration
    // makes the final slot emit the out frame.
    char filterDesc[512];
    if (hdr) {
        // HDR frames arrive already tone-mapped to sRGB BT.709 RGBA, so the
        // colorspace conversion is skipped — just resample, scale, and palettize.
        snprintf(filterDesc, sizeof(filterDesc),
                 "fps=fps=%.4f:round=up:eof_action=pass,scale=%d:%d:flags=lanczos,format=rgb24,"
                 "split[a][b];"
                 "[a]palettegen=stats_mode=full[p];"
                 "[b][p]paletteuse=dither=bayer:bayer_scale=3",
                 srcSampleFps, gifWidth, outH);
    } else {
        snprintf(filterDesc, sizeof(filterDesc),
                 "fps=fps=%.4f:round=up:eof_action=pass,scale=%d:%d:flags=lanczos,"
                 "format=rgb24,colorspace=all=bt709:iall=bt709:fast=1,"
                 "setparams=colorspace=bt709:color_primaries=bt709:color_trc=iec61966-2-1,"
                 "split[a][b];"
                 "[a]palettegen=stats_mode=full[p];"
                 "[b][p]paletteuse=dither=bayer:bayer_scale=3",
                 srcSampleFps, gifWidth, outH);
    }

    ret = avfilter_graph_parse_ptr(filterGraph, filterDesc, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        m_progress.SetError("GIF: Failed to parse filter graph: " + ff::ErrorString(ret));
        avfilter_graph_free(&filterGraph);
        return false;
    }

    ret = avfilter_graph_config(filterGraph, nullptr);
    if (ret < 0) {
        m_progress.SetError("GIF: Failed to configure filter graph: " + ff::ErrorString(ret));
        avfilter_graph_free(&filterGraph);
        return false;
    }

    // --- Set up GIF output ---
    AVFormatContext* outFmt = nullptr;
    ret = avformat_alloc_output_context2(&outFmt, nullptr, "gif", outputPath.c_str());
    if (ret < 0 || !outFmt) {
        m_progress.SetError("GIF: Failed to create output context: " + ff::ErrorString(ret));
        avfilter_graph_free(&filterGraph);
        return false;
    }

    const AVCodec* gifCodec = avcodec_find_encoder(AV_CODEC_ID_GIF);
    if (!gifCodec) {
        m_progress.SetError("GIF: GIF encoder not found");
        avformat_free_context(outFmt);
        avfilter_graph_free(&filterGraph);
        return false;
    }

    AVStream* outStream = avformat_new_stream(outFmt, gifCodec);
    AVCodecContext* encCtx = avcodec_alloc_context3(gifCodec);
    encCtx->width = gifWidth;
    encCtx->height = outH;
    encCtx->pix_fmt = AV_PIX_FMT_PAL8;
    // GIF stores delays in centiseconds (1/100s), so use that as time_base
    // to avoid rounding errors that cause wrong playback speed
    encCtx->time_base = {1, 100};
    outStream->time_base = {1, 100};

    if (outFmt->oformat->flags & AVFMT_GLOBALHEADER)
        encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(encCtx, gifCodec, nullptr);
    if (ret < 0) {
        m_progress.SetError("GIF: Failed to open encoder: " + ff::ErrorString(ret));
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        avfilter_graph_free(&filterGraph);
        return false;
    }

    avcodec_parameters_from_context(outStream->codecpar, encCtx);

    if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outFmt->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            m_progress.SetError("GIF: Failed to open output: " + ff::ErrorString(ret));
            avcodec_free_context(&encCtx);
            avformat_free_context(outFmt);
            avfilter_graph_free(&filterGraph);
            return false;
        }
    }

    ret = avformat_write_header(outFmt, nullptr);
    if (ret < 0) {
        m_progress.SetError("GIF: Failed to write header: " + ff::ErrorString(ret));
        avcodec_free_context(&encCtx);
        avio_closep(&outFmt->pb);
        avformat_free_context(outFmt);
        avfilter_graph_free(&filterGraph);
        return false;
    }

    // Frames shown at the marks (see SegmentFrames); raw mark times if the
    // stream has no usable timestamps.
    SegmentFrames sf;
    if (!ResolveSegmentFrames(demuxer, range, sf)) {
        sf.inPts  = ff::SecondsToPts(range.startSec, srcTimeBase);
        sf.outPts = ff::SecondsToPts(range.endSec, srcTimeBase);
    }

    // --- Decode, filter, encode loop ---
    demuxer.Seek(range.startSec);
    decoder.Flush();

    AVPacket* pkt = av_packet_alloc();
    AVFrame* decFrame = av_frame_alloc();
    AVFrame* filtFrame = av_frame_alloc();
    AVPacket* encPkt = av_packet_alloc();

    // HDR only: converts each decoded frame to packed 10-bit, then the shader
    // tone-maps it to the SDR RGBA8 that gets fed into the (rgba) filter graph.
    FrameConverter gifConv;
    std::vector<uint8_t> tmRGBA;

    int videoIdx = demuxer.GetVideoStreamIndex();
    double segStartSec = static_cast<double>(sf.inPts) * av_q2d(srcTimeBase);
    double segDuration = static_cast<double>(sf.outPts - sf.inPts) * av_q2d(srcTimeBase);
    // One output interval of the fps resampler, in source ticks.
    const int64_t outIntervalTs = ff::SecondsToPts(1.0 / srcSampleFps, srcTimeBase) + 1;
    int64_t frameCount = 0;

    auto encodeFilteredFrames = [&]() -> bool {
        PROFILE_SCOPE_N("GIF::EncodeFilteredFrames");
        while (true) {
            ret = av_buffersink_get_frame(bufSinkCtx, filtFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return true;
            if (ret < 0) {
                m_progress.SetError("GIF: Filter error: " + ff::ErrorString(ret));
                return false;
            }

            // PTS in centiseconds (time_base = 1/100). Output frames play
            // at the requested gifFps regardless of speed — the speed
            // effect comes from sampling the source at gifFps/speed above.
            filtFrame->pts = static_cast<int64_t>(std::round(frameCount * 100.0 / gifFps));
            frameCount++;

            ret = avcodec_send_frame(encCtx, filtFrame);
            av_frame_unref(filtFrame);
            if (ret < 0) {
                m_progress.SetError("GIF: Encode send error: " + ff::ErrorString(ret));
                return false;
            }

            while (true) {
                ret = avcodec_receive_packet(encCtx, encPkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    m_progress.SetError("GIF: Encode receive error: " + ff::ErrorString(ret));
                    return false;
                }
                av_packet_rescale_ts(encPkt, encCtx->time_base, outStream->time_base);
                encPkt->stream_index = 0;
                av_interleaved_write_frame(outFmt, encPkt);
                av_packet_unref(encPkt);
            }
        }
    };

    // Feed one decoded frame to the filter graph and encode whatever comes
    // out. Returns +1 to keep going, 0 once the out frame has been fed, -1 on
    // error (progress error already set).
    auto feedFrame = [&](AVFrame* frame) -> int {
        int64_t pts = frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = frame->pts;
        if (pts < sf.inPts) return 1;  // keyframe pre-roll before the in frame

        // Stretch the out frame to a full output interval so the end of
        // stream lands in the next slot and the resampler emits it.
        if (pts >= sf.outPts)
            frame->duration = std::max<int64_t>(frame->duration, outIntervalTs);

        // Update progress
        if (segDuration > 0.0) {
            double frameTime = static_cast<double>(pts) * av_q2d(srcTimeBase);
            float segProgress = static_cast<float>((frameTime - segStartSec) / segDuration);
            int totalItems = std::max(1, m_progress.totalItems.load());
            float base = static_cast<float>(m_progress.currentItem - 1) / totalItems;
            m_progress.fraction = base + std::max(0.0f, std::min(segProgress, 1.0f)) / totalItems;
        }

        // In frame at t=0 so it owns the resampler's first slot; output
        // timestamps are regenerated anyway.
        frame->pts = pts - sf.inPts;

        if (hdr) {
            // Tone-map the HDR frame to SDR RGBA8, wrap it in an rgba AVFrame
            // (keeping the source pts/time_base), and feed that to the graph.
            const uint8_t* packed = gifConv.Convert(frame);
            if (!packed || !m_tonemap.RenderToBuffer(packed, gifConv.GetWidth(),
                                                     gifConv.GetHeight(), colorMode,
                                                     colorPrimaries, m_settings.tonemapper,
                                                     tmRGBA)) {
                m_progress.SetError("GIF: HDR tone-map failed");
                return -1;
            }
            AVFrame* rgbaFrame = av_frame_alloc();
            rgbaFrame->format = AV_PIX_FMT_RGBA;
            rgbaFrame->width = gifConv.GetWidth();
            rgbaFrame->height = gifConv.GetHeight();
            if (av_frame_get_buffer(rgbaFrame, 0) < 0) {
                av_frame_free(&rgbaFrame);
                m_progress.SetError("GIF: Failed to alloc RGBA frame");
                return -1;
            }
            for (int y = 0; y < rgbaFrame->height; y++) {
                memcpy(rgbaFrame->data[0] + static_cast<size_t>(y) * rgbaFrame->linesize[0],
                       tmRGBA.data() + static_cast<size_t>(y) * rgbaFrame->width * 4,
                       static_cast<size_t>(rgbaFrame->width) * 4);
            }
            rgbaFrame->pts = frame->pts;
            rgbaFrame->duration = frame->duration;
            ret = av_buffersrc_add_frame_flags(bufSrcCtx, rgbaFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
            av_frame_free(&rgbaFrame);
        } else {
            ret = av_buffersrc_add_frame(bufSrcCtx, frame);
        }
        if (ret < 0) {
            m_progress.SetError("GIF: Failed to feed filter: " + ff::ErrorString(ret));
            return -1;
        }

        if (!encodeFilteredFrames()) return -1;
        // Presentation order: the first frame at or past the out pts is the
        // out frame.
        return (pts >= sf.outPts) ? 0 : 1;
    };

    // Read until the out frame has been fed; drain the decoder at EOF so an
    // out frame in the last GOP isn't lost.
    int feedStatus = 1;
    while (feedStatus > 0 && !m_cancel) {
        ret = demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) {
            av_packet_unref(pkt);
            decoder.DrainAtEOF(decFrame, [&](AVFrame* f) {
                feedStatus = feedFrame(f);
                return feedStatus > 0;
            });
            if (feedStatus > 0) feedStatus = 0;  // ran out of frames first
            break;
        }
        if (ret < 0) {
            av_packet_unref(pkt);
            break;  // read error: finish the GIF with what we have
        }
        if (pkt->stream_index != videoIdx) {
            av_packet_unref(pkt);
            continue;
        }

        decoder.SendPacket(pkt);
        av_packet_unref(pkt);

        while (feedStatus > 0) {
            ret = decoder.ReceiveFrame(decFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { feedStatus = 0; break; }
            feedStatus = feedFrame(decFrame);
            av_frame_unref(decFrame);
        }
    }
    if (feedStatus < 0) goto gif_cleanup;

    // Flush filter graph
    if (av_buffersrc_add_frame(bufSrcCtx, nullptr) < 0) goto gif_cleanup;
    if (!encodeFilteredFrames()) goto gif_cleanup;

    // Flush encoder
    avcodec_send_frame(encCtx, nullptr);
    while (true) {
        ret = avcodec_receive_packet(encCtx, encPkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        av_packet_rescale_ts(encPkt, encCtx->time_base, outStream->time_base);
        encPkt->stream_index = 0;
        av_interleaved_write_frame(outFmt, encPkt);
        av_packet_unref(encPkt);
    }

    av_write_trailer(outFmt);

    av_packet_free(&pkt);
    av_frame_free(&decFrame);
    av_frame_free(&filtFrame);
    av_packet_free(&encPkt);
    avcodec_free_context(&encCtx);
    if (!(outFmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&outFmt->pb);
    avformat_free_context(outFmt);
    avfilter_graph_free(&filterGraph);
    return true;

gif_cleanup:
    av_write_trailer(outFmt);
    av_packet_free(&pkt);
    av_frame_free(&decFrame);
    av_frame_free(&filtFrame);
    av_packet_free(&encPkt);
    avcodec_free_context(&encCtx);
    if (!(outFmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&outFmt->pb);
    avformat_free_context(outFmt);
    avfilter_graph_free(&filterGraph);
    return false;
}

// ---------------------------------------------------------------------------
// PNG still-frame export
// ---------------------------------------------------------------------------
bool Exporter::ExportFramePNG(const std::string& inputPath,
                               const FrameMark& frame,
                               const std::string& outputPath) {
    PROFILE_SCOPE();
    Demuxer demuxer;
    if (!demuxer.Open(inputPath, "export")) {
        m_progress.SetError("Failed to open input: " + inputPath);
        return false;
    }

    AVCodecParameters* vparams = demuxer.GetVideoCodecParams();
    if (!vparams) {
        m_progress.SetError("Input has no video stream");
        return false;
    }

    VideoDecoder decoder;
    if (!decoder.Open(vparams, /*quiet=*/true)) {
        m_progress.SetError("Failed to open video decoder");
        return false;
    }

    if (!demuxer.Seek(frame.timeSec)) {
        m_progress.SetError("Seek failed for frame at " + std::to_string(frame.timeSec) + "s");
        return false;
    }

    // Resolve the mark to a frame the way the player's seek does
    // (Player::SyncSeekAndDecode): the first frame at or past the target. A
    // mark between two frames exports the later one, the frame on screen at
    // that time.
    AVRational tb = demuxer.GetVideoTimeBase();
    int64_t targetPts = ff::SecondsToPts(frame.timeSec, tb);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* decFrame = av_frame_alloc();
    // First frame at or past the target; until one arrives, the latest
    // decoded, so a mark past the last frame exports that frame.
    AVFrame* captured = nullptr;
    bool found = false;

    while (!found && !m_cancel) {
        int rr = demuxer.ReadPacket(pkt);
        if (rr < 0) {
            // EOF or error — flush decoder
            decoder.SendPacket(nullptr);
        } else if (pkt->stream_index != demuxer.GetVideoStreamIndex()) {
            av_packet_unref(pkt);
            continue;
        } else {
            decoder.SendPacket(pkt);
            av_packet_unref(pkt);
        }

        while (true) {
            int rf = decoder.ReceiveFrame(decFrame);
            if (rf == AVERROR(EAGAIN) || rf == AVERROR_EOF) break;
            if (rf < 0) {
                m_progress.SetError("Decode error: " + ff::ErrorString(rf));
                av_packet_free(&pkt);
                av_frame_free(&decFrame);
                if (captured) av_frame_free(&captured);
                return false;
            }
            int64_t pts = decFrame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = decFrame->pts;
            if (captured) av_frame_free(&captured);
            captured = av_frame_clone(decFrame);
            av_frame_unref(decFrame);
            // Presentation order, so this is the first frame at or past the
            // target. Also covers a mark at 0.0s on a stream whose first
            // frame starts later.
            if (pts >= targetPts) {
                found = true;
                break;
            }
        }
        if (rr < 0) break;  // EOF after flush
    }

    av_packet_free(&pkt);
    av_frame_free(&decFrame);

    if (m_cancel) {
        if (captured) av_frame_free(&captured);
        return false;
    }
    if (!captured) {
        m_progress.SetError("No frame decoded at " + std::to_string(frame.timeSec) + "s");
        return false;
    }
    {
        int64_t pts = captured->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = captured->pts;
        LOG_INFO("Frame export: mark %.3fs -> frame pts %.3fs%s", frame.timeSec,
                 static_cast<double>(pts) * av_q2d(tb), found ? "" : " (last frame)");
    }

    FrameConverter conv;
    const uint8_t* rgba = conv.Convert(captured);
    int W = conv.GetWidth();
    int H = conv.GetHeight();

    if (!rgba || W <= 0 || H <= 0) {
        av_frame_free(&captured);
        m_progress.SetError("Frame conversion failed");
        return false;
    }

    // HDR frames come back as 10-bit packed X2BGR10LE, which stb would misread
    // as 8-bit RGBA. Tone-map them to SDR RGBA8 through the same shader the
    // display uses. Requires the shared GPU device.
    std::vector<uint8_t> tonemapped;
    if (conv.GetColorMode() != VideoColorMode::SDR) {
        if (!EnsureTonemap()) {
            av_frame_free(&captured);
            m_progress.SetError("HDR frame export needs the GPU tone-mapper, which is unavailable");
            return false;
        }
        VideoColorPrimaries prim = FrameConverter::PrimariesForTag(vparams->color_primaries);
        if (!m_tonemap.RenderToBuffer(rgba, W, H, conv.GetColorMode(), prim,
                                      m_settings.tonemapper, tonemapped)) {
            av_frame_free(&captured);
            m_progress.SetError("HDR tone-map failed for frame export");
            return false;
        }
        rgba = tonemapped.data();
    }

    int ok = stbi_write_png(outputPath.c_str(), W, H, 4, rgba, W * 4);
    av_frame_free(&captured);

    if (!ok) {
        m_progress.SetError("Failed to write PNG: " + outputPath);
        return false;
    }
    return true;
}
