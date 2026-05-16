#include "core/WaveformExtractor.h"
#include "core/Demuxer.h"
#include "core/AudioDecoder.h"
#include "util/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

WaveformExtractor::WaveformExtractor() : m_peaks(kBuckets, 0.0f) {}

WaveformExtractor::~WaveformExtractor() {
    Stop();
}

void WaveformExtractor::Start(const std::string& path, double durationSec) {
    Stop();
    std::fill(m_peaks.begin(), m_peaks.end(), 0.0f);
    m_filledBuckets.store(0, std::memory_order_release);
    if (durationSec <= 0.0) return;

    m_stop.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_relaxed);
    m_thread = std::thread(&WaveformExtractor::Worker, this, path, durationSec);
}

void WaveformExtractor::Stop() {
    m_stop.store(true, std::memory_order_relaxed);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false, std::memory_order_relaxed);
}

void WaveformExtractor::Reset() {
    Stop();
    std::fill(m_peaks.begin(), m_peaks.end(), 0.0f);
    m_filledBuckets.store(0, std::memory_order_release);
}

void WaveformExtractor::Worker(std::string path, double durationSec) {
    Demuxer demux;
    if (!demux.Open(path)) {
        LOG_WARN("Waveform: failed to open %s", path.c_str());
        m_running.store(false, std::memory_order_relaxed);
        return;
    }
    int audioIdx = demux.GetAudioStreamIndex();
    if (audioIdx < 0) {
        m_running.store(false, std::memory_order_relaxed);
        return; // no audio track — nothing to do, filledBuckets stays 0
    }

    AudioDecoder decoder;
    if (!decoder.Open(demux.GetAudioCodecParams())) {
        LOG_WARN("Waveform: failed to open audio decoder");
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    AVCodecParameters* par = demux.GetAudioCodecParams();
    AVRational atb = demux.GetAudioTimeBase();

    // Resample to mono float for easy peak detection across channels.
    AVChannelLayout monoLayout;
    av_channel_layout_default(&monoLayout, 1);

    SwrContext* swr = nullptr;
    int ret = swr_alloc_set_opts2(&swr,
        &monoLayout, AV_SAMPLE_FMT_FLT, par->sample_rate,
        &par->ch_layout, static_cast<AVSampleFormat>(par->format), par->sample_rate,
        0, nullptr);
    if (ret < 0 || !swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        av_channel_layout_uninit(&monoLayout);
        LOG_WARN("Waveform: swr init failed");
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> monoBuf;

    auto commitPeak = [&](int bucket, float peak) {
        if (bucket < 0 || bucket >= kBuckets) return;
        m_peaks[bucket] = peak;
        // Advance filled-count to bucket+1 only if it moved forward (samples
        // arrive monotonically, but commitment can be triggered for any
        // bucket strictly past the current one — clamp via max).
        int prev = m_filledBuckets.load(std::memory_order_relaxed);
        if (bucket + 1 > prev)
            m_filledBuckets.store(bucket + 1, std::memory_order_release);
    };

    int currentBucket = -1;
    float currentPeak = 0.0f;
    double secsPerBucket = durationSec / static_cast<double>(kBuckets);

    while (!m_stop.load(std::memory_order_relaxed)) {
        int rr = demux.ReadPacket(pkt);
        if (rr == AVERROR_EOF) break;
        if (rr < 0) break;
        if (pkt->stream_index != audioIdx) {
            av_packet_unref(pkt);
            continue;
        }

        if (decoder.SendPacket(pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (decoder.ReceiveFrame(frame) == 0) {
            if (m_stop.load(std::memory_order_relaxed)) break;

            double frameStart = (frame->pts != AV_NOPTS_VALUE)
                ? frame->pts * av_q2d(atb)
                : 0.0;

            int outSamples = frame->nb_samples;
            if (static_cast<int>(monoBuf.size()) < outSamples) monoBuf.resize(outSamples);
            uint8_t* outPtrs[1] = { reinterpret_cast<uint8_t*>(monoBuf.data()) };
            int converted = swr_convert(swr, outPtrs, outSamples,
                                        const_cast<const uint8_t**>(frame->extended_data),
                                        frame->nb_samples);
            if (converted <= 0) { av_frame_unref(frame); continue; }

            double dtPerSample = 1.0 / par->sample_rate;
            for (int i = 0; i < converted; i++) {
                double t = frameStart + i * dtPerSample;
                int bucket = static_cast<int>(t / secsPerBucket);
                if (bucket < 0) bucket = 0;
                if (bucket >= kBuckets) bucket = kBuckets - 1;

                if (bucket != currentBucket) {
                    if (currentBucket >= 0)
                        commitPeak(currentBucket, currentPeak);
                    currentBucket = bucket;
                    currentPeak = 0.0f;
                }
                float a = std::fabs(monoBuf[i]);
                if (a > currentPeak) currentPeak = a;
            }
            av_frame_unref(frame);
        }
    }

    // Drain the decoder so the final samples make it through and the last
    // bucket is committed even if the file's PTS-based duration estimate is
    // slightly off.
    if (!m_stop.load(std::memory_order_relaxed)) {
        decoder.SendPacket(nullptr); // flush
        while (decoder.ReceiveFrame(frame) == 0) {
            if (m_stop.load(std::memory_order_relaxed)) break;
            double frameStart = (frame->pts != AV_NOPTS_VALUE)
                ? frame->pts * av_q2d(atb)
                : 0.0;
            int outSamples = frame->nb_samples;
            if (static_cast<int>(monoBuf.size()) < outSamples) monoBuf.resize(outSamples);
            uint8_t* outPtrs[1] = { reinterpret_cast<uint8_t*>(monoBuf.data()) };
            int converted = swr_convert(swr, outPtrs, outSamples,
                                        const_cast<const uint8_t**>(frame->extended_data),
                                        frame->nb_samples);
            if (converted <= 0) { av_frame_unref(frame); continue; }
            double dtPerSample = 1.0 / par->sample_rate;
            for (int i = 0; i < converted; i++) {
                double t = frameStart + i * dtPerSample;
                int bucket = static_cast<int>(t / secsPerBucket);
                if (bucket < 0) bucket = 0;
                if (bucket >= kBuckets) bucket = kBuckets - 1;
                if (bucket != currentBucket) {
                    if (currentBucket >= 0)
                        commitPeak(currentBucket, currentPeak);
                    currentBucket = bucket;
                    currentPeak = 0.0f;
                }
                float a = std::fabs(monoBuf[i]);
                if (a > currentPeak) currentPeak = a;
            }
            av_frame_unref(frame);
        }
    }

    if (currentBucket >= 0)
        commitPeak(currentBucket, currentPeak);

    // Fill any remaining trailing buckets to kBuckets with zeros so the
    // renderer can stop at filledBuckets without leaving a visible gap.
    if (m_filledBuckets.load(std::memory_order_relaxed) < kBuckets)
        m_filledBuckets.store(kBuckets, std::memory_order_release);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    av_channel_layout_uninit(&monoLayout);

    m_running.store(false, std::memory_order_relaxed);
}
