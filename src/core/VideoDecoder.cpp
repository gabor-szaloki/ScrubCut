#include "core/VideoDecoder.h"
#include "util/Log.h"

VideoDecoder::~VideoDecoder() {
    Close();
}

bool VideoDecoder::Open(AVCodecParameters* codecParams) {
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

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        LOG_ERROR("avcodec_open2 failed: %s", ff::ErrorString(ret).c_str());
        Close();
        return false;
    }

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
    return avcodec_receive_frame(m_codecCtx, frame);
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
        if (!onFrame(tmp)) break;
    }
}

int VideoDecoder::GetWidth() const {
    return m_codecCtx ? m_codecCtx->width : 0;
}

int VideoDecoder::GetHeight() const {
    return m_codecCtx ? m_codecCtx->height : 0;
}

AVPixelFormat VideoDecoder::GetPixelFormat() const {
    return m_codecCtx ? m_codecCtx->pix_fmt : AV_PIX_FMT_NONE;
}
