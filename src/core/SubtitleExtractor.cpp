#include "core/SubtitleExtractor.h"
#include "util/Log.h"
#include "util/Profiler.h"

#include <algorithm>
#include <chrono>
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

// Run-length encode a w x h block of 8-bit palette indices (rows `stride`
// bytes apart) as [index][LEB128 run length] pairs, row-major, with runs
// allowed to span rows. Subtitle images are mostly long transparent or
// solid-fill runs, so this is ~5x smaller than the raw indices. Also returns
// the highest index used, so the palette can be trimmed to match.
void EncodeIndicesRle(const uint8_t* src, int stride, int w, int h,
                      std::vector<uint8_t>& out, int& maxIndex) {
    out.clear();
    maxIndex = 0;
    auto emit = [&](uint8_t value, size_t run) {
        out.push_back(value);
        do {
            uint8_t b = run & 0x7F;
            run >>= 7;
            out.push_back(run ? static_cast<uint8_t>(b | 0x80) : b);
        } while (run);
        maxIndex = std::max<int>(maxIndex, value);
    };
    size_t run = 0;
    uint8_t cur = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            if (run && row[x] == cur) { ++run; continue; }
            if (run) emit(cur, run);
            cur = row[x];
            run = 1;
        }
    }
    if (run) emit(cur, run);
    out.shrink_to_fit();
}

} // namespace

void ExpandSubtitleRect(const SubtitleBitmapRect& r, uint8_t* rgba) {
    // Palette (0xAARRGGBB) -> RGBA8 byte order; indices past the palette are
    // transparent.
    uint32_t lut[256] = {};
    for (size_t c = 0; c < r.palette.size() && c < 256; ++c) {
        uint32_t p = r.palette[c];
        uint8_t px[4] = {static_cast<uint8_t>(p >> 16), static_cast<uint8_t>(p >> 8),
                         static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 24)};
        std::memcpy(&lut[c], px, 4);
    }
    uint32_t* out = reinterpret_cast<uint32_t*>(rgba);
    const size_t total = static_cast<size_t>(r.w) * r.h;
    size_t pos = 0;
    const uint8_t* p = r.rle.data();
    const uint8_t* end = p + r.rle.size();
    while (p < end && pos < total) {
        uint32_t color = lut[*p++];
        size_t run = 0;
        for (int shift = 0; p < end && shift < 64; shift += 7) {
            uint8_t b = *p++;
            run |= static_cast<size_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
        }
        run = std::min(run, total - pos);
        std::fill(out + pos, out + pos + run, color);
        pos += run;
    }
    std::fill(out + pos, out + total, 0u);
}

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
    const SubtitleEvent* e = FindActive(t);
    return e ? e->text : std::string();
}

std::shared_ptr<const SubtitleBitmap> SubtitleExtractor::ActiveBitmap(double t) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    const SubtitleEvent* e = FindActive(t);
    return e ? e->bitmap : nullptr;
}

const SubtitleEvent* SubtitleExtractor::FindActive(double t) const {
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
            return &*it;
    }
    return nullptr;
}

void SubtitleExtractor::Worker(std::string path, int streamIndex) {
    PROFILE_THREAD("Subtitle Scan");
    PROFILE_SCOPE();
    Profiler::ScopedSection section(Profiler::kSectionJobs, "Subtitle scan");
    const auto scanStart = std::chrono::steady_clock::now();
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

    // Bitmap tracks differ from text ones in two ways: cues are images, and
    // a cue often has no real end time (PGS: end_display_time == UINT32_MAX)
    // — it lasts until the next display set, which for a plain "clear" has no
    // images at all. So for bitmap tracks every decoded subtitle, empty ones
    // included, is recorded and later used to cut the previous cue short.
    const AVCodecDescriptor* desc = avcodec_descriptor_get(st->codecpar->codec_id);
    const bool bitmapTrack = desc && (desc->props & AV_CODEC_PROP_BITMAP_SUB);
    constexpr double kOpenEndedSec = 10.0; // cap for a final cue nothing clears

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
        if (bitmapTrack) {
            if (sub.end_display_time == UINT32_MAX || end <= start)
                end = start + kOpenEndedSec; // open-ended; trimmed by the next cue below
            std::shared_ptr<SubtitleBitmap> bmp;
            for (unsigned i = 0; i < sub.num_rects; ++i) {
                const AVSubtitleRect* r = sub.rects[i];
                if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 ||
                    !r->data[0] || !r->data[1] || r->nb_colors <= 0)
                    continue;
                if (!bmp) {
                    bmp = std::make_shared<SubtitleBitmap>();
                    // PGS sets the canvas size on the decoder while parsing;
                    // VobSub/DVB may leave it 0 (the renderer then uses the
                    // video frame size).
                    bmp->canvasW = ctx->width;
                    bmp->canvasH = ctx->height;
                }
                SubtitleBitmapRect br;
                br.x = r->x; br.y = r->y; br.w = r->w; br.h = r->h;
                int maxIndex = 0;
                EncodeIndicesRle(r->data[0], r->linesize[0], r->w, r->h, br.rle, maxIndex);
                const uint32_t* pal = reinterpret_cast<const uint32_t*>(r->data[1]);
                br.palette.assign(pal, pal + std::min({r->nb_colors, maxIndex + 1, 256}));
                bmp->rects.push_back(std::move(br));
            }
            avsubtitle_free(&sub);
            // Keep empty ("clear") cues for now: they end the previous one.
            events.push_back({start, end, {}, std::move(bmp)});
            continue;
        }
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
        if (bitmapTrack) {
            // Bitmap cues replace each other on screen: each one ends no later
            // than the next one starts. Then drop the "clear" markers.
            for (size_t i = 0; i + 1 < events.size(); ++i)
                events[i].endSec = std::min(events[i].endSec, events[i + 1].startSec);
            events.erase(std::remove_if(events.begin(), events.end(),
                                        [](const SubtitleEvent& e) {
                                            return !e.bitmap || e.endSec <= e.startSec;
                                        }),
                         events.end());
        }
        double maxDur = 0.0;
        for (const auto& e : events)
            maxDur = std::max(maxDur, e.endSec - e.startSec);
        const double scanMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - scanStart).count();
        if (bitmapTrack) {
            size_t bytes = 0;
            for (const SubtitleEvent& e : events)
                for (const SubtitleBitmapRect& r : e.bitmap->rects)
                    bytes += r.rle.size() + r.palette.size() * sizeof(uint32_t);
            LOG_INFO("Subtitle: scanned %zu bitmap cues (%.1f MB encoded) in %.0f ms from %s (stream %d)",
                     events.size(), bytes / (1024.0 * 1024.0), scanMs, path.c_str(), subIdx);
        } else {
            LOG_INFO("Subtitle: scanned %zu text cues in %.0f ms from %s (stream %d)",
                     events.size(), scanMs, path.c_str(), subIdx);
        }
        std::lock_guard<std::mutex> lk(m_mutex);
        m_events = std::move(events);
        m_maxDurationSec = maxDur;
    }
    m_running.store(false, std::memory_order_relaxed);
}
