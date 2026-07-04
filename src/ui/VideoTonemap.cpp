#include "ui/VideoTonemap.h"
#include "scrubcut_shaders.h"  // generated from src/ui/shaders/compiled/* (see EmbedShaders.cmake)
#include "util/Log.h"

#include <SDL3/SDL.h>

#include <cstring>

// Matches cbuffer TonemapParams in tonemap.frag.hlsl (b0, space3).
struct TonemapParams {
    int32_t transfer;    // 0 = PQ, 1 = HLG
    int32_t primaries;   // VideoColorPrimaries
    int32_t tonemapper;  // Tonemapper
    float exposure;
};

VideoTonemap::~VideoTonemap() {
    Shutdown();
}

bool VideoTonemap::Init(SDL_GPUDevice* device) {
    if (m_ready) return true;
    m_device = device;

    // Pick the shader format the device accepts. A format's blob can be empty
    // when it isn't compiled on this platform (DXIL on macOS) — skip those.
    const SDL_GPUShaderFormat avail = SDL_GetGPUShaderFormats(device);
    SDL_GPUShaderFormat format;
    tonemap_shader::Blob vertBlob, fragBlob;
    const char* entrypoint;
    if ((avail & SDL_GPU_SHADERFORMAT_MSL) && tonemap_shader::kFragMsl.size) {
        format = SDL_GPU_SHADERFORMAT_MSL;
        vertBlob = tonemap_shader::kVertMsl;
        fragBlob = tonemap_shader::kFragMsl;
        entrypoint = "main0";  // SPIRV-Cross renames `main` when emitting MSL
    } else if ((avail & SDL_GPU_SHADERFORMAT_DXIL) && tonemap_shader::kFragDxil.size) {
        format = SDL_GPU_SHADERFORMAT_DXIL;
        vertBlob = tonemap_shader::kVertDxil;
        fragBlob = tonemap_shader::kFragDxil;
        entrypoint = "main";
    } else if ((avail & SDL_GPU_SHADERFORMAT_SPIRV) && tonemap_shader::kFragSpirv.size) {
        format = SDL_GPU_SHADERFORMAT_SPIRV;
        vertBlob = tonemap_shader::kVertSpirv;
        fragBlob = tonemap_shader::kFragSpirv;
        entrypoint = "main";
    } else {
        LOG_ERROR("VideoTonemap: no embedded shader for the device's formats (0x%x)", avail);
        return false;
    }

    SDL_GPUShaderCreateInfo vsInfo = {};
    vsInfo.code = vertBlob.data;
    vsInfo.code_size = vertBlob.size;
    vsInfo.entrypoint = entrypoint;
    vsInfo.format = format;
    vsInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShader* vs = SDL_CreateGPUShader(device, &vsInfo);

    SDL_GPUShaderCreateInfo fsInfo = {};
    fsInfo.code = fragBlob.data;
    fsInfo.code_size = fragBlob.size;
    fsInfo.entrypoint = entrypoint;
    fsInfo.format = format;
    fsInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsInfo.num_samplers = 1;
    fsInfo.num_uniform_buffers = 1;
    SDL_GPUShader* fs = SDL_CreateGPUShader(device, &fsInfo);

    if (!vs || !fs) {
        LOG_ERROR("VideoTonemap: shader creation failed: %s", SDL_GetError());
        if (vs) SDL_ReleaseGPUShader(device, vs);
        if (fs) SDL_ReleaseGPUShader(device, fs);
        Shutdown();
        return false;
    }

    // Fullscreen triangle: no vertex input, no cull/depth/blend, one RGBA8 target.
    SDL_GPUColorTargetDescription colorTarget = {};
    colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    SDL_GPUGraphicsPipelineCreateInfo pipeInfo = {};
    pipeInfo.vertex_shader = vs;
    pipeInfo.fragment_shader = fs;
    pipeInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeInfo.target_info.color_target_descriptions = &colorTarget;
    pipeInfo.target_info.num_color_targets = 1;
    m_pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeInfo);

    SDL_ReleaseGPUShader(device, vs);
    SDL_ReleaseGPUShader(device, fs);

    if (!m_pipeline) {
        LOG_ERROR("VideoTonemap: pipeline creation failed: %s", SDL_GetError());
        Shutdown();
        return false;
    }

    SDL_GPUSamplerCreateInfo samplerInfo = {};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_sampler = SDL_CreateGPUSampler(device, &samplerInfo);
    if (!m_sampler) {
        LOG_ERROR("VideoTonemap: sampler creation failed: %s", SDL_GetError());
        Shutdown();
        return false;
    }

    m_ready = true;
    return true;
}

