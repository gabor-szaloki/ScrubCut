#pragma once

#include "util/Types.h"

#include <SDL3/SDL_opengl.h>

// GPU HDR->SDR tone-mapper.
//
// HDR frames reach us as 10-bit BT.2020 R'G'B' still carrying their PQ/HLG
// transfer function (see FrameConverter). This renders that source texture
// through a fragment shader that decodes the transfer, converts BT.2020 -> 709,
// tone-maps to SDR with the selected operator, and sRGB-encodes the result into
// an 8-bit texture that the rest of the UI composites like an ordinary SDR frame.
//
// All OpenGL 3.x entry points it needs (shaders, FBOs, VAOs) are loaded lazily
// via SDL_GL_GetProcAddress, since the app otherwise only touches GL 1.1.
class VideoTonemap {
public:
    ~VideoTonemap();

    // Compile the shader and allocate GL objects. Requires a current GL
    // context. Returns false (and leaves the tone-mapper inert) on failure.
    bool Init();
    void Shutdown();

    bool IsReady() const { return m_ready; }

    // Tone-map an HDR source texture (id `srcTex`, `width`x`height`, color
    // `mode`) into an internally-owned SDR RGBA8 texture, returning that
    // texture's id. Returns 0 if not ready or on error. The returned id stays
    // valid until the next Process() call with different dimensions, Shutdown(),
    // or destruction. GL state touched (bound framebuffer, viewport) is restored.
    GLuint Process(GLuint srcTex, int width, int height, VideoColorMode mode,
                   VideoColorPrimaries primaries, Tonemapper tonemapper);

private:
    bool LoadProcs();
    bool EnsureTarget(int width, int height);

    bool m_ready = false;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_fbo = 0;
    GLuint m_outTex = 0;
    int m_outW = 0;
    int m_outH = 0;
    GLint m_uTex = -1;
    GLint m_uTransfer = -1;
    GLint m_uPrimaries = -1;
    GLint m_uTonemapper = -1;
    GLint m_uExposure = -1;

    // GL 3.x entry points (not exported by opengl32.dll on Windows).
    PFNGLCREATESHADERPROC m_glCreateShader = nullptr;
    PFNGLSHADERSOURCEPROC m_glShaderSource = nullptr;
    PFNGLCOMPILESHADERPROC m_glCompileShader = nullptr;
    PFNGLGETSHADERIVPROC m_glGetShaderiv = nullptr;
    PFNGLGETSHADERINFOLOGPROC m_glGetShaderInfoLog = nullptr;
    PFNGLCREATEPROGRAMPROC m_glCreateProgram = nullptr;
    PFNGLATTACHSHADERPROC m_glAttachShader = nullptr;
    PFNGLLINKPROGRAMPROC m_glLinkProgram = nullptr;
    PFNGLGETPROGRAMIVPROC m_glGetProgramiv = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC m_glGetProgramInfoLog = nullptr;
    PFNGLDELETESHADERPROC m_glDeleteShader = nullptr;
    PFNGLDELETEPROGRAMPROC m_glDeleteProgram = nullptr;
    PFNGLUSEPROGRAMPROC m_glUseProgram = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC m_glGetUniformLocation = nullptr;
    PFNGLUNIFORM1IPROC m_glUniform1i = nullptr;
    PFNGLUNIFORM1FPROC m_glUniform1f = nullptr;
    PFNGLGENVERTEXARRAYSPROC m_glGenVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC m_glBindVertexArray = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC m_glDeleteVertexArrays = nullptr;
    PFNGLGENFRAMEBUFFERSPROC m_glGenFramebuffers = nullptr;
    PFNGLBINDFRAMEBUFFERPROC m_glBindFramebuffer = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC m_glFramebufferTexture2D = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC m_glCheckFramebufferStatus = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC m_glDeleteFramebuffers = nullptr;
    PFNGLACTIVETEXTUREPROC m_glActiveTexture = nullptr;
};
