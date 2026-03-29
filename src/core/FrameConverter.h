#pragma once

#include "util/FFmpegUtils.h"
#include <cstdint>

class FrameConverter {
public:
    FrameConverter() = default;
    ~FrameConverter();

    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    // Convert an AVFrame to RGBA. Returns pointer to internal RGBA buffer.
    // The pointer is valid until the next call to Convert() or destruction.
    const uint8_t* Convert(AVFrame* frame);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    void EnsureContext(int width, int height, AVPixelFormat srcFmt);
    void FreeBuffer();

    SwsContext* m_swsCtx = nullptr;
    uint8_t* m_buffer = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_bufferSize = 0;
    AVPixelFormat m_srcFmt = AV_PIX_FMT_NONE;
};
