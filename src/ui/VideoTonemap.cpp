#include "ui/VideoTonemap.h"
#include "scrubcut_shaders.h"  // generated from src/ui/shaders/*.glsl (see EmbedShaders.cmake)
#include "util/Log.h"

#include <SDL3/SDL.h>

using tonemap_shader::kFragmentSrc;
using tonemap_shader::kVertexSrc;

VideoTonemap::~VideoTonemap() {
    Shutdown();
}

bool VideoTonemap::LoadProcs() {
    bool ok = true;
    auto load = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(SDL_GL_GetProcAddress(name));
        if (!fn) {
            LOG_ERROR("VideoTonemap: missing GL entry point %s", name);
            ok = false;
        }
    };
    load(m_glCreateShader, "glCreateShader");
    load(m_glShaderSource, "glShaderSource");
    load(m_glCompileShader, "glCompileShader");
    load(m_glGetShaderiv, "glGetShaderiv");
    load(m_glGetShaderInfoLog, "glGetShaderInfoLog");
    load(m_glCreateProgram, "glCreateProgram");
    load(m_glAttachShader, "glAttachShader");
    load(m_glLinkProgram, "glLinkProgram");
    load(m_glGetProgramiv, "glGetProgramiv");
    load(m_glGetProgramInfoLog, "glGetProgramInfoLog");
    load(m_glDeleteShader, "glDeleteShader");
    load(m_glDeleteProgram, "glDeleteProgram");
    load(m_glUseProgram, "glUseProgram");
    load(m_glGetUniformLocation, "glGetUniformLocation");
    load(m_glUniform1i, "glUniform1i");
    load(m_glUniform1f, "glUniform1f");
    load(m_glGenVertexArrays, "glGenVertexArrays");
    load(m_glBindVertexArray, "glBindVertexArray");
    load(m_glDeleteVertexArrays, "glDeleteVertexArrays");
    load(m_glGenFramebuffers, "glGenFramebuffers");
    load(m_glBindFramebuffer, "glBindFramebuffer");
    load(m_glFramebufferTexture2D, "glFramebufferTexture2D");
    load(m_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    load(m_glDeleteFramebuffers, "glDeleteFramebuffers");
    load(m_glActiveTexture, "glActiveTexture");
    return ok;
}

static GLuint CompileShader(PFNGLCREATESHADERPROC create,
                            PFNGLSHADERSOURCEPROC source, PFNGLCOMPILESHADERPROC compile,
                            PFNGLGETSHADERIVPROC getiv, PFNGLGETSHADERINFOLOGPROC getlog,
                            GLenum type, const char* src) {
    GLuint sh = create(type);
    source(sh, 1, &src, nullptr);
    compile(sh);
    GLint ok = GL_FALSE;
    getiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        getlog(sh, sizeof(log), nullptr, log);
        LOG_ERROR("VideoTonemap: shader compile failed: %s", log);
        return 0;
    }
    return sh;
}

bool VideoTonemap::Init() {
    if (m_ready) return true;
    if (!LoadProcs()) {
        Shutdown();
        return false;
    }

    GLuint vs = CompileShader(m_glCreateShader, m_glShaderSource, m_glCompileShader,
                              m_glGetShaderiv, m_glGetShaderInfoLog, GL_VERTEX_SHADER, kVertexSrc);
    GLuint fs = CompileShader(m_glCreateShader, m_glShaderSource, m_glCompileShader,
                              m_glGetShaderiv, m_glGetShaderInfoLog, GL_FRAGMENT_SHADER, kFragmentSrc);
    if (!vs || !fs) {
        if (vs) m_glDeleteShader(vs);
        if (fs) m_glDeleteShader(fs);
        Shutdown();
        return false;
    }

    m_program = m_glCreateProgram();
    m_glAttachShader(m_program, vs);
    m_glAttachShader(m_program, fs);
    m_glLinkProgram(m_program);
    GLint linked = GL_FALSE;
    m_glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    m_glDeleteShader(vs);
    m_glDeleteShader(fs);
    if (!linked) {
        char log[1024] = {0};
        m_glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        LOG_ERROR("VideoTonemap: program link failed: %s", log);
        Shutdown();
        return false;
    }

    m_uTex = m_glGetUniformLocation(m_program, "uTex");
    m_uTransfer = m_glGetUniformLocation(m_program, "uTransfer");
    m_uPrimaries = m_glGetUniformLocation(m_program, "uPrimaries");
    m_uTonemapper = m_glGetUniformLocation(m_program, "uTonemapper");
    m_uExposure = m_glGetUniformLocation(m_program, "uExposure");

    m_glGenVertexArrays(1, &m_vao);
    m_glGenFramebuffers(1, &m_fbo);

    m_ready = true;
    return true;
}

