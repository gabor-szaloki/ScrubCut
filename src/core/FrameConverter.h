#pragma once

#include "core/ConvertedFrame.h"
#include "util/FFmpegUtils.h"
#include "util/Types.h"
#include <cstdint>

class FrameConverter {
public:
    FrameConverter() = default;
    ~FrameConverter();

    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    // Convert an AVFrame to a 4-byte-per-pixel buffer. Returns a pointer to the
    // internal buffer, valid until the next Convert() or destruction.
    //
    // For SDR content the output is 8-bit RGBA in sRGB, ready to display as-is.
    // For HDR content (PQ/HLG transfer) the output is 10-bit-per-channel packed
    // wide-gamut R'G'B' (AV_PIX_FMT_X2BGR10LE — still 4 bytes/pixel, so the frame
    // cache and texture upload stay the same size); the transfer function and the
    // source primaries (BT.2020 or P3) are left intact for the GPU tone-mapper to
    // decode at draw time.
    const uint8_t* Convert(AVFrame* frame);

    // Like Convert, but writes into a caller-owned ConvertedFrame (resized,
    // width/height filled in; pts is the caller's business). Used by the
    // playback pipeline to convert straight into pooled buffers.
    bool ConvertInto(AVFrame* frame, ConvertedFrame& dst);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    // Color mode of the most recently converted frame (constant per stream).
    VideoColorMode GetColorMode() const { return m_colorMode; }

    // Map an FFmpeg transfer characteristic to a VideoColorMode. Shared so the
    // Player can report a file's color mode before the first frame is converted.
    static VideoColorMode ColorModeForTransfer(int colorTrc);

    // Map FFmpeg color primaries to the gamut the tone-map shader converts from.
    // Unspecified/unrecognized defaults to BT.2020 (the HDR norm), so HDR files
    // that omit the tag keep working exactly as before.
    static VideoColorPrimaries PrimariesForTag(int colorPrimaries);

private:
    void EnsureContext(AVFrame* frame);
    int OutputSize() const;                       // bytes for the current context
    void ScaleInto(AVFrame* frame, uint8_t* dst); // shared sws_scale core
    void FreeBuffer();

    SwsContext* m_swsCtx = nullptr;
    uint8_t* m_buffer = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_bufferSize = 0;
    AVPixelFormat m_srcFmt = AV_PIX_FMT_NONE;
    AVPixelFormat m_dstFmt = AV_PIX_FMT_RGBA;
    int m_srcTrc = -1;  // AVColorTransferCharacteristic of the cached context
    VideoColorMode m_colorMode = VideoColorMode::SDR;
};
