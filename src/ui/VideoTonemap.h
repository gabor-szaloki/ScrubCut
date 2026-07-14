#pragma once

#include "util/Types.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

// GPU HDR->SDR tone-mapper.
//
// HDR frames reach us as 10-bit BT.2020 R'G'B' still carrying their PQ/HLG
// transfer function (see FrameConverter). This renders that source texture
// through a fragment shader that decodes the transfer, converts BT.2020 -> 709,
// tone-maps to SDR with the selected operator, and sRGB-encodes the result into
// an 8-bit texture that the rest of the UI composites like an ordinary SDR frame.
class VideoTonemap {
public:
    ~VideoTonemap();

    // Create the pipeline from the embedded shader blobs (picking the format
    // the device accepts). Returns false (and leaves the tone-mapper inert) on
    // failure.
    bool Init(SDL_GPUDevice* device);
    void Shutdown();

    bool IsReady() const { return m_ready; }

    // Tone-map an HDR source texture (`srcTex`, `width`x`height`, color `mode`)
    // into an internally-owned texture, returning that texture. With
    // `hdrPassthrough` false: SDR tone-mapping with the selected operator into
    // an RGBA8 texture (extended-sRGB values clamped to [0,1]). With it true
    // (HDR output active): linear light with a soft rolloff at `headroom`
    // (display peak in SDR-white units), extended-sRGB encoded unclamped into
    // an FP16 texture; the operator is ignored, and dividing by `sdrWhite`
    // (OS SDR white in scRGB units) pre-cancels the composite pass's
    // paper-white scaling so content nits stay display-absolute.
    // Returns nullptr if not ready or on error. The returned handle stays valid
    // until the next Process() with a different size or output format,
    // Shutdown(), or destruction; the render is submitted on its own command
    // buffer, so in-order submission makes the result visible to the frame's
    // later UI submission.
    SDL_GPUTexture* Process(SDL_GPUTexture* srcTex, int width, int height, VideoColorMode mode,
                            VideoColorPrimaries primaries, Tonemapper tonemapper,
                            bool hdrPassthrough = false, float headroom = 1.0f,
                            float sdrWhite = 1.0f);

    // Tone-map raw HDR pixel data (10-bit packed AV_PIX_FMT_X2BGR10LE, as
    // produced by FrameConverter for HDR) into a tightly-packed 8-bit RGBA
    // buffer, top row first. Uploads the data itself, so no source texture is
    // needed — used by the export path. `outRGBA` is resized to width*height*4.
    // Returns false if not ready or on error. Blocks until the GPU finishes.
    // Same math as Process(), so exports match the display.
    bool RenderToBuffer(const uint8_t* src, int width, int height, VideoColorMode mode,
                        VideoColorPrimaries primaries, Tonemapper tonemapper,
                        std::vector<uint8_t>& outRGBA);

private:
    bool EnsureTarget(int width, int height, SDL_GPUTextureFormat format);
    // Record the fullscreen tone-map pass rendering `srcTex` into m_outTex.
    void RecordRenderPass(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* srcTex,
                          VideoColorMode mode, VideoColorPrimaries primaries,
                          Tonemapper tonemapper, bool hdrPassthrough, float headroom,
                          float sdrWhite);

    bool m_ready = false;
    SDL_GPUDevice* m_device = nullptr;
    // Pipelines bake their color-target format, so SDR (RGBA8) and HDR
    // passthrough (FP16) output each need their own.
    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;      // RGBA8 target
    SDL_GPUGraphicsPipeline* m_pipelineFP16 = nullptr;  // FP16 target
    SDL_GPUSampler* m_sampler = nullptr;
    SDL_GPUTexture* m_outTex = nullptr;
    SDL_GPUTextureFormat m_outFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    int m_outW = 0;
    int m_outH = 0;
    SDL_GPUTexture* m_inTex = nullptr;  // source upload texture for RenderToBuffer
    int m_inW = 0;
    int m_inH = 0;
    SDL_GPUTransferBuffer* m_uploadTB = nullptr;    // sized with m_inTex
    SDL_GPUTransferBuffer* m_downloadTB = nullptr;  // sized with m_outTex
    int m_downloadW = 0;
    int m_downloadH = 0;
};
