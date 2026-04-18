#pragma once

#include "util/FFmpegUtils.h"

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool Open(AVCodecParameters* codecParams);
    void Close();

    // Send a packet to the decoder. Pass nullptr to flush.
    int SendPacket(AVPacket* pkt);

    // Receive a decoded frame. Returns 0 on success, AVERROR(EAGAIN) if needs more input,
    // AVERROR_EOF if flushed.
    int ReceiveFrame(AVFrame* frame);

    // Flush decoder state (call after seeking).
    void Flush();

    int GetWidth() const;
    int GetHeight() const;
    AVPixelFormat GetPixelFormat() const;
private:
    AVCodecContext* m_codecCtx = nullptr;
};
