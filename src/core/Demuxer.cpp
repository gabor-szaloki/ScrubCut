#include "core/Demuxer.h"
#include "util/Log.h"

Demuxer::~Demuxer() {
    Close();
}

bool Demuxer::Open(const std::string& path) {
    Close();

    int ret = avformat_open_input(&m_fmtCtx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        LOG_ERROR("avformat_open_input failed: %s", ff::ErrorString(ret).c_str());
        return false;
    }

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        LOG_ERROR("avformat_find_stream_info failed: %s", ff::ErrorString(ret).c_str());
        Close();
        return false;
    }

    // Find best video and audio streams
    m_videoStreamIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    m_audioStreamIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (m_videoStreamIdx < 0) {
        LOG_ERROR("No video stream found in %s", path.c_str());
        Close();
        return false;
    }

    LOG_INFO("Opened: %s", path.c_str());
    LOG_INFO("  Video stream: %d, Audio stream: %d", m_videoStreamIdx, m_audioStreamIdx);
    LOG_INFO("  Duration: %.2f seconds", GetDuration());
    LOG_INFO("  Frame rate: %.2f fps", GetVideoFrameRate());

    return true;
}

void Demuxer::Close() {
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_videoStreamIdx = -1;
    m_audioStreamIdx = -1;
}

int Demuxer::ReadPacket(AVPacket* pkt) {
    return av_read_frame(m_fmtCtx, pkt);
}

bool Demuxer::Seek(double seconds) {
    if (!m_fmtCtx || m_videoStreamIdx < 0)
        return false;

    AVRational tb = GetVideoTimeBase();
    int64_t ts = static_cast<int64_t>(seconds / av_q2d(tb));

    int ret = av_seek_frame(m_fmtCtx, m_videoStreamIdx, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        LOG_WARN("Seek failed: %s", ff::ErrorString(ret).c_str());
        return false;
    }
    return true;
}

AVCodecParameters* Demuxer::GetVideoCodecParams() const {
    if (m_fmtCtx && m_videoStreamIdx >= 0)
        return m_fmtCtx->streams[m_videoStreamIdx]->codecpar;
    return nullptr;
}

AVCodecParameters* Demuxer::GetAudioCodecParams() const {
    if (m_fmtCtx && m_audioStreamIdx >= 0)
        return m_fmtCtx->streams[m_audioStreamIdx]->codecpar;
    return nullptr;
}

double Demuxer::GetDuration() const {
    if (!m_fmtCtx)
        return 0.0;
    if (m_fmtCtx->duration != AV_NOPTS_VALUE)
        return static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE;
    return 0.0;
}

AVRational Demuxer::GetVideoTimeBase() const {
    if (m_fmtCtx && m_videoStreamIdx >= 0)
        return m_fmtCtx->streams[m_videoStreamIdx]->time_base;
    return {1, 1};
}

double Demuxer::GetVideoFrameRate() const {
    if (!m_fmtCtx || m_videoStreamIdx < 0)
        return 0.0;
    AVRational fr = av_guess_frame_rate(m_fmtCtx, m_fmtCtx->streams[m_videoStreamIdx], nullptr);
    if (fr.num == 0)
        return 0.0;
    return av_q2d(fr);
}