bool VideoTonemap::EnsureTarget(int width, int height) {
    if (m_outTex && m_outW == width && m_outH == height)
        return true;

    if (m_outTex) {
        SDL_ReleaseGPUTexture(m_device, m_outTex);
        m_outTex = nullptr;
    }

    SDL_GPUTextureCreateInfo texInfo = {};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width = static_cast<Uint32>(width);
    texInfo.height = static_cast<Uint32>(height);
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels = 1;
    m_outTex = SDL_CreateGPUTexture(m_device, &texInfo);
    if (!m_outTex) {
        LOG_ERROR("VideoTonemap: target texture creation failed: %s", SDL_GetError());
        return false;
    }

    m_outW = width;
    m_outH = height;
    return true;
}

void VideoTonemap::RecordRenderPass(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* srcTex,
                                    VideoColorMode mode, VideoColorPrimaries primaries,
                                    Tonemapper tonemapper) {
    TonemapParams params = {};
    params.transfer = (mode == VideoColorMode::HDR_HLG) ? 1 : 0;
    params.primaries = static_cast<int32_t>(primaries);
    params.tonemapper = static_cast<int32_t>(tonemapper);
    params.exposure = 1.0f;
    SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params));

    SDL_GPUColorTargetInfo target = {};
    target.texture = m_outTex;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;  // fullscreen triangle covers everything
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, m_pipeline);
    SDL_GPUTextureSamplerBinding binding = {};
    binding.texture = srcTex;
    binding.sampler = m_sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

SDL_GPUTexture* VideoTonemap::Process(SDL_GPUTexture* srcTex, int width, int height,
                                      VideoColorMode mode, VideoColorPrimaries primaries,
                                      Tonemapper tonemapper) {
    if (!m_ready || !srcTex || width <= 0 || height <= 0)
        return nullptr;
    if (!EnsureTarget(width, height))
        return nullptr;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(m_device);
    if (!cmd) {
        LOG_ERROR("VideoTonemap: command buffer acquire failed: %s", SDL_GetError());
        return nullptr;
    }
    RecordRenderPass(cmd, srcTex, mode, primaries, tonemapper);
    SDL_SubmitGPUCommandBuffer(cmd);

    return m_outTex;
}

