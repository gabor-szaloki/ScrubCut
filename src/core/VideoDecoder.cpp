#include "core/VideoDecoder.h"
#include "util/Log.h"

extern "C" {
#include <libavutil/intreadwrite.h>
}

VideoDecoder::~VideoDecoder() {
    Close();
}

bool VideoDecoder::Open(AVCodecParameters* codecParams, bool quiet) {
    Close();

    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        LOG_ERROR("Unsupported video codec: %d", codecParams->codec_id);
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("avcodec_alloc_context3 failed");
        return false;
    }

    int ret = avcodec_parameters_to_context(m_codecCtx, codecParams);
    if (ret < 0) {
        LOG_ERROR("avcodec_parameters_to_context failed: %s", ff::ErrorString(ret).c_str());
        Close();
        return false;
    }

    // Enable multi-threaded decoding
    m_codecCtx->thread_count = 0;  // auto-detect (typically number of CPU cores)
    m_codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    // Container-level cropping (Matroska PixelCrop* elements — e.g. WebM VP9
    // coded 1920x1088 with 8 rows cropped off the bottom for a 1920x1080
    // display size). FFmpeg exports it as stream side data instead of
    // applying it; ApplyContainerCrop applies it to every decoded frame.
    m_cropTop = m_cropBottom = m_cropLeft = m_cropRight = 0;
    if (const AVPacketSideData* sd = av_packet_side_data_get(
            codecParams->coded_side_data, codecParams->nb_coded_side_data,
            AV_PKT_DATA_FRAME_CROPPING)) {
        if (sd->size >= 16) {
            unsigned top    = AV_RL32(sd->data + 0);
            unsigned bottom = AV_RL32(sd->data + 4);
            unsigned left   = AV_RL32(sd->data + 8);
            unsigned right  = AV_RL32(sd->data + 12);
            if (static_cast<int>(top + bottom) < codecParams->height &&
                static_cast<int>(left + right) < codecParams->width) {
                m_cropTop = top;
                m_cropBottom = bottom;
                m_cropLeft = left;
                m_cropRight = right;
            }
        }
    }

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        LOG_ERROR("avcodec_open2 failed: %s", ff::ErrorString(ret).c_str());
        Close();
        return false;
    }

    if (!quiet)
        LOG_INFO("Video decoder opened: %s, %dx%d", codec->name, GetWidth(), GetHeight());
    return true;
}

void VideoDecoder::Close() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
}

int VideoDecoder::SendPacket(AVPacket* pkt) {
    if (!m_codecCtx) return AVERROR(EINVAL);
    return avcodec_send_packet(m_codecCtx, pkt);
}

int VideoDecoder::ReceiveFrame(AVFrame* frame) {
    if (!m_codecCtx) return AVERROR(EINVAL);
    int ret = avcodec_receive_frame(m_codecCtx, frame);
    if (ret == 0)
        ApplyContainerCrop(frame);
    return ret;
}

void VideoDecoder::ApplyContainerCrop(AVFrame* frame) {
    if (!(m_cropTop | m_cropBottom | m_cropLeft | m_cropRight)) return;
    frame->crop_top    += m_cropTop;
    frame->crop_bottom += m_cropBottom;
    frame->crop_left   += m_cropLeft;
    frame->crop_right  += m_cropRight;
    av_frame_apply_cropping(frame, 0);
}

void VideoDecoder::Flush() {
    if (m_codecCtx)
        avcodec_flush_buffers(m_codecCtx);
}

void VideoDecoder::DrainAtEOF(AVFrame* tmp, const std::function<bool(AVFrame*)>& onFrame) {
    if (!m_codecCtx) return;
    avcodec_send_packet(m_codecCtx, nullptr);
    while (true) {
        // avcodec_receive_frame unrefs `tmp` before refilling, so the
        // helper doesn't need to. If onFrame returns false, the caller
        // may want to take ownership of `tmp` — leave it intact.
        int ret = avcodec_receive_frame(m_codecCtx, tmp);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        ApplyContainerCrop(tmp);
        if (!onFrame(tmp)) break;
    }
}

int VideoDecoder::GetWidth() const {
    return m_codecCtx ? m_codecCtx->width - static_cast<int>(m_cropLeft + m_cropRight) : 0;
}

int VideoDecoder::GetHeight() const {
    return m_codecCtx ? m_codecCtx->height - static_cast<int>(m_cropTop + m_cropBottom) : 0;
}

AVPixelFormat VideoDecoder::GetPixelFormat() const {
    return m_codecCtx ? m_codecCtx->pix_fmt : AV_PIX_FMT_NONE;
}
