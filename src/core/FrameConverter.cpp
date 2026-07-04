#include "core/FrameConverter.h"
#include "util/Log.h"

VideoColorMode FrameConverter::ColorModeForTransfer(int colorTrc) {
    switch (colorTrc) {
        case AVCOL_TRC_SMPTE2084:    return VideoColorMode::HDR_PQ;   // HDR10 / PQ
        case AVCOL_TRC_ARIB_STD_B67: return VideoColorMode::HDR_HLG;  // HLG
        default:                     return VideoColorMode::SDR;
    }
}

VideoColorPrimaries FrameConverter::PrimariesForTag(int colorPrimaries) {
    switch (colorPrimaries) {
        case AVCOL_PRI_BT709:    return VideoColorPrimaries::BT709;
        case AVCOL_PRI_SMPTE431: // DCI-P3 (approximated as P3-D65 below)
        case AVCOL_PRI_SMPTE432: return VideoColorPrimaries::DisplayP3; // P3-D65
        case AVCOL_PRI_BT2020:
        default:                 return VideoColorPrimaries::BT2020;
    }
}

FrameConverter::~FrameConverter() {
    if (m_swsCtx)
        sws_freeContext(m_swsCtx);
    FreeBuffer();
}

const uint8_t* FrameConverter::Convert(AVFrame* frame) {
    if (!frame)
        return nullptr;

    EnsureContext(frame);

    if (!m_swsCtx)
        return nullptr;

    // Both the SDR (RGBA) and HDR (X2BGR10LE) destination formats are 4 bytes
    // per pixel, so the destination stride is the same either way.
    uint8_t* dstData[1] = { m_buffer };
    int dstLinesize[1] = { m_width * 4 };

    sws_scale(m_swsCtx,
              frame->data, frame->linesize,
              0, frame->height,
              dstData, dstLinesize);

    return m_buffer;
}

void FrameConverter::EnsureContext(AVFrame* frame) {
    const int width = frame->width;
    const int height = frame->height;
    const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frame->format);
    const int trc = frame->color_trc;

    if (m_swsCtx && m_width == width && m_height == height &&
        m_srcFmt == srcFmt && m_srcTrc == trc)
        return;

    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }

    m_width = width;
    m_height = height;
    m_srcFmt = srcFmt;
    m_srcTrc = trc;
    m_colorMode = ColorModeForTransfer(trc);

    // Normalize deprecated JPEG-range formats to their standard equivalents.
    // The actual range is communicated via sws_setColorspaceDetails below.
    bool fullRange = false;
    AVPixelFormat normalizedFmt = srcFmt;
    switch (srcFmt) {
        case AV_PIX_FMT_YUVJ420P: normalizedFmt = AV_PIX_FMT_YUV420P; fullRange = true; break;
        case AV_PIX_FMT_YUVJ422P: normalizedFmt = AV_PIX_FMT_YUV422P; fullRange = true; break;
        case AV_PIX_FMT_YUVJ444P: normalizedFmt = AV_PIX_FMT_YUV444P; fullRange = true; break;
        default: break;
    }

    // HDR (PQ/HLG) is kept in 10-bit BT.2020 and tone-mapped on the GPU. SDR
    // collapses straight to 8-bit sRGB as before. X2BGR10LE packs as
    // (msb)2X 10B 10G 10R(lsb), which matches SDL_GPU_TEXTUREFORMAT_
    // R10G10B10A2_UNORM (R in the low 10 bits) on little-endian.
    const bool hdr = (m_colorMode != VideoColorMode::SDR);
    m_dstFmt = hdr ? AV_PIX_FMT_X2BGR10LE : AV_PIX_FMT_RGBA;

    m_swsCtx = sws_getContext(
        width, height, normalizedFmt,
        width, height, m_dstFmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!m_swsCtx) {
        LOG_ERROR("sws_getContext failed");
        return;
    }

    if (hdr) {
        // Apply the BT.2020 YUV->RGB matrix coefficients (the standard for HDR
        // content) with the correct input range so the 10-bit R'G'B' is
        // reconstructed faithfully. The transfer function (PQ/HLG) and the source
        // primaries (BT.2020 or P3) are deliberately preserved — the GPU
        // tone-mapper decodes them. dstRange is full (RGB is always full range).
        int srcRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
        const int* inv_table = sws_getCoefficients(SWS_CS_BT2020);
        const int* table = sws_getCoefficients(SWS_CS_BT2020);
        sws_setColorspaceDetails(m_swsCtx, inv_table, srcRange, table, 1,
                                 0, 1 << 16, 1 << 16);
    } else if (fullRange) {
        // SDR JPEG-range input: keep the default (BT.601/709) coefficients but
        // flag the input as full range so the conversion matrix is correct.
        int srcRange = 1;
        int dstRange = 1;
        const int* inv_table = sws_getCoefficients(SWS_CS_DEFAULT);
        const int* table = sws_getCoefficients(SWS_CS_DEFAULT);
        sws_setColorspaceDetails(m_swsCtx, inv_table, srcRange, table, dstRange, 0, 1 << 16, 1 << 16);
    }

    int needed = av_image_get_buffer_size(m_dstFmt, width, height, 1);
    if (needed > m_bufferSize) {
        FreeBuffer();
        m_bufferSize = needed;
        m_buffer = static_cast<uint8_t*>(av_malloc(m_bufferSize));
    }
}

void FrameConverter::FreeBuffer() {
    if (m_buffer) {
        av_free(m_buffer);
        m_buffer = nullptr;
        m_bufferSize = 0;
    }
}
