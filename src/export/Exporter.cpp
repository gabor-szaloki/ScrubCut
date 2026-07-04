#include "export/Exporter.h"
#include "core/Demuxer.h"
#include "core/VideoDecoder.h"
#include "core/AudioDecoder.h"
#include "core/FrameConverter.h"
#include "util/Log.h"
#include "util/FFmpegUtils.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

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

    // Release the tone-mapper's GPU resources (deferred-safe from this thread).
    // Must run on every exit path; called before every `return` below.
    auto finish = [&]() {
        m_tonemap.Shutdown();
        m_progress.running = false;
    };

    int totalItems = static_cast<int>(m_settings.segments.size() + m_settings.frames.size());
    int itemsDone = 0;
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
        if (srcIsGif) {
            // GIF source: output is always GIF at source W/FPS, regardless
            // of seg.mode (the UI locks the toggle, but old segments may
            // still carry mode==GIF; treat them as SourceFormat anyway).
            std::string outPath = BuildOutputPath(m_settings.outputPath, seg.name, i, ".gif");
            LOG_INFO("Exporting segment %d/%d (GIF, source params) -> %s",
                     itemsDone + 1, totalItems, outPath.c_str());
            // 0/0.0 = "match source W/FPS" — see ExportSegmentGIF.
            ok = ExportSegmentGIF(m_inputPath, seg, outPath, 0, 0.0);
        } else if (seg.mode == ExportMode::SourceFormat) {
            std::string outPath = BuildOutputPath(m_settings.outputPath, seg.name, i, sourceFormatExt);
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
            std::string outPath = BuildOutputPath(m_settings.outputPath, seg.name, i, ".gif");
            LOG_INFO("Exporting segment %d/%d (GIF) -> %s",
                     itemsDone + 1, totalItems, outPath.c_str());
            ok = ExportSegmentGIF(m_inputPath, seg, outPath,
                                  m_settings.gifWidth, m_settings.gifFps);
        }

        if (!ok && !m_cancel) {
            finish();
            return;
        }
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
        suffix = "_" + markName;
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "_%03d", fallbackIndex + 1);
        suffix = buf;
    }

    std::filesystem::path result = std::filesystem::path(dir) / (stem + suffix + extension);
    return result.string();
}

