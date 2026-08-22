#include "core/AudioOutput.h"
#include "util/Log.h"

AudioOutput::~AudioOutput() {
    Close();
}

bool AudioOutput::Open(int sampleRate, int channels) {
    Close();

    m_sampleRate = sampleRate;
    m_channels = channels;

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = channels;
    spec.freq = sampleRate;

    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!m_stream) {
        LOG_ERROR("SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(m_stream);
    LOG_INFO("Audio output opened: %d Hz, %d ch, S16", sampleRate, channels);
    return true;
}

void AudioOutput::Close() {
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastWrittenEndPts = 0.0;
}

void AudioOutput::Write(const uint8_t* data, int size, double ptsSec) {
    if (!m_stream || !data || size <= 0) return;
    SDL_PutAudioStreamData(m_stream, data, size);
    std::lock_guard<std::mutex> lock(m_mutex);
    int bytesPerSample = 2 * m_channels;
    double durSec = (bytesPerSample > 0 && m_sampleRate > 0)
        ? static_cast<double>(size) / (bytesPerSample * m_sampleRate) : 0.0;
    if (ptsSec == ptsSec) // not NAN — anchor to the source timestamp
        m_lastWrittenEndPts = ptsSec + durSec;
    else
        m_lastWrittenEndPts += durSec;
}

void AudioOutput::Flush() {
    if (m_stream) {
        SDL_ClearAudioStream(m_stream);
    }
}

void AudioOutput::Pause() {
    if (m_stream)
        SDL_PauseAudioStreamDevice(m_stream);
}

void AudioOutput::Resume() {
    if (m_stream)
        SDL_ResumeAudioStreamDevice(m_stream);
}

void AudioOutput::SetSpeed(float speed) {
    if (m_stream)
        SDL_SetAudioStreamFrequencyRatio(m_stream, speed);
}

void AudioOutput::SetVolume(float volume) {
    m_volume = volume;
    if (m_stream)
        SDL_SetAudioStreamGain(m_stream, volume);
}

double AudioOutput::GetPlaybackPosition() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    int bytesPerSample = 2 * m_channels; // S16 interleaved
    if (bytesPerSample == 0 || m_sampleRate == 0) return m_lastWrittenEndPts;

    // SDL_GetAudioStreamQueued is input-side: bytes put but not yet consumed
    // by the device. Input bytes map directly to media time regardless of the
    // stream's frequency ratio (playback speed).
    int64_t queued = m_stream ? SDL_GetAudioStreamQueued(m_stream) : 0;
    double queuedSec = static_cast<double>(queued) / (bytesPerSample * m_sampleRate);
    return m_lastWrittenEndPts - queuedSec;
}

bool AudioOutput::HasQueuedData() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stream && SDL_GetAudioStreamQueued(m_stream) > 0;
}

void AudioOutput::ResetPosition(double startTime) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastWrittenEndPts = startTime;
}

bool AudioOutput::DiscardUntil(double targetPts) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stream) return false;
    int bytesPerSample = 2 * m_channels;
    if (bytesPerSample == 0 || m_sampleRate == 0) return false;

    int64_t queued = SDL_GetAudioStreamQueued(m_stream);
    double pos = m_lastWrittenEndPts
               - static_cast<double>(queued) / (bytesPerSample * m_sampleRate);
    double gapSec = targetPts - pos;
    if (gapSec <= 0.0) return false;
    if (targetPts > m_lastWrittenEndPts - 0.005) return false; // beyond buffered content

    // SDL_GetAudioStreamData reads output-format bytes; with frequency ratio
    // r (playback speed) the stream turns gapSec of content into gapSec/r of
    // output time.
    SDL_AudioSpec inSpec, outSpec;
    if (!SDL_GetAudioStreamFormat(m_stream, &inSpec, &outSpec)) return false;
    float ratio = SDL_GetAudioStreamFrequencyRatio(m_stream);
    if (ratio <= 0.0f) ratio = 1.0f;
    int64_t discard = static_cast<int64_t>(gapSec / ratio * outSpec.freq)
                    * SDL_AUDIO_FRAMESIZE(outSpec);

    uint8_t scratch[4096];
    while (discard > 0) {
        int chunk = discard < static_cast<int64_t>(sizeof(scratch))
                  ? static_cast<int>(discard) : static_cast<int>(sizeof(scratch));
        int n = SDL_GetAudioStreamData(m_stream, scratch, chunk);
        if (n <= 0) break;
        discard -= n;
    }
    return true;
}
