#include "core/SubtitleExtractor.h"
#include "util/Log.h"

#include <algorithm>
#include <cstring>

namespace {

// Strip ASS/SSA override blocks ({\...}), convert \N / \n to newlines and \h
// to a hard space, and collapse the rest to plain UTF-8 text.
std::string CleanAssText(const char* in) {
    std::string out;
    if (!in) return out;
    for (const char* p = in; *p; ++p) {
        if (*p == '{') {
            const char* close = std::strchr(p, '}');
            if (close) { p = close; continue; }
        }
        if (*p == '\\' && (p[1] == 'N' || p[1] == 'n')) { out.push_back('\n'); ++p; continue; }
        if (*p == '\\' && p[1] == 'h') { out.push_back(' '); ++p; continue; }
        out.push_back(*p);
    }
    return out;
}

// The decoder hands us an ASS event line of the form
//   ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
// (8 comma-separated fields before the free-form Text). Take everything past
// the 8th comma, then clean override tags.
std::string ExtractAssDialogue(const char* ass) {
    if (!ass) return {};
    const char* p = ass;
    int commas = 0;
    for (; *p && commas < 8; ++p) {
        if (*p == ',') ++commas;
    }
    if (commas < 8) p = ass; // unexpected layout — clean the whole thing
    return CleanAssText(p);
}

// Strip simple SRT/HTML markup (<i>, <b>, <font ...>, etc.) from plain-text cues.
std::string StripTags(const char* in) {
    std::string out;
    if (!in) return out;
    bool inTag = false;
    for (const char* p = in; *p; ++p) {
        if (*p == '<') { inTag = true; continue; }
        if (*p == '>') { inTag = false; continue; }
        if (!inTag) out.push_back(*p);
    }
    return out;
}

} // namespace

SubtitleExtractor::~SubtitleExtractor() {
    Stop();
}

void SubtitleExtractor::Start(const std::string& path, int streamIndex) {
    Stop();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_events.clear();
        m_maxDurationSec = 0.0;
    }
    m_stop.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_relaxed);
    m_thread = std::thread(&SubtitleExtractor::Worker, this, path, streamIndex);
}

void SubtitleExtractor::Stop() {
    m_stop.store(true, std::memory_order_relaxed);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false, std::memory_order_relaxed);
}

void SubtitleExtractor::Reset() {
    Stop();
    std::lock_guard<std::mutex> lk(m_mutex);
    m_events.clear();
    m_maxDurationSec = 0.0;
}

bool SubtitleExtractor::HasEvents() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return !m_events.empty();
}

std::string SubtitleExtractor::ActiveText(double t) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    // m_events is sorted by startSec. Binary-search to the last cue that starts
    // at or before t, then walk back over the few cues that could still be on
    // screen. Any cue containing t must have started no earlier than
    // (t - m_maxDurationSec), so that window bounds the back-scan to O(log n)
    // plus a small constant — correct even for overlapping (e.g. ASS) cues.
    auto it = std::upper_bound(m_events.begin(), m_events.end(), t,
                               [](double tt, const SubtitleEvent& e) {
                                   return tt < e.startSec;
                               });
    double earliestStart = t - m_maxDurationSec;
    for (; it != m_events.begin();) {
        --it;
        if (it->startSec < earliestStart) break; // nothing earlier can still be active
        if (t >= it->startSec && t < it->endSec)
            return it->text;
    }
    return {};
}

void SubtitleExtractor::Worker(std::string path, int streamIndex) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        LOG_WARN("Subtitle: failed to open %s", path.c_str());
        m_running.store(false, std::memory_order_relaxed);
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    int subIdx = streamIndex;
    if (subIdx < 0)
        subIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
    if (subIdx < 0 || subIdx >= static_cast<int>(fmt->nb_streams) ||
        fmt->streams[subIdx]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        LOG_WARN("Subtitle: no subtitle stream in %s", path.c_str());
        avformat_close_input(&fmt);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    AVStream* st = fmt->streams[subIdx];
    AVRational tb = st->time_base;
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        LOG_WARN("Subtitle: no decoder for codec %d", st->codecpar->codec_id);
        avformat_close_input(&fmt);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) {
        avformat_close_input(&fmt);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->pkt_timebase = tb;
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        LOG_WARN("Subtitle: failed to open decoder");
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    std::vector<SubtitleEvent> events;
    AVPacket* pkt = av_packet_alloc();

    while (!m_stop.load(std::memory_order_relaxed)) {
        int rr = av_read_frame(fmt, pkt);
        if (rr < 0) break; // EOF or error
        if (pkt->stream_index != subIdx) { av_packet_unref(pkt); continue; }

        AVSubtitle sub;
        int got = 0;
        int ret = avcodec_decode_subtitle2(ctx, &sub, &got, pkt);
        double pktSec = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(tb) : 0.0;
        double pktDur = pkt->duration > 0 ? pkt->duration * av_q2d(tb) : 0.0;
        av_packet_unref(pkt);
        if (ret < 0 || !got) { if (got) avsubtitle_free(&sub); continue; }

        double start = pktSec + sub.start_display_time / 1000.0;
        double end   = pktSec + sub.end_display_time / 1000.0;
        if (end <= start)
            end = start + (pktDur > 0.0 ? pktDur : 5.0); // fall back to packet duration

        std::string text;
        for (unsigned i = 0; i < sub.num_rects; ++i) {
            AVSubtitleRect* r = sub.rects[i];
            std::string piece;
            if (r->type == SUBTITLE_ASS && r->ass)
                piece = ExtractAssDialogue(r->ass);
            else if (r->type == SUBTITLE_TEXT && r->text)
                piece = StripTags(r->text);
            if (!piece.empty()) {
                if (!text.empty()) text.push_back('\n');
                text += piece;
            }
        }
        avsubtitle_free(&sub);

        // Trim trailing whitespace/newlines.
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ' ||
                                 text.back() == '\r' || text.back() == '\t'))
            text.pop_back();
        if (!text.empty())
            events.push_back({start, end, std::move(text)});
    }

    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    if (!m_stop.load(std::memory_order_relaxed)) {
        std::stable_sort(events.begin(), events.end(),
                         [](const SubtitleEvent& a, const SubtitleEvent& b) {
                             return a.startSec < b.startSec;
                         });
        double maxDur = 0.0;
        for (const auto& e : events)
            maxDur = std::max(maxDur, e.endSec - e.startSec);
        LOG_INFO("Subtitle: scanned %zu cues from %s (stream %d)",
                 events.size(), path.c_str(), subIdx);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_events = std::move(events);
        m_maxDurationSec = maxDur;
    }
    m_running.store(false, std::memory_order_relaxed);
}
