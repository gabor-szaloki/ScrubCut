#include "core/OcrIndexer.h"
#include "core/Demuxer.h"
#include "core/OcrCache.h"
#include "core/VideoDecoder.h"
#include "util/AppPaths.h"
#include "util/FFmpegUtils.h"
#include "util/Levenshtein.h"
#include "util/Log.h"

#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>
#include <leptonica/allheaders.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace {

// Pixel-diff fingerprint: 16x16 grayscale block averages. Stored as raw bytes
// (256 of them). Cheap to compute, robust to small encoding noise, sensitive
// enough to break dedup when on-screen text changes.
constexpr int kFpDim = 16;
constexpr int kFpBytes = kFpDim * kFpDim;
// Sum-of-absolute-differences threshold below which we treat two fingerprints
// as the same frame. Each cell can drift up to ~1 in 256 — anything bigger
// implies real pixel change.
constexpr int kFpSameThreshold = kFpDim * kFpDim * 4;

// Max long-edge dimension for the buffer we hand to Tesseract. 4K screen
// recordings get OCR'd at ~720p; text remains crisp at this size and OCR
// runs ~quadratically faster than at native resolution. Real on-screen
// text rarely needs more than this — Tesseract's training data tops out
// around the same range. Natural footage (no text) also benefits because
// fine textures that fool the LSTM into hallucinating words get smoothed
// away by the downscale.
constexpr int kOcrMaxDim = 1280;

// Tesseract confidence cutoff (0..100). Real screen-recording text scores
// 80–95; hallucinations on natural imagery typically score 30–60.
// 60 is a conservative middle ground — kills the bulk of noise while
// preserving legit text even when font rendering is shaky.
constexpr float kMinWordConfidence = 60.0f;

std::array<uint8_t, kFpBytes> ComputeFingerprint(const uint8_t* rgba, int w, int h) {
    std::array<uint8_t, kFpBytes> fp{};
    if (w <= 0 || h <= 0) return fp;
    // For each of 16x16 blocks, average luminance over a sparse 4x4 grid of
    // samples inside the block. Avoids touching every pixel for ~free.
    for (int by = 0; by < kFpDim; by++) {
        int y0 = (by * h) / kFpDim;
        int y1 = ((by + 1) * h) / kFpDim;
        if (y1 <= y0) y1 = y0 + 1;
        for (int bx = 0; bx < kFpDim; bx++) {
            int x0 = (bx * w) / kFpDim;
            int x1 = ((bx + 1) * w) / kFpDim;
            if (x1 <= x0) x1 = x0 + 1;
            int sum = 0, n = 0;
            for (int sy = 0; sy < 4; sy++) {
                int y = y0 + ((y1 - y0) * sy) / 4;
                for (int sx = 0; sx < 4; sx++) {
                    int x = x0 + ((x1 - x0) * sx) / 4;
                    const uint8_t* p = rgba + (y * w + x) * 4;
                    // Simple luminance: (R + 2G + B) / 4
                    int lum = (p[0] + 2 * p[1] + p[2]) >> 2;
                    sum += lum;
                    n++;
                }
            }
            fp[by * kFpDim + bx] = static_cast<uint8_t>(sum / std::max(n, 1));
        }
    }
    return fp;
}

int FpDistance(const std::array<uint8_t, kFpBytes>& a,
               const std::array<uint8_t, kFpBytes>& b) {
    int sum = 0;
    for (int i = 0; i < kFpBytes; i++)
        sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return sum;
}

}  // namespace

OcrIndexer::OcrIndexer() = default;
OcrIndexer::~OcrIndexer() { Stop(); }

void OcrIndexer::Start(const std::string& path, double durationSec) {
    Stop();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_occurrences.clear();
    }
    m_processedSeconds.store(0, std::memory_order_release);
    m_totalSeconds.store(static_cast<int>(durationSec), std::memory_order_relaxed);
    if (durationSec <= 0.0) return;

    // Cache hit short-circuits the worker entirely. Results are populated
    // synchronously and processedSeconds is set to total so the UI shows
    // "done" immediately.
    std::vector<TextOccurrence> cached;
    if (OcrCache::Load(path, cached)) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_occurrences = std::move(cached);
        m_processedSeconds.store(static_cast<int>(durationSec), std::memory_order_release);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    m_stop.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_relaxed);
    m_thread = std::thread(&OcrIndexer::Worker, this, path, durationSec);
}

void OcrIndexer::Stop() {
    m_stop.store(true, std::memory_order_relaxed);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false, std::memory_order_relaxed);
}

void OcrIndexer::Reset() {
    Stop();
    std::lock_guard<std::mutex> lk(m_mu);
    m_occurrences.clear();
    m_processedSeconds.store(0, std::memory_order_release);
    m_totalSeconds.store(0, std::memory_order_relaxed);
}

std::vector<TextOccurrence> OcrIndexer::Snapshot() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_occurrences;
}

