#include "core/AudioDecoder.h"
#include "util/Log.h"

AudioDecoder::~AudioDecoder() {
    Close();
}

bool AudioDecoder::Open(AVCodecParameters* codecParams) {
    Close();

    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        LOG_ERROR("Unsupported audio codec: %d", codecParams->codec_id);
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    int ret = avcodec_parameters_to_context(m_codecCtx, codecParams);
    if (ret < 0) { Close(); return false; }

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) { Close(); return false; }

    LOG_INFO("Audio decoder opened: %s, %d Hz, %d ch",
             codec->name, GetSampleRate(), GetChannels());
    return true;
}

void AudioDecoder::Close() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
}

int AudioDecoder::SendPacket(AVPacket* pkt) {
    return avcodec_send_packet(m_codecCtx, pkt);
}

int AudioDecoder::ReceiveFrame(AVFrame* frame) {
    return avcodec_receive_frame(m_codecCtx, frame);
}

void AudioDecoder::Flush() {
    if (m_codecCtx)
        avcodec_flush_buffers(m_codecCtx);
}

int AudioDecoder::GetSampleRate() const {
    return m_codecCtx ? m_codecCtx->sample_rate : 0;
}

int AudioDecoder::GetChannels() const {
    return m_codecCtx ? m_codecCtx->ch_layout.nb_channels : 0;
}
