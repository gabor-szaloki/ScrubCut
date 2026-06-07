#pragma once

#include "util/FFmpegUtils.h"
#include <string>

class Demuxer {
public:
    Demuxer() = default;
    ~Demuxer();

    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    bool Open(const std::string& path);
    void Close();

    // Read the next packet. Returns 0 on success, AVERROR_EOF at end, or negative error.
    int ReadPacket(AVPacket* pkt);

    // Seek to the given timestamp (in seconds). Seeks to the keyframe at or before the target.
    bool Seek(double seconds);

    AVFormatContext* GetFormatContext() const { return m_fmtCtx; }
    int GetVideoStreamIndex() const { return m_videoStreamIdx; }
    int GetAudioStreamIndex() const { return m_audioStreamIdx; }

    // Switch which audio stream is active. The DemuxThread packet filter and
    // GetAudioCodecParams()/GetAudioTimeBase() all key off this index, so the
    // caller must park the pipeline + reopen the audio decoder around this.
    // Ignores out-of-range indices and indices that aren't audio streams.
    void SetAudioStreamIndex(int idx);

    AVCodecParameters* GetVideoCodecParams() const;
    AVCodecParameters* GetAudioCodecParams() const;

    double GetDuration() const; // in seconds
    AVRational GetVideoTimeBase() const;
    AVRational GetAudioTimeBase() const;
    double GetVideoFrameRate() const; // fps

private:
    AVFormatContext* m_fmtCtx = nullptr;
    int m_videoStreamIdx = -1;
    int m_audioStreamIdx = -1;
};