bool VideoTonemap::EnsureTarget(int width, int height) {
    if (m_outTex && m_outW == width && m_outH == height)
        return true;

    if (!m_outTex)
        glGenTextures(1, &m_outTex);
    glBindTexture(GL_TEXTURE_2D, m_outTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    m_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_outTex, 0);
    GLenum status = m_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    m_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("VideoTonemap: incomplete framebuffer (0x%x)", status);
        return false;
    }

    m_outW = width;
    m_outH = height;
    return true;
}

void VideoTonemap::RenderPass(GLuint srcTex, int width, int height, VideoColorMode mode,
                              VideoColorPrimaries primaries, Tonemapper tonemapper) {
    m_glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    m_glUseProgram(m_program);
    m_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    if (m_uTex >= 0) m_glUniform1i(m_uTex, 0);
    if (m_uTransfer >= 0) m_glUniform1i(m_uTransfer, mode == VideoColorMode::HDR_HLG ? 1 : 0);
    if (m_uPrimaries >= 0) m_glUniform1i(m_uPrimaries, static_cast<int>(primaries));
    if (m_uTonemapper >= 0) m_glUniform1i(m_uTonemapper, static_cast<int>(tonemapper));
    if (m_uExposure >= 0) m_glUniform1f(m_uExposure, 1.0f);

    m_glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    m_glBindVertexArray(0);
}

GLuint VideoTonemap::Process(GLuint srcTex, int width, int height, VideoColorMode mode,
                             VideoColorPrimaries primaries, Tonemapper tonemapper) {
    if (!m_ready || width <= 0 || height <= 0)
        return 0;
    if (!EnsureTarget(width, height))
        return 0;

    // Save the state we touch so the subsequent ImGui pass is undisturbed.
    GLint prevFbo = 0;
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    RenderPass(srcTex, width, height, mode, primaries, tonemapper);

    // Restore state.
    glBindTexture(GL_TEXTURE_2D, 0);
    m_glUseProgram(0);
    m_glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);

    return m_outTex;
}

bool VideoTonemap::RenderToBuffer(const uint8_t* src, int width, int height, VideoColorMode mode,
                                  VideoColorPrimaries primaries, Tonemapper tonemapper,
                                  std::vector<uint8_t>& outRGBA) {
    if (!m_ready || !src || width <= 0 || height <= 0)
        return false;
    if (!EnsureTarget(width, height))
        return false;

    // Upload the packed 10-bit source into a GL_RGB10_A2 texture — same format
    // the display path uses for HDR frames (X2BGR10LE == RGBA + REV_2_10_10_10).
    if (!m_inTex)
        glGenTextures(1, &m_inTex);
    glBindTexture(GL_TEXTURE_2D, m_inTex);
    if (m_inW != width || m_inH != height) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_INT_2_10_10_10_REV, src);
        m_inW = width;
        m_inH = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                        GL_UNSIGNED_INT_2_10_10_10_REV, src);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    GLint prevFbo = 0;
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    RenderPass(m_inTex, width, height, mode, primaries, tonemapper);

    // Read the SDR result back while the FBO is still bound. The fullscreen
    // triangle maps image-top to the FBO's bottom row, and glReadPixels returns
    // bottom-to-top, so the first row read is the image top — no flip needed.
    outRGBA.resize(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA.data());

    glBindTexture(GL_TEXTURE_2D, 0);
    m_glUseProgram(0);
    m_glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return true;
}

void VideoTonemap::Shutdown() {
    if (m_outTex) {
        glDeleteTextures(1, &m_outTex);
        m_outTex = 0;
    }
    if (m_inTex) {
        glDeleteTextures(1, &m_inTex);
        m_inTex = 0;
    }
    m_inW = m_inH = 0;
    if (m_fbo && m_glDeleteFramebuffers) {
        m_glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_vao && m_glDeleteVertexArrays) {
        m_glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_program && m_glDeleteProgram) {
        m_glDeleteProgram(m_program);
        m_program = 0;
    }
    m_outW = m_outH = 0;
    m_ready = false;
}
