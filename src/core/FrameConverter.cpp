#include "core/FrameConverter.h"
#include "util/Log.h"

FrameConverter::~FrameConverter() {
    if (m_swsCtx)
        sws_freeContext(m_swsCtx);
    FreeBuffer();
}

const uint8_t* FrameConverter::Convert(AVFrame* frame) {
    if (!frame)
        return nullptr;

    EnsureContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format));

    if (!m_swsCtx)
        return nullptr;

    uint8_t* dstData[1] = { m_buffer };
    int dstLinesize[1] = { m_width * 4 }; // RGBA = 4 bytes per pixel

    sws_scale(m_swsCtx,
              frame->data, frame->linesize,
              0, frame->height,
              dstData, dstLinesize);

    return m_buffer;
}

void FrameConverter::EnsureContext(int width, int height, AVPixelFormat srcFmt) {
    if (m_swsCtx && m_width == width && m_height == height && m_srcFmt == srcFmt)
        return;

    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }

    m_width = width;
    m_height = height;
    m_srcFmt = srcFmt;

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

    m_swsCtx = sws_getContext(
        width, height, normalizedFmt,
        width, height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!m_swsCtx) {
        LOG_ERROR("sws_getContext failed");
        return;
    }

    // Set correct color range so sws_scale uses the right conversion matrix
    if (fullRange) {
        int srcRange = 1;  // full range
        int dstRange = 1;
        const int* inv_table = sws_getCoefficients(SWS_CS_DEFAULT);
        const int* table = sws_getCoefficients(SWS_CS_DEFAULT);
        sws_setColorspaceDetails(m_swsCtx, inv_table, srcRange, table, dstRange, 0, 1 << 16, 1 << 16);
    }

    int needed = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);
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
