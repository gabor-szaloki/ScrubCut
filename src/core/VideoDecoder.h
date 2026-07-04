#pragma once

#include "util/FFmpegUtils.h"

#include <functional>

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // `quiet` suppresses the success log line — used by secondary decoders
    // (frame cache, export) whose codec/dimensions the main open already logged.
    bool Open(AVCodecParameters* codecParams, bool quiet = false);
    void Close();

    // Send a packet to the decoder. Pass nullptr to flush.
    int SendPacket(AVPacket* pkt);

    // Receive a decoded frame. Returns 0 on success, AVERROR(EAGAIN) if needs more input,
    // AVERROR_EOF if flushed.
    int ReceiveFrame(AVFrame* frame);

    // Flush decoder state (call after seeking).
    void Flush();

    // Drain frames buffered inside the decoder pipeline at the end of a
    // stream. Send a NULL packet to signal end-of-input, then pull every
    // frame the decoder has held back, calling `onFrame(tmp)` once per
    // frame. The callback returns true to keep going, false to stop
    // early — when stopping, `tmp` retains the last frame's data so the
    // caller can take ownership (e.g. via av_frame_move_ref).
    //
    // WHY THIS EXISTS: VideoDecoder runs the codec with FF_THREAD_FRAME,
    // which decodes several frames in parallel and emits them in display
    // order. As a side effect, the last ~thread_count frames are still
    // in the threading pipeline when the demuxer reports EOF — they
    // haven't been returned by ReceiveFrame yet. A naive read loop
    // ("ReadPacket / SendPacket / drain ReceiveFrame; on EOF break")
    // therefore loses those trailing frames. They emerge only after we
    // explicitly send a NULL packet to flush the pipeline.
    //
    // Concretely, at the end of a video this manifests as: scrubbing to
    // the very end lands a frame too early, frame-stepping past the
    // last GOP can't reach the final frames, the cache is missing the
    // tail entries so backward-step jumps over them, and playback stops
    // ~thread_count frames short of the actual end. Calling DrainAtEOF
    // after any read-to-EOF loop fixes all of those at once.
    //
    // After this returns the decoder is in "drained" state; call
    // Flush() before sending more packets to it.
    void DrainAtEOF(AVFrame* tmp, const std::function<bool(AVFrame*)>& onFrame);

    int GetWidth() const;
    int GetHeight() const;
    AVPixelFormat GetPixelFormat() const;
private:
    AVCodecContext* m_codecCtx = nullptr;
};
