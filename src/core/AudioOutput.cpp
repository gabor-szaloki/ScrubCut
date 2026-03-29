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
    m_totalBytesWritten = 0;
    m_positionOffset = 0.0;
}

void AudioOutput::Write(const uint8_t* data, int size) {
    if (!m_stream || !data || size <= 0) return;
    SDL_PutAudioStreamData(m_stream, data, size);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_totalBytesWritten += size;
}

void AudioOutput::Flush() {
    if (m_stream) {
        SDL_ClearAudioStream(m_stream);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_totalBytesWritten = 0;
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

double AudioOutput::GetPlaybackPosition() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    int bytesPerSample = 2 * m_channels; // S16 interleaved
    if (bytesPerSample == 0 || m_sampleRate == 0) return m_positionOffset;

    // Total bytes written minus bytes still buffered in the stream = bytes actually played
    int buffered = m_stream ? SDL_GetAudioStreamAvailable(m_stream) : 0;
    int64_t played = m_totalBytesWritten - buffered;
    if (played < 0) played = 0;

    double secs = static_cast<double>(played) / (bytesPerSample * m_sampleRate);
    return m_positionOffset + secs;
}

void AudioOutput::ResetPosition(double startTime) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_positionOffset = startTime;
    m_totalBytesWritten = 0;
}
