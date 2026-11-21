#pragma once

#include <cstdint>
#include <memory>

// A decoded video frame converted to the 4-byte-per-pixel display format
// (RGBA for SDR, packed 10-bit for HDR — see FrameConverter). Ref-counted and
// shared between the frame queue, the frame cache, and the currently
// displayed frame, so handing a frame around never copies pixels. Buffers are
// recycled through Player's frame pool.
struct ConvertedFrame {
    int64_t pts = 0;
    int width = 0;
    int height = 0;
    bool keyframe = false;  // GOP boundary — drives the frame cache's window policy

    // swscale's SIMD output stores whole vectors, so it can write past the end
    // of the last row when the width isn't a multiple of 16 (32 bytes at 1080).
    // Keep slack after the pixels so that can't hit the next heap chunk.
    static constexpr size_t kSwsPadding = 128;

    // Grow the pixel storage without initializing it — sws_scale overwrites
    // every byte, and value-initializing tens of MB per frame is measurable
    // on the scrub path.
    void Resize(size_t bytes) {
        if (bytes > m_capacity) {
            m_pixels.reset(new uint8_t[bytes + kSwsPadding]);
            m_capacity = bytes;
        }
        m_size = bytes;
    }

    uint8_t* data() { return m_pixels.get(); }
    const uint8_t* data() const { return m_pixels.get(); }
    size_t size() const { return m_size; }

private:
    std::unique_ptr<uint8_t[]> m_pixels;
    size_t m_capacity = 0;
    size_t m_size = 0;
};
using ConvertedFramePtr = std::shared_ptr<ConvertedFrame>;