bool OcrIndexer::SelfTest() {
    const auto& dir = GetResourceDir();
    tesseract::TessBaseAPI api;
    int rc = api.Init(dir.string().c_str(), "eng");
    if (rc != 0) {
        LOG_WARN("OCR self-test: tesseract Init failed (rc=%d, dir=%s)",
                 rc, dir.string().c_str());
        return false;
    }
    LOG_INFO("OCR self-test: tesseract %s ready (dir=%s)",
             tesseract::TessBaseAPI::Version(),
             dir.string().c_str());
    api.End();
    return true;
}

void OcrIndexer::Worker(std::string path, double durationSec) {
    Demuxer demux;
    if (!demux.Open(path)) {
        LOG_WARN("OCR: failed to open %s", path.c_str());
        m_running.store(false, std::memory_order_relaxed);
        return;
    }
    int vidIdx = demux.GetVideoStreamIndex();
    if (vidIdx < 0) {
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    VideoDecoder decoder;
    if (!decoder.Open(demux.GetVideoCodecParams())) {
        LOG_WARN("OCR: failed to open video decoder");
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    // Dedicated downscaling sws context. Initialized lazily once we see
    // the first frame's pixel format (decoders sometimes emit a different
    // format than codec_params advertised, e.g. yuvj420p vs yuv420p).
    SwsContext* sws = nullptr;
    AVPixelFormat swsSrcFmt = AV_PIX_FMT_NONE;
    int swsSrcW = 0, swsSrcH = 0;
    int dstW = 0, dstH = 0;
    std::vector<uint8_t> rgbaBuf;

    tesseract::TessBaseAPI api;
    if (api.Init(GetResourceDir().string().c_str(), "eng") != 0) {
        LOG_WARN("OCR: tesseract init failed");
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVRational vtb = demux.GetVideoTimeBase();

    auto decodeAt = [&](double t) -> bool {
        if (!demux.Seek(t)) return false;
        decoder.Flush();
        while (!m_stop.load(std::memory_order_relaxed)) {
            int r = demux.ReadPacket(pkt);
            if (r == AVERROR_EOF) {
                // Drain anything still in the decoder pipeline.
                decoder.SendPacket(nullptr);
                while (decoder.ReceiveFrame(frame) == 0) {
                    double pts = (frame->pts == AV_NOPTS_VALUE) ? 0.0
                                 : frame->pts * av_q2d(vtb);
                    if (pts >= t) return true;
                    av_frame_unref(frame);
                }
                return false;
            }
            if (r < 0) { av_packet_unref(pkt); continue; }
            if (pkt->stream_index != vidIdx) { av_packet_unref(pkt); continue; }
            decoder.SendPacket(pkt);
            av_packet_unref(pkt);

            while (decoder.ReceiveFrame(frame) == 0) {
                double pts = (frame->pts == AV_NOPTS_VALUE) ? 0.0
                             : frame->pts * av_q2d(vtb);
                if (pts >= t) return true;
                av_frame_unref(frame);
            }
        }
        return false;
    };

    int total = static_cast<int>(durationSec);
    m_totalSeconds.store(total, std::memory_order_relaxed);

    std::array<uint8_t, kFpBytes> prevFp{};
    bool prevFpValid = false;
    bool prevHadText = false;
    std::string prevText;

    for (int s = 0; s < total; s++) {
        if (m_stop.load(std::memory_order_relaxed)) break;

        if (!decodeAt(static_cast<double>(s))) {
            m_processedSeconds.store(s + 1, std::memory_order_release);
            continue;
        }

        int srcW = frame->width;
        int srcH = frame->height;
        AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frame->format);
        if (srcW <= 0 || srcH <= 0 || srcFmt == AV_PIX_FMT_NONE) {
            av_frame_unref(frame);
            m_processedSeconds.store(s + 1, std::memory_order_release);
            continue;
        }

        // (Re)build the downscaler if source dims/format changed. SWS_AREA
        // is a box filter — better than bilinear for preserving small text
        // and suppressing the high-frequency noise that makes the LSTM
        // hallucinate words on textured backgrounds.
        if (!sws || srcW != swsSrcW || srcH != swsSrcH || srcFmt != swsSrcFmt) {
            if (sws) { sws_freeContext(sws); sws = nullptr; }
            int maxSide = std::max(srcW, srcH);
            double scale = (maxSide > kOcrMaxDim)
                ? static_cast<double>(kOcrMaxDim) / static_cast<double>(maxSide)
                : 1.0;
            dstW = std::max(1, static_cast<int>(std::lround(srcW * scale)));
            dstH = std::max(1, static_cast<int>(std::lround(srcH * scale)));
            // Normalize deprecated JPEG-range formats — sws still handles
            // them but warns; matches FrameConverter's behavior.
            AVPixelFormat normalizedFmt = srcFmt;
            switch (srcFmt) {
                case AV_PIX_FMT_YUVJ420P: normalizedFmt = AV_PIX_FMT_YUV420P; break;
                case AV_PIX_FMT_YUVJ422P: normalizedFmt = AV_PIX_FMT_YUV422P; break;
                case AV_PIX_FMT_YUVJ444P: normalizedFmt = AV_PIX_FMT_YUV444P; break;
                default: break;
            }
            sws = sws_getContext(srcW, srcH, normalizedFmt,
                                 dstW, dstH, AV_PIX_FMT_RGBA,
                                 SWS_AREA, nullptr, nullptr, nullptr);
            if (!sws) {
                LOG_WARN("OCR: sws_getContext failed (%dx%d -> %dx%d)",
                         srcW, srcH, dstW, dstH);
                av_frame_unref(frame);
                m_processedSeconds.store(s + 1, std::memory_order_release);
                continue;
            }
            swsSrcW = srcW; swsSrcH = srcH; swsSrcFmt = srcFmt;
            rgbaBuf.assign(static_cast<size_t>(dstW) * dstH * 4, 0);
            LOG_INFO("OCR: scaling %dx%d -> %dx%d for indexing",
                     srcW, srcH, dstW, dstH);
        }

        uint8_t* dstData[1] = { rgbaBuf.data() };
        int dstStride[1] = { dstW * 4 };
        sws_scale(sws, frame->data, frame->linesize, 0, srcH, dstData, dstStride);
        const uint8_t* rgba = rgbaBuf.data();
        int W = dstW, H = dstH;

        auto fp = ComputeFingerprint(rgba, W, H);
        bool sameAsPrev = prevFpValid && FpDistance(fp, prevFp) < kFpSameThreshold;

        if (sameAsPrev) {
            // Skip OCR. If previous sample had text, extend that occurrence.
            if (prevHadText) {
                std::lock_guard<std::mutex> lk(m_mu);
                if (!m_occurrences.empty())
                    m_occurrences.back().endSec = static_cast<double>(s + 1);
            }
        } else {
            // Run OCR on this frame.
            api.SetImage(rgba, W, H, 4, W * 4);
            api.Recognize(0);

            std::vector<TextSpan> words;
            tesseract::ResultIterator* iter = api.GetIterator();
            if (iter) {
                do {
                    // Drop low-confidence words first to skip the alloc.
                    float conf = iter->Confidence(tesseract::RIL_WORD);
                    if (conf < kMinWordConfidence) continue;

                    char* raw = iter->GetUTF8Text(tesseract::RIL_WORD);
                    if (!raw) continue;
                    std::string text(raw);
                    delete[] raw;
                    // Strip trailing whitespace/newlines that Tesseract appends.
                    while (!text.empty() &&
                           (text.back() == '\n' || text.back() == ' ' || text.back() == '\r'))
                        text.pop_back();
                    if (text.empty()) continue;

                    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                    iter->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
                    TextSpan w;
                    w.text = std::move(text);
                    w.bx = static_cast<float>(x1) / static_cast<float>(W);
                    w.by = static_cast<float>(y1) / static_cast<float>(H);
                    w.bw = static_cast<float>(x2 - x1) / static_cast<float>(W);
                    w.bh = static_cast<float>(y2 - y1) / static_cast<float>(H);
                    words.push_back(std::move(w));
                } while (iter->Next(tesseract::RIL_WORD));
            }

            std::string joined;
            for (const auto& w : words) {
                if (!joined.empty()) joined.push_back(' ');
                joined += w.text;
            }

            if (joined.empty()) {
                prevHadText = false;
                prevText.clear();
            } else {
                bool extend = prevHadText &&
                              lev::Similarity(joined, prevText) >= 0.8;
                std::lock_guard<std::mutex> lk(m_mu);
                if (extend && !m_occurrences.empty()) {
                    m_occurrences.back().endSec = static_cast<double>(s + 1);
                } else {
                    TextOccurrence occ;
                    occ.startSec = static_cast<double>(s);
                    occ.endSec   = static_cast<double>(s + 1);
                    occ.words    = std::move(words);
                    m_occurrences.push_back(std::move(occ));
                }
                prevText = std::move(joined);
                prevHadText = true;
            }
        }

        prevFp = fp;
        prevFpValid = true;
        av_frame_unref(frame);
        m_processedSeconds.store(s + 1, std::memory_order_release);
    }

    api.End();
    if (sws) sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    // Persist results only when the scan ran to completion. If the user
    // closed the file or app mid-scan, m_stop is set and a partial index
    // shouldn't be cached.
    if (!m_stop.load(std::memory_order_relaxed)) {
        std::vector<TextOccurrence> snapshot;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            snapshot = m_occurrences;
        }
        OcrCache::Save(path, snapshot);
        OcrCache::EvictLRU(50);
    }

    m_running.store(false, std::memory_order_relaxed);
}
