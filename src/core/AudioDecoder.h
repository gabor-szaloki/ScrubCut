#pragma once

#include "util/FFmpegUtils.h"

class AudioDecoder {
public:
    AudioDecoder() = default;
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool Open(AVCodecParameters* codecParams);
    void Close();

    int SendPacket(AVPacket* pkt);
    int ReceiveFrame(AVFrame* frame);
    void Flush();

    int GetSampleRate() const;
    int GetChannels() const;

private:
    AVCodecContext* m_codecCtx = nullptr;
};