// ---------------------------------------------------------------------------
// Stream copy export
// ---------------------------------------------------------------------------
bool Exporter::ExportSegmentStreamCopy(const std::string& inputPath,
                                        const TimeRange& range,
                                        const std::string& outputPath) {
    Demuxer demuxer;
    if (!demuxer.Open(inputPath)) {
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

    for (int i = 0; i < static_cast<int>(inFmt->nb_streams); i++) {
        AVStream* inStream = inFmt->streams[i];
        if (i == videoInIdx || i == audioInIdx) {
            AVStream* outStream = avformat_new_stream(outFmt, nullptr);
            if (!outStream) {
                m_progress.SetError("Failed to create output stream");
                avformat_free_context(outFmt);
                return false;
            }
            avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
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
            avformat_free_context(outFmt);
            return false;
        }
    }

    ret = avformat_write_header(outFmt, nullptr);
    if (ret < 0) {
        m_progress.SetError("Failed to write header: " + ff::ErrorString(ret));
        avio_closep(&outFmt->pb);
        avformat_free_context(outFmt);
        return false;
    }

    // Seek to start of range. av_seek_frame with BACKWARD lands on the
    // keyframe at or before range.startSec — stream copy can only start at a
    // keyframe.
    demuxer.Seek(range.startSec);

    // Compute start/end PTS for each stream from the mark, NOT from the first
    // keyframe we read. Packets between the pre-roll keyframe and the mark-in
    // flow through with negative PTS; MP4/MOV muxers pick that up and emit an
    // edit list so players skip the pre-roll on playback and start at mark-in
    // frame-accurately. MKV has no equivalent — the pre-roll will play as
    // leading content in .mkv exports.
    // Convert user timeline seconds to raw stream PTS. The Player's clock
    // tracks time as `frame->pts * tb` directly (no stream start_time
    // subtraction), so user seconds correspond 1:1 to raw pts seconds.
    auto toPts = [](double sec, AVStream* s) -> int64_t {
        return static_cast<int64_t>(sec / av_q2d(s->time_base));
    };

    int64_t videoStartPts = (videoInIdx >= 0) ? toPts(range.startSec, inFmt->streams[videoInIdx]) : 0;
    int64_t audioStartPts = (audioInIdx >= 0) ? toPts(range.startSec, inFmt->streams[audioInIdx]) : 0;
    int64_t videoEndPts   = (videoInIdx >= 0) ? toPts(range.endSec,   inFmt->streams[videoInIdx]) : 0;
    int64_t audioEndPts   = (audioInIdx >= 0) ? toPts(range.endSec,   inFmt->streams[audioInIdx]) : 0;

    double segDuration = range.endSec - range.startSec;
    // Speed scaling. At speed != 1, audio packets are filtered out earlier
    // (audioInIdx = -1 above) so this block only touches video packets:
    // dividing video pts/dts/duration by `speed` compresses (>1×) or
    // stretches (<1×) the output timeline.
    bool speedScaled = range.speed > 0.0 && std::abs(range.speed - 1.0) > 1e-6;
    double speedInv = speedScaled ? (1.0 / range.speed) : 1.0;
    double effSegDuration = speedScaled ? segDuration / range.speed : segDuration;

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

        // Terminate via DTS (monotonic) with a safety margin for B-frame
        // reorder depth — stopping on first pts > endPts would drop trailing
        // B-frames whose pts falls at/before mark-out but which arrive later
        // in decode order.
        if (inIdx == videoInIdx && pkt->dts != AV_NOPTS_VALUE) {
            AVRational vtb = inFmt->streams[videoInIdx]->time_base;
            int64_t marginTs = static_cast<int64_t>(1.0 / av_q2d(vtb));
            if (pkt->dts > endPts + marginTs) {
                done = true;
                av_packet_unref(pkt);
                continue;
            }
        }

        // Drop packets whose display time is past mark-out, but keep reading
        // — later packets in decode order may still be within range.
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts > endPts) {
            av_packet_unref(pkt);
            continue;
        }

        // Rebase timestamps against mark-in. Packets before mark-in (keyframe
        // pre-roll) will get negative PTS/DTS — intentional. The muxer
        // flows these into an edit list for MP4/MOV so playback skips the
        // pre-roll. (MKV has no edit list equivalent; pre-roll will play.)
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
    av_write_trailer(outFmt);

    if (!(outFmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&outFmt->pb);
    avformat_free_context(outFmt);
    return true;

cleanup:
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
    // Open input
    Demuxer demuxer;
    if (!demuxer.Open(inputPath)) {
        m_progress.SetError("GIF: Failed to open input");
        return false;
    }

    VideoDecoder decoder;
    if (!decoder.Open(demuxer.GetVideoCodecParams())) {
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
    char filterDesc[512];
    if (hdr) {
        // HDR frames arrive already tone-mapped to sRGB BT.709 RGBA, so the
        // colorspace conversion is skipped — just resample, scale, and palettize.
        snprintf(filterDesc, sizeof(filterDesc),
                 "fps=fps=%.4f,scale=%d:%d:flags=lanczos,format=rgb24,"
                 "split[a][b];"
                 "[a]palettegen=stats_mode=full[p];"
                 "[b][p]paletteuse=dither=bayer:bayer_scale=3",
                 srcSampleFps, gifWidth, outH);
    } else {
        snprintf(filterDesc, sizeof(filterDesc),
                 "fps=fps=%.4f,scale=%d:%d:flags=lanczos,"
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
    double segDuration = range.endSec - range.startSec;
    bool inputDone = false;
    int64_t frameCount = 0;

    auto encodeFilteredFrames = [&]() -> bool {
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

    // Feed frames into filter graph
    while (!inputDone && !m_cancel) {
        ret = demuxer.ReadPacket(pkt);
        if (ret == AVERROR_EOF) {
            inputDone = true;
            av_packet_unref(pkt);
            break;
        }
        if (ret < 0 || pkt->stream_index != videoIdx) {
            av_packet_unref(pkt);
            if (ret < 0 && ret != AVERROR_EOF) {
                inputDone = true;
            }
            continue;
        }

        // Check if past end of range
        double pktTime = static_cast<double>(pkt->pts) * av_q2d(srcTimeBase);
        if (pktTime > range.endSec) {
            av_packet_unref(pkt);
            inputDone = true;
            break;
        }

        decoder.SendPacket(pkt);
        av_packet_unref(pkt);

        while (true) {
            ret = decoder.ReceiveFrame(decFrame);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) { inputDone = true; break; }
            if (ret < 0) { inputDone = true; break; }

            double frameTime = static_cast<double>(decFrame->pts) * av_q2d(srcTimeBase);

            // Skip frames before range start
            if (frameTime < range.startSec) {
                av_frame_unref(decFrame);
                continue;
            }
            if (frameTime > range.endSec) {
                av_frame_unref(decFrame);
                inputDone = true;
                break;
            }

            // Update progress
            if (segDuration > 0.0) {
                float segProgress = static_cast<float>((frameTime - range.startSec) / segDuration);
                int totalItems = std::max(1, m_progress.totalItems.load());
                float base = static_cast<float>(m_progress.currentItem - 1) / totalItems;
                m_progress.fraction = base + std::max(0.0f, std::min(segProgress, 1.0f)) / totalItems;
            }

            if (hdr) {
                // Tone-map the HDR frame to SDR RGBA8, wrap it in an rgba AVFrame
                // (keeping the source pts/time_base), and feed that to the graph.
                const uint8_t* packed = gifConv.Convert(decFrame);
                if (!packed || !m_tonemap.RenderToBuffer(packed, gifConv.GetWidth(),
                                                         gifConv.GetHeight(), colorMode,
                                                         colorPrimaries, m_settings.tonemapper,
                                                         tmRGBA)) {
                    av_frame_unref(decFrame);
                    m_progress.SetError("GIF: HDR tone-map failed");
                    goto gif_cleanup;
                }
                AVFrame* rgbaFrame = av_frame_alloc();
                rgbaFrame->format = AV_PIX_FMT_RGBA;
                rgbaFrame->width = gifConv.GetWidth();
                rgbaFrame->height = gifConv.GetHeight();
                if (av_frame_get_buffer(rgbaFrame, 0) < 0) {
                    av_frame_free(&rgbaFrame);
                    av_frame_unref(decFrame);
                    m_progress.SetError("GIF: Failed to alloc RGBA frame");
                    goto gif_cleanup;
                }
                for (int y = 0; y < rgbaFrame->height; y++) {
                    memcpy(rgbaFrame->data[0] + static_cast<size_t>(y) * rgbaFrame->linesize[0],
                           tmRGBA.data() + static_cast<size_t>(y) * rgbaFrame->width * 4,
                           static_cast<size_t>(rgbaFrame->width) * 4);
                }
                rgbaFrame->pts = decFrame->pts;
                ret = av_buffersrc_add_frame_flags(bufSrcCtx, rgbaFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
                av_frame_free(&rgbaFrame);
            } else {
                ret = av_buffersrc_add_frame(bufSrcCtx, decFrame);
            }
            av_frame_unref(decFrame);
            if (ret < 0) {
                m_progress.SetError("GIF: Failed to feed filter: " + ff::ErrorString(ret));
                goto gif_cleanup;
            }

            if (!encodeFilteredFrames()) goto gif_cleanup;
        }
    }

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
    Demuxer demuxer;
    if (!demuxer.Open(inputPath)) {
        m_progress.SetError("Failed to open input: " + inputPath);
        return false;
    }

    AVCodecParameters* vparams = demuxer.GetVideoCodecParams();
    if (!vparams) {
        m_progress.SetError("Input has no video stream");
        return false;
    }

    VideoDecoder decoder;
    if (!decoder.Open(vparams)) {
        m_progress.SetError("Failed to open video decoder");
        return false;
    }

    if (!demuxer.Seek(frame.timeSec)) {
        m_progress.SetError("Seek failed for frame at " + std::to_string(frame.timeSec) + "s");
        return false;
    }

    AVRational tb = demuxer.GetVideoTimeBase();
    int64_t targetPts = static_cast<int64_t>(frame.timeSec / av_q2d(tb));

    AVPacket* pkt = av_packet_alloc();
    AVFrame* decFrame = av_frame_alloc();
    AVFrame* captured = nullptr;  // the latest frame whose PTS <= target
    bool pastTarget = false;

    while (!pastTarget && !m_cancel) {
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
            if (pts <= targetPts) {
                // Keep the most recent frame at-or-before the target.
                if (captured) av_frame_free(&captured);
                captured = av_frame_clone(decFrame);
            } else {
                pastTarget = true;
                av_frame_unref(decFrame);
                break;
            }
            av_frame_unref(decFrame);
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
