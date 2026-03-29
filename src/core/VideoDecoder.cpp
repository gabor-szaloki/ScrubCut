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
    return avcodec_send_packet(m_codecCtx, pkt);
}

int VideoDecoder::ReceiveFrame(AVFrame* frame) {
    return avcodec_receive_frame(m_codecCtx, frame);
}

void VideoDecoder::Flush() {
    if (m_codecCtx)
        avcodec_flush_buffers(m_codecCtx);
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
