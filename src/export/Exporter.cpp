#include "export/Exporter.h"
#include "core/Demuxer.h"
#include "core/VideoDecoder.h"
#include "core/AudioDecoder.h"
#include "core/FrameConverter.h"
#include "util/Log.h"
#include "util/FFmpegUtils.h"

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
    m_progress.totalSegments = static_cast<int>(settings.segments.size());
    m_progress.running = true;

    m_thread = std::thread(&Exporter::ExportThread, this);
}

void Exporter::Cancel() {
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

void Exporter::ExportThread() {
    SetCurrentThreadName(L"ScrubCut Export");

    for (int i = 0; i < static_cast<int>(m_settings.segments.size()); i++) {
        if (m_cancel) break;

        m_progress.currentSegment = i + 1;
        const auto& seg = m_settings.segments[i];

        bool ok = false;
        if (seg.mode == ExportMode::SourceFormat) {
            std::string ext = std::filesystem::path(m_inputPath).extension().string();
            std::string outPath = BuildOutputPath(m_settings.outputPath, i,
                                                   static_cast<int>(m_settings.segments.size()), ext);
            LOG_INFO("Exporting segment %d/%d (stream copy) -> %s",
                     i + 1, static_cast<int>(m_settings.segments.size()), outPath.c_str());
            ok = ExportSegmentStreamCopy(m_inputPath, seg, outPath);
        } else {
            std::string outPath = BuildOutputPath(m_settings.outputPath, i,
                                                   static_cast<int>(m_settings.segments.size()), ".gif");
            LOG_INFO("Exporting segment %d/%d (GIF) -> %s",
                     i + 1, static_cast<int>(m_settings.segments.size()), outPath.c_str());
            ok = ExportSegmentGIF(m_inputPath, seg, outPath,
                                  m_settings.gifWidth, m_settings.gifFps);
        }

        if (!ok && !m_cancel) {
            // Error already set inside the export function
            m_progress.running = false;
            return;
        }

        // Update overall progress
        m_progress.fraction = static_cast<float>(i + 1) / static_cast<float>(m_settings.segments.size());
    }

    if (!m_cancel) {
        m_progress.fraction = 1.0f;
        m_progress.finished = true;
        LOG_INFO("Export complete");
    }
    m_progress.running = false;
}

std::string Exporter::BuildOutputPath(const std::string& basePath, int segmentIndex,
                                       int totalSegments, const std::string& extension) const {
    std::filesystem::path base(basePath);
    std::string stem = base.stem().string();
    std::string dir = base.parent_path().string();

    char suffix[32];
    snprintf(suffix, sizeof(suffix), "_%03d", segmentIndex + 1);

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

    // Map streams: copy video and audio
    int videoInIdx = demuxer.GetVideoStreamIndex();
    int audioInIdx = demuxer.GetAudioStreamIndex();
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

    // Seek to start of range
    demuxer.Seek(range.startSec);

    // Compute end PTS for each stream
    auto toPts = [](double sec, AVRational tb) -> int64_t {
        return static_cast<int64_t>(sec / av_q2d(tb));
    };

    int64_t videoEndPts = (videoInIdx >= 0) ? toPts(range.endSec, inFmt->streams[videoInIdx]->time_base) : 0;
    int64_t audioEndPts = (audioInIdx >= 0) ? toPts(range.endSec, inFmt->streams[audioInIdx]->time_base) : 0;

    // We'll determine the actual start offset from the first keyframe we read
    // (the seek lands on a keyframe at or before startSec)
    int64_t videoStartPts = AV_NOPTS_VALUE;
    int64_t audioStartPts = AV_NOPTS_VALUE;

    double segDuration = range.endSec - range.startSec;

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
            // Capture actual start PTS from first video packet (keyframe after seek)
            if (videoStartPts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
                videoStartPts = pkt->pts;
            endPts = videoEndPts;
        } else if (inIdx == audioInIdx) {
            outIdx = audioOutIdx;
            if (audioStartPts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
                audioStartPts = pkt->pts;
            endPts = audioEndPts;
        } else {
            av_packet_unref(pkt);
            continue;
        }

        // Check if past end
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts > endPts) {
            if (inIdx == videoInIdx) done = true;
            av_packet_unref(pkt);
            continue;
        }

        // Get the start offset for this stream
        int64_t startPts = (inIdx == videoInIdx) ? videoStartPts : audioStartPts;
        if (startPts == AV_NOPTS_VALUE) startPts = 0;

        // Remap timestamps relative to the actual first packet
        AVStream* inStream = inFmt->streams[inIdx];
        AVStream* outStream = outFmt->streams[outIdx];

        pkt->stream_index = outIdx;
        if (pkt->pts != AV_NOPTS_VALUE) {
            pkt->pts = av_rescale_q(pkt->pts - startPts, inStream->time_base, outStream->time_base);
            if (pkt->pts < 0) pkt->pts = 0;
        }
        if (pkt->dts != AV_NOPTS_VALUE) {
            pkt->dts = av_rescale_q(pkt->dts - startPts, inStream->time_base, outStream->time_base);
            if (pkt->dts < 0) pkt->dts = 0;
        }
        pkt->duration = av_rescale_q(pkt->duration, inStream->time_base, outStream->time_base);
        pkt->pos = -1;

        // Update progress within segment
        if (segDuration > 0.0 && pkt->pts != AV_NOPTS_VALUE) {
            double pktTime = static_cast<double>(pkt->pts) * av_q2d(outStream->time_base);
            float segProgress = static_cast<float>(pktTime / segDuration);
            segProgress = std::max(0.0f, std::min(segProgress, 1.0f));

            int totalSegs = static_cast<int>(m_settings.segments.size());
            float base = static_cast<float>(m_progress.currentSegment - 1) / totalSegs;
            m_progress.fraction = base + segProgress / totalSegs;
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

    // Get color space info from codec params to avoid filter graph warnings
    AVCodecParameters* vpar = demuxer.GetVideoCodecParams();
    AVColorSpace colorspace = vpar ? vpar->color_space : AVCOL_SPC_UNSPECIFIED;
    AVColorRange colorrange = vpar ? vpar->color_range : AVCOL_RANGE_UNSPECIFIED;

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

    // Buffer source (include color space info to avoid filter warnings)
    char bufSrcArgs[512];
    snprintf(bufSrcArgs, sizeof(bufSrcArgs),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:frame_rate=%d/1"
             ":colorspace=%d:range=%d",
             srcW, srcH, static_cast<int>(srcFmt),
             srcTimeBase.num, srcTimeBase.den,
             static_cast<int>(std::round(srcFps)),
             static_cast<int>(colorspace), static_cast<int>(colorrange));

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

    char filterDesc[512];
    snprintf(filterDesc, sizeof(filterDesc),
             "fps=fps=%.2f,scale=%d:%d:flags=lanczos,"
             "format=rgb24,colorspace=all=bt709:iall=bt709:fast=1,"
             "setparams=colorspace=bt709:color_primaries=bt709:color_trc=iec61966-2-1,"
             "split[a][b];"
             "[a]palettegen=stats_mode=full[p];"
             "[b][p]paletteuse=dither=bayer:bayer_scale=3",
             gifFps, gifWidth, outH);

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

            // PTS in centiseconds (time_base = 1/100)
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
                int totalSegs = static_cast<int>(m_settings.segments.size());
                float base = static_cast<float>(m_progress.currentSegment - 1) / totalSegs;
                m_progress.fraction = base + std::max(0.0f, std::min(segProgress, 1.0f)) / totalSegs;
            }

            ret = av_buffersrc_add_frame(bufSrcCtx, decFrame);
            av_frame_unref(decFrame);
            if (ret < 0) {
                m_progress.SetError("GIF: Failed to feed filter: " + ff::ErrorString(ret));
                goto gif_cleanup;
            }

            if (!encodeFilteredFrames()) goto gif_cleanup;
        }
    }

    // Flush filter graph
    av_buffersrc_add_frame(bufSrcCtx, nullptr);
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