bool VideoTonemap::RenderToBuffer(const uint8_t* src, int width, int height, VideoColorMode mode,
                                  VideoColorPrimaries primaries, Tonemapper tonemapper,
                                  std::vector<uint8_t>& outRGBA) {
    if (!m_ready || !src || width <= 0 || height <= 0)
        return false;
    if (!EnsureTarget(width, height))
        return false;

    const Uint32 byteSize = static_cast<Uint32>(width) * height * 4;

    // Source texture + upload transfer buffer for the packed 10-bit data —
    // same format equivalence the display path uses for HDR frames
    // (X2BGR10LE == R10G10B10A2_UNORM).
    if (m_inTex && (m_inW != width || m_inH != height)) {
        SDL_ReleaseGPUTexture(m_device, m_inTex);
        m_inTex = nullptr;
        if (m_uploadTB) {
            SDL_ReleaseGPUTransferBuffer(m_device, m_uploadTB);
            m_uploadTB = nullptr;
        }
    }
    if (!m_inTex) {
        SDL_GPUTextureCreateInfo texInfo = {};
        texInfo.type = SDL_GPU_TEXTURETYPE_2D;
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
        texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texInfo.width = static_cast<Uint32>(width);
        texInfo.height = static_cast<Uint32>(height);
        texInfo.layer_count_or_depth = 1;
        texInfo.num_levels = 1;
        m_inTex = SDL_CreateGPUTexture(m_device, &texInfo);

        SDL_GPUTransferBufferCreateInfo tbInfo = {};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size = byteSize;
        m_uploadTB = SDL_CreateGPUTransferBuffer(m_device, &tbInfo);

        if (!m_inTex || !m_uploadTB) {
            LOG_ERROR("VideoTonemap: source texture/transfer buffer creation failed: %s",
                      SDL_GetError());
            return false;
        }
        m_inW = width;
        m_inH = height;
    }

    if (m_downloadTB && (m_downloadW != width || m_downloadH != height)) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_downloadTB);
        m_downloadTB = nullptr;
    }
    if (!m_downloadTB) {
        SDL_GPUTransferBufferCreateInfo tbInfo = {};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbInfo.size = byteSize;
        m_downloadTB = SDL_CreateGPUTransferBuffer(m_device, &tbInfo);
        if (!m_downloadTB) {
            LOG_ERROR("VideoTonemap: download transfer buffer creation failed: %s", SDL_GetError());
            return false;
        }
        m_downloadW = width;
        m_downloadH = height;
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_device, m_uploadTB, true);
    if (!mapped) {
        LOG_ERROR("VideoTonemap: transfer buffer map failed: %s", SDL_GetError());
        return false;
    }
    std::memcpy(mapped, src, byteSize);
    SDL_UnmapGPUTransferBuffer(m_device, m_uploadTB);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(m_device);
    if (!cmd) {
        LOG_ERROR("VideoTonemap: command buffer acquire failed: %s", SDL_GetError());
        return false;
    }

    // One command buffer: upload -> tone-map pass -> download.
    SDL_GPUCopyPass* upload = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo uploadSrc = {};
    uploadSrc.transfer_buffer = m_uploadTB;
    uploadSrc.pixels_per_row = static_cast<Uint32>(width);
    uploadSrc.rows_per_layer = static_cast<Uint32>(height);
    SDL_GPUTextureRegion uploadDst = {};
    uploadDst.texture = m_inTex;
    uploadDst.w = static_cast<Uint32>(width);
    uploadDst.h = static_cast<Uint32>(height);
    uploadDst.d = 1;
    SDL_UploadToGPUTexture(upload, &uploadSrc, &uploadDst, true);
    SDL_EndGPUCopyPass(upload);

    RecordRenderPass(cmd, m_inTex, mode, primaries, tonemapper);

    SDL_GPUCopyPass* download = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion downloadSrc = {};
    downloadSrc.texture = m_outTex;
    downloadSrc.w = static_cast<Uint32>(width);
    downloadSrc.h = static_cast<Uint32>(height);
    downloadSrc.d = 1;
    SDL_GPUTextureTransferInfo downloadDst = {};
    downloadDst.transfer_buffer = m_downloadTB;
    downloadDst.pixels_per_row = static_cast<Uint32>(width);
    downloadDst.rows_per_layer = static_cast<Uint32>(height);
    SDL_DownloadFromGPUTexture(download, &downloadSrc, &downloadDst);
    SDL_EndGPUCopyPass(download);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        LOG_ERROR("VideoTonemap: submit failed: %s", SDL_GetError());
        return false;
    }
    SDL_WaitForGPUFences(m_device, true, &fence, 1);
    SDL_ReleaseGPUFence(m_device, fence);

    // Rows come back top-first (SDL_GPU standardizes top-left texture origin).
    mapped = SDL_MapGPUTransferBuffer(m_device, m_downloadTB, false);
    if (!mapped) {
        LOG_ERROR("VideoTonemap: download map failed: %s", SDL_GetError());
        return false;
    }
    outRGBA.resize(byteSize);
    std::memcpy(outRGBA.data(), mapped, byteSize);
    SDL_UnmapGPUTransferBuffer(m_device, m_downloadTB);
    return true;
}

void VideoTonemap::Shutdown() {
    if (m_device) {
        if (m_uploadTB) SDL_ReleaseGPUTransferBuffer(m_device, m_uploadTB);
        if (m_downloadTB) SDL_ReleaseGPUTransferBuffer(m_device, m_downloadTB);
        if (m_inTex) SDL_ReleaseGPUTexture(m_device, m_inTex);
        if (m_outTex) SDL_ReleaseGPUTexture(m_device, m_outTex);
        if (m_sampler) SDL_ReleaseGPUSampler(m_device, m_sampler);
        if (m_pipeline) SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
    }
    m_uploadTB = nullptr;
    m_downloadTB = nullptr;
    m_inTex = nullptr;
    m_outTex = nullptr;
    m_sampler = nullptr;
    m_pipeline = nullptr;
    m_device = nullptr;
    m_inW = m_inH = 0;
    m_outW = m_outH = 0;
    m_downloadW = m_downloadH = 0;
    m_ready = false;
}
