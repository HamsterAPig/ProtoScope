#include "wave_render_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include <imgui_impl_opengl3_loader.h>

#ifdef _WIN32
extern "C" __declspec(dllimport) PROC WINAPI wglGetProcAddress(LPCSTR);
#endif

namespace protoscope::ui {
namespace {

    constexpr GLenum kGlFramebuffer = 0x8D40;
    constexpr GLenum kGlColorAttachment0 = 0x8CE0;
    constexpr GLenum kGlFramebufferComplete = 0x8CD5;
    constexpr GLenum kGlFramebufferBinding = 0x8CA6;
    constexpr GLenum kGlZero = 0;
    constexpr GLenum kGlLines = 0x0001;
    constexpr GLenum kGlTriangleStrip = 0x0005;
    constexpr GLenum kGlLineWidth = 0x0B21;
    constexpr GLenum kGlColorClearValue = 0x0C22;

    using GlGenFramebuffersProc = void(APIENTRYP)(GLsizei, GLuint*);
    using GlBindFramebufferProc = void(APIENTRYP)(GLenum, GLuint);
    using GlFramebufferTexture2DProc = void(APIENTRYP)(GLenum, GLenum, GLenum, GLuint, GLint);
    using GlCheckFramebufferStatusProc = GLenum(APIENTRYP)(GLenum);
    using GlDeleteFramebuffersProc = void(APIENTRYP)(GLsizei, const GLuint*);
    using GlDrawArraysProc = void(APIENTRYP)(GLenum, GLint, GLsizei);
    using GlGetFloatvProc = void(APIENTRYP)(GLenum, GLfloat*);
    using GlLineWidthProc = void(APIENTRYP)(GLfloat);

#ifdef _WIN32
    void* loadGlProc(const char* name)
    {
        void* proc = reinterpret_cast<void*>(wglGetProcAddress(name));
        if (proc != nullptr && proc != reinterpret_cast<void*>(1) && proc != reinterpret_cast<void*>(2) &&
            proc != reinterpret_cast<void*>(3) && proc != reinterpret_cast<void*>(-1)) {
            return proc;
        }
        static HMODULE opengl = GetModuleHandleA("opengl32.dll");
        if (opengl == nullptr) {
            opengl = LoadLibraryA("opengl32.dll");
        }
        return opengl == nullptr ? nullptr : reinterpret_cast<void*>(GetProcAddress(opengl, name));
    }
#else
    void* loadGlProc(const char*)
    {
        return nullptr;
    }
#endif

    struct FboApi {
        GlGenFramebuffersProc genFramebuffers{nullptr};
        GlBindFramebufferProc bindFramebuffer{nullptr};
        GlFramebufferTexture2DProc framebufferTexture2D{nullptr};
        GlCheckFramebufferStatusProc checkFramebufferStatus{nullptr};
        GlDeleteFramebuffersProc deleteFramebuffers{nullptr};
        GlDrawArraysProc drawArrays{nullptr};
        GlGetFloatvProc getFloatv{nullptr};
        GlLineWidthProc lineWidth{nullptr};
        bool loaded{false};
        bool available{false};
    };

    FboApi& fboApi()
    {
        static FboApi api;
        if (!api.loaded) {
            api.loaded = true;
            api.genFramebuffers = reinterpret_cast<GlGenFramebuffersProc>(loadGlProc("glGenFramebuffers"));
            api.bindFramebuffer = reinterpret_cast<GlBindFramebufferProc>(loadGlProc("glBindFramebuffer"));
            api.framebufferTexture2D =
                reinterpret_cast<GlFramebufferTexture2DProc>(loadGlProc("glFramebufferTexture2D"));
            api.checkFramebufferStatus =
                reinterpret_cast<GlCheckFramebufferStatusProc>(loadGlProc("glCheckFramebufferStatus"));
            api.deleteFramebuffers = reinterpret_cast<GlDeleteFramebuffersProc>(loadGlProc("glDeleteFramebuffers"));
            api.drawArrays = reinterpret_cast<GlDrawArraysProc>(loadGlProc("glDrawArrays"));
            api.getFloatv = reinterpret_cast<GlGetFloatvProc>(loadGlProc("glGetFloatv"));
            api.lineWidth = reinterpret_cast<GlLineWidthProc>(loadGlProc("glLineWidth"));
            api.available = api.genFramebuffers != nullptr && api.bindFramebuffer != nullptr &&
                            api.framebufferTexture2D != nullptr && api.checkFramebufferStatus != nullptr &&
                            api.deleteFramebuffers != nullptr;
        }
        return api;
    }

    struct SavedGlState {
        GLint framebuffer{0};
        GLint viewport[4]{0, 0, 0, 0};
        GLint program{0};
        GLint texture{0};
        GLint arrayBuffer{0};
        GLint vertexArray{0};
        GLint blendEquationRgb{GL_FUNC_ADD};
        GLint blendEquationAlpha{GL_FUNC_ADD};
        GLint blendSrcRgb{GL_ONE};
        GLint blendDstRgb{kGlZero};
        GLint blendSrcAlpha{GL_ONE};
        GLint blendDstAlpha{kGlZero};
        GLfloat lineWidth{1.0F};
        GLfloat clearColor[4]{0.0F, 0.0F, 0.0F, 0.0F};
        GLboolean blendEnabled{GL_FALSE};
        GLboolean scissorEnabled{GL_FALSE};
    };

    constexpr const char* kPhosphorVertexShader = R"glsl(
#version 130
in vec2 Position;
in vec4 Color;
out vec4 FragColor;

void main()
{
    FragColor = Color;
    gl_Position = vec4(Position, 0.0, 1.0);
}
)glsl";

    constexpr const char* kPhosphorFragmentShader = R"glsl(
#version 130
in vec4 FragColor;
out vec4 OutColor;

void main()
{
    OutColor = FragColor;
}
)glsl";

    double safeDuration(const ImPlotRect& limits)
    {
        const double duration = limits.X.Max - limits.X.Min;
        return std::isfinite(duration) && std::abs(duration) > 1e-12 ? duration : 1.0;
    }

    double safeValueRange(const ImPlotRect& limits)
    {
        const double range = limits.Y.Max - limits.Y.Min;
        return std::isfinite(range) && std::abs(range) > 1e-12 ? range : 1.0;
    }

    int plotPixelWidth()
    {
        return (std::max)(1, static_cast<int>(std::lround((std::max)(ImPlot::GetPlotSize().x, 1.0F))));
    }

    int plotPixelHeight()
    {
        return (std::max)(1, static_cast<int>(std::lround((std::max)(ImPlot::GetPlotSize().y, 1.0F))));
    }

    float pixelXForTime(const ImPlotRect& limits, const int width, const double time)
    {
        const double x = (time - limits.X.Min) / safeDuration(limits);
        return static_cast<float>(x * static_cast<double>((std::max)(width - 1, 1)));
    }

    float pixelYForValue(const ImPlotRect& limits, const int height, const double value)
    {
        const double y = (limits.Y.Max - value) / safeValueRange(limits);
        return static_cast<float>(y * static_cast<double>((std::max)(height - 1, 1)));
    }

    bool channelCanEnterPhosphor(const plot::WaveSnapshot& snapshot, const std::size_t channelIndex)
    {
        return channelIndex < snapshot.channels.size() && !bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay);
    }

    const std::vector<plot::WaveSample>* samplesForChannel(const plot::WaveDisplayData& displayData,
                                                           const std::size_t channelIndex)
    {
        if (channelIndex >= displayData.channels.size()) {
            return nullptr;
        }
        return &displayData.channels[channelIndex].samples;
    }

    class WavePhosphorRenderer {
    public:
        bool render(plot::WaveViewState& view,
                    const plot::WaveSnapshot& snapshot,
                    const plot::WaveDisplayData& displayData,
                    const std::vector<std::size_t>& visibleChannelIndices,
                    const ImPlotRect& limits)
        {
            if (!view.phosphorEnabled) {
                view.lastRenderStats.phosphorBackendStatus = "关闭";
                return false;
            }
            if (view.viewMode == plot::WaveViewMode::Split) {
                view.lastRenderStats.phosphorBackendStatus = "Split 暂不支持";
                return false;
            }
            if (!hasVisiblePhosphorChannel(snapshot, visibleChannelIndices)) {
                view.lastRenderStats.phosphorBackendStatus = "无可见模拟通道";
                return false;
            }

            const int width = plotPixelWidth();
            const int height = plotPixelHeight();
            const bool advance = wavePhosphorShouldAdvance(view);
            if (!ensureBackend(view, width, height, advance)) {
                view.lastRenderStats.phosphorBackendStatus =
                    advance ? "禁用: 后端不可用" : "冻结: 等待跟随模式";
                return false;
            }

            if (advance) {
                if (activeBackend_ == plot::WavePhosphorBackend::GpuFbo && beginGpuFrame(view.persistenceWindow)) {
                    accumulate(view, snapshot, displayData, visibleChannelIndices, limits);
                    endGpuFrame();
                } else {
                    activeBackend_ = plot::WavePhosphorBackend::CpuTexture;
                    decay(view.persistenceWindow);
                    accumulate(view, snapshot, displayData, visibleChannelIndices, limits);
                    uploadTexture();
                }
            }

            drawTexture();
            const std::string_view state = advance ? "" : "冻结 ";
            const std::string_view fallback =
                view.phosphorBackend != plot::WavePhosphorBackend::CpuTexture &&
                        activeBackend_ == plot::WavePhosphorBackend::CpuTexture
                    ? " (GPU FBO 回退)"
                    : "";
            view.lastRenderStats.phosphorBackendStatus =
                std::string(state) +
                (activeBackend_ == plot::WavePhosphorBackend::GpuFbo ? "GPU FBO" : "CPU Texture") +
                std::string(fallback);
            return true;
        }

    private:
        static bool hasVisiblePhosphorChannel(const plot::WaveSnapshot& snapshot,
                                              const std::vector<std::size_t>& visibleChannelIndices)
        {
            return std::any_of(visibleChannelIndices.begin(), visibleChannelIndices.end(), [&](std::size_t channelIndex) {
                return channelCanEnterPhosphor(snapshot, channelIndex);
            });
        }

        bool ensureBackend(const plot::WaveViewState& view, const int width, const int height, const bool allowCreate)
        {
            if (texture_ == 0 && !allowCreate) {
                return false;
            }
            if (view.phosphorBackend != plot::WavePhosphorBackend::CpuTexture && ensureGpuFbo(width, height)) {
                activeBackend_ = plot::WavePhosphorBackend::GpuFbo;
                return true;
            }
            if (!ensureCpuTexture(width, height)) {
                return false;
            }
            activeBackend_ = plot::WavePhosphorBackend::CpuTexture;
            return true;
        }

        bool textureApiAvailable() const
        {
            return glGenTextures != nullptr && glBindTexture != nullptr && glTexImage2D != nullptr &&
                   glTexSubImage2D != nullptr && glTexParameteri != nullptr;
        }

        bool ensureCpuTexture(const int width, const int height)
        {
            if (!textureApiAvailable()) {
                return false;
            }
            if (texture_ == 0) {
                glGenTextures(1, &texture_);
            }
            if (texture_ == 0) {
                return false;
            }
            if (width_ == width && height_ == height && !pixels_.empty()) {
                return true;
            }

            width_ = width;
            height_ = height;
            pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4U, 0.0F);
            uploadBytes_.assign(pixels_.size(), 0U);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, uploadBytes_.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            textureNeedsClear_ = true;
            return true;
        }

        bool ensureGpuFbo(const int width, const int height)
        {
            if (!ensureCpuTexture(width, height)) {
                return false;
            }
            auto& api = fboApi();
            if (!api.available || !ensureGpuProgram()) {
                return false;
            }
            if (fbo_ == 0) {
                api.genFramebuffers(1, &fbo_);
            }
            if (fbo_ == 0) {
                return false;
            }

            GLint previousFbo = 0;
            if (glGetIntegerv != nullptr) {
                glGetIntegerv(kGlFramebufferBinding, &previousFbo);
            }
            api.bindFramebuffer(kGlFramebuffer, fbo_);
            api.framebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, texture_, 0);
            const bool complete = api.checkFramebufferStatus(kGlFramebuffer) == kGlFramebufferComplete;
            api.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(previousFbo));
            return complete;
        }

        bool gpuApiAvailable() const
        {
            return textureApiAvailable() && glCreateShader != nullptr && glShaderSource != nullptr &&
                   glCompileShader != nullptr && glGetShaderiv != nullptr && glDeleteShader != nullptr &&
                   glCreateProgram != nullptr && glAttachShader != nullptr && glLinkProgram != nullptr &&
                   glGetProgramiv != nullptr && glDeleteProgram != nullptr && glUseProgram != nullptr &&
                   glGetAttribLocation != nullptr && glGenBuffers != nullptr && glBindBuffer != nullptr &&
                   glBufferData != nullptr && glEnableVertexAttribArray != nullptr && glVertexAttribPointer != nullptr &&
                   glGenVertexArrays != nullptr && glBindVertexArray != nullptr &&
                   glBlendEquationSeparate != nullptr && glBlendFuncSeparate != nullptr && glEnable != nullptr &&
                   glDisable != nullptr && glIsEnabled != nullptr && glViewport != nullptr && glClearColor != nullptr &&
                   glClear != nullptr && glGetIntegerv != nullptr && fboApi().drawArrays != nullptr &&
                   fboApi().getFloatv != nullptr && fboApi().lineWidth != nullptr;
        }

        GLuint compileGpuShader(const GLenum type, const char* source) const
        {
            const GLuint shader = glCreateShader(type);
            if (shader == 0) {
                return 0;
            }
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint ok = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (ok == GL_FALSE) {
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        bool ensureGpuProgram()
        {
            if (program_ != 0 && vao_ != 0 && vbo_ != 0 && positionLocation_ >= 0 && colorLocation_ >= 0) {
                return true;
            }
            if (!gpuApiAvailable()) {
                return false;
            }

            const GLuint vertexShader = compileGpuShader(GL_VERTEX_SHADER, kPhosphorVertexShader);
            const GLuint fragmentShader = compileGpuShader(GL_FRAGMENT_SHADER, kPhosphorFragmentShader);
            if (vertexShader == 0 || fragmentShader == 0) {
                if (vertexShader != 0) {
                    glDeleteShader(vertexShader);
                }
                if (fragmentShader != 0) {
                    glDeleteShader(fragmentShader);
                }
                return false;
            }

            const GLuint program = glCreateProgram();
            if (program == 0) {
                glDeleteShader(vertexShader);
                glDeleteShader(fragmentShader);
                return false;
            }
            glAttachShader(program, vertexShader);
            glAttachShader(program, fragmentShader);
            glLinkProgram(program);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);

            GLint ok = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (ok == GL_FALSE) {
                glDeleteProgram(program);
                return false;
            }

            const GLint positionLocation = glGetAttribLocation(program, "Position");
            const GLint colorLocation = glGetAttribLocation(program, "Color");
            if (positionLocation < 0 || colorLocation < 0) {
                glDeleteProgram(program);
                return false;
            }

            if (vao_ == 0) {
                glGenVertexArrays(1, &vao_);
            }
            if (vbo_ == 0) {
                glGenBuffers(1, &vbo_);
            }
            if (vao_ == 0 || vbo_ == 0) {
                glDeleteProgram(program);
                return false;
            }

            program_ = program;
            positionLocation_ = positionLocation;
            colorLocation_ = colorLocation;
            return true;
        }

        void decay(const double persistenceWindow)
        {
            if (pixels_.empty()) {
                return;
            }
            const double dt = (std::max)(0.0F, ImGui::GetIO().DeltaTime);
            const double window = (std::max)(persistenceWindow, 1e-6);
            const auto factor = static_cast<float>((std::clamp)(std::exp(-dt / window), 0.0, 1.0));
            for (auto& value : pixels_) {
                value *= factor;
            }
        }

        float decayFactor(const double persistenceWindow) const
        {
            const double dt = (std::max)(0.0F, ImGui::GetIO().DeltaTime);
            const double window = (std::max)(persistenceWindow, 1e-6);
            return static_cast<float>((std::clamp)(std::exp(-dt / window), 0.0, 1.0));
        }

        SavedGlState saveGlState() const
        {
            SavedGlState state;
            auto& api = fboApi();
            glGetIntegerv(kGlFramebufferBinding, &state.framebuffer);
            glGetIntegerv(GL_VIEWPORT, state.viewport);
            glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state.arrayBuffer);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state.vertexArray);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &state.blendEquationRgb);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &state.blendEquationAlpha);
            glGetIntegerv(GL_BLEND_SRC_RGB, &state.blendSrcRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &state.blendDstRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &state.blendSrcAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &state.blendDstAlpha);
            api.getFloatv(kGlLineWidth, &state.lineWidth);
            api.getFloatv(kGlColorClearValue, state.clearColor);
            state.blendEnabled = glIsEnabled(GL_BLEND);
            state.scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
            return state;
        }

        void restoreGlState(const SavedGlState& state) const
        {
            auto& api = fboApi();
            api.bindFramebuffer(kGlFramebuffer, static_cast<GLuint>(state.framebuffer));
            glViewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
            if (state.scissorEnabled == GL_TRUE) {
                glEnable(GL_SCISSOR_TEST);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }
            if (state.blendEnabled == GL_TRUE) {
                glEnable(GL_BLEND);
            } else {
                glDisable(GL_BLEND);
            }
            glBlendEquationSeparate(static_cast<GLenum>(state.blendEquationRgb),
                                    static_cast<GLenum>(state.blendEquationAlpha));
            glBlendFuncSeparate(static_cast<GLenum>(state.blendSrcRgb),
                                static_cast<GLenum>(state.blendDstRgb),
                                static_cast<GLenum>(state.blendSrcAlpha),
                                static_cast<GLenum>(state.blendDstAlpha));
            api.lineWidth(state.lineWidth);
            glClearColor(state.clearColor[0], state.clearColor[1], state.clearColor[2], state.clearColor[3]);
            glUseProgram(static_cast<GLuint>(state.program));
            glBindVertexArray(static_cast<GLuint>(state.vertexArray));
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(state.arrayBuffer));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(state.texture));
        }

        bool beginGpuFrame(const double persistenceWindow)
        {
            if (program_ == 0 || vao_ == 0 || vbo_ == 0 || fbo_ == 0 || width_ <= 0 || height_ <= 0) {
                return false;
            }

            savedGpuState_ = saveGlState();
            auto& api = fboApi();
            api.bindFramebuffer(kGlFramebuffer, fbo_);
            glViewport(0, 0, width_, height_);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(program_);
            glBindVertexArray(vao_);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);

            // 核心流程：FBO 中的纹理即余辉缓冲；跟随模式每帧只做一次衰减和新线段累积。
            if (textureNeedsClear_) {
                glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
                glClear(GL_COLOR_BUFFER_BIT);
                textureNeedsClear_ = false;
            } else {
                fadeGpuTexture(decayFactor(persistenceWindow));
            }
            gpuVertices_.clear();
            gpuLineWidth_ = 1.0F;
            gpuFrameActive_ = true;
            return true;
        }

        void endGpuFrame()
        {
            if (!gpuFrameActive_) {
                return;
            }
            flushGpuLines();
            restoreGlState(savedGpuState_);
            gpuFrameActive_ = false;
        }

        void configureGpuVertexLayout() const
        {
            glEnableVertexAttribArray(static_cast<GLuint>(positionLocation_));
            glEnableVertexAttribArray(static_cast<GLuint>(colorLocation_));
            glVertexAttribPointer(static_cast<GLuint>(positionLocation_),
                                  2,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  6 * static_cast<GLsizei>(sizeof(float)),
                                  reinterpret_cast<const void*>(0));
            glVertexAttribPointer(static_cast<GLuint>(colorLocation_),
                                  4,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  6 * static_cast<GLsizei>(sizeof(float)),
                                  reinterpret_cast<const void*>(2 * sizeof(float)));
        }

        void drawGpuVertices(const float* vertices, const std::size_t floatCount, const GLenum mode) const
        {
            if (vertices == nullptr || floatCount < 6U || floatCount % 6U != 0U) {
                return;
            }
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(floatCount * sizeof(float)),
                         vertices,
                         GL_STREAM_DRAW);
            configureGpuVertexLayout();
            fboApi().drawArrays(mode, 0, static_cast<GLsizei>(floatCount / 6U));
        }

        void fadeGpuTexture(const float factor) const
        {
            const float alpha = (std::clamp)(factor, 0.0F, 1.0F);
            const std::array<float, 24> vertices{{
                -1.0F, -1.0F, 0.0F, 0.0F, 0.0F, alpha,
                1.0F,  -1.0F, 0.0F, 0.0F, 0.0F, alpha,
                -1.0F, 1.0F,  0.0F, 0.0F, 0.0F, alpha,
                1.0F,  1.0F,  0.0F, 0.0F, 0.0F, alpha,
            }};
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(kGlZero, GL_SRC_ALPHA, kGlZero, GL_SRC_ALPHA);
            drawGpuVertices(vertices.data(), vertices.size(), kGlTriangleStrip);
        }

        void accumulate(plot::WaveViewState& view,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const std::vector<std::size_t>& visibleChannelIndices,
                        const ImPlotRect& limits)
        {
            if (view.phosphorMode == plot::WavePhosphorMode::Triggered) {
                accumulateTriggered(view, snapshot, displayData, visibleChannelIndices, limits);
                return;
            }
            for (const auto channelIndex : visibleChannelIndices) {
                if (!channelCanEnterPhosphor(snapshot, channelIndex)) {
                    continue;
                }
                const auto* samples = samplesForChannel(displayData, channelIndex);
                if (samples == nullptr) {
                    continue;
                }
                accumulateSampleWindow(*samples,
                                       limits.X.Min,
                                       limits.X.Max,
                                       limits,
                                       wavePhosphorStrokeStyle(snapshot.channels[channelIndex], channelIndex));
            }
        }

        void accumulateTriggered(plot::WaveViewState& view,
                                 const plot::WaveSnapshot& snapshot,
                                 const plot::WaveDisplayData& displayData,
                                 const std::vector<std::size_t>& visibleChannelIndices,
                                 const ImPlotRect& limits)
        {
            if (view.triggerChannelIndex >= snapshot.channels.size() ||
                !channelCanEnterPhosphor(snapshot, view.triggerChannelIndex)) {
                return;
            }
            const auto* triggerSamples = samplesForChannel(displayData, view.triggerChannelIndex);
            if (triggerSamples == nullptr || triggerSamples->size() < 2) {
                return;
            }
            const auto triggers = findWavePhosphorTriggers(
                *triggerSamples, limits.X.Min, limits.X.Max, view.triggerEdge, view.triggerThreshold);
            const double duration = (std::max)(safeDuration(limits), view.minVisibleTimeSpan);
            for (const auto& trigger : triggers) {
                const auto window =
                    makeWavePhosphorTriggerWindow(trigger.time, limits.X.Min, duration, view.triggerPositionRatio);
                for (const auto channelIndex : visibleChannelIndices) {
                    if (!channelCanEnterPhosphor(snapshot, channelIndex)) {
                        continue;
                    }
                    const auto* samples = samplesForChannel(displayData, channelIndex);
                    if (samples == nullptr) {
                        continue;
                    }
                    accumulateSampleWindow(*samples,
                                           window.sourceMinTime,
                                           window.sourceMaxTime,
                                           limits,
                                           wavePhosphorStrokeStyle(snapshot.channels[channelIndex], channelIndex),
                                           &window);
                }
            }
        }

        void accumulateSampleWindow(const std::vector<plot::WaveSample>& samples,
                                    const double minTime,
                                    const double maxTime,
                                    const ImPlotRect& limits,
                                    const WavePhosphorStrokeStyle& style,
                                    const WavePhosphorTriggerWindow* triggerWindow = nullptr)
        {
            if (samples.size() < 2 || maxTime <= minTime) {
                return;
            }
            auto begin = std::lower_bound(samples.begin(),
                                          samples.end(),
                                          minTime,
                                          [](const plot::WaveSample& sample, double value) { return sample.time < value; });
            if (begin != samples.begin()) {
                --begin;
            }
            auto end = std::upper_bound(samples.begin(),
                                        samples.end(),
                                        maxTime,
                                        [](double value, const plot::WaveSample& sample) { return value < sample.time; });
            if (end != samples.end()) {
                ++end;
            }
            if (begin >= end) {
                return;
            }

            auto previous = begin;
            for (auto current = std::next(begin); current != end; ++current) {
                if (current->time < minTime || previous->time > maxTime) {
                    previous = current;
                    continue;
                }
                const double previousTime = triggerWindow == nullptr ? previous->time
                                                                     : alignWavePhosphorSampleTime(*triggerWindow, previous->time);
                const double currentTime = triggerWindow == nullptr ? current->time
                                                                    : alignWavePhosphorSampleTime(*triggerWindow, current->time);
                const float x0 = pixelXForTime(limits, width_, previousTime);
                const float y0 = pixelYForValue(limits, height_, previous->value);
                const float x1 = pixelXForTime(limits, width_, currentTime);
                const float y1 = pixelYForValue(limits, height_, current->value);
                accumulateLine(x0, y0, x1, y1, style);
                previous = current;
            }
        }

        void accumulateLine(const float x0,
                            const float y0,
                            const float x1,
                            const float y1,
                            const WavePhosphorStrokeStyle& style)
        {
            if (activeBackend_ == plot::WavePhosphorBackend::GpuFbo && gpuFrameActive_) {
                accumulateGpuLine(x0, y0, x1, y1, style);
                return;
            }
            accumulateCpuLine(x0, y0, x1, y1, style);
        }

        void accumulateCpuPixel(const int x, const int y, const ImVec4& color, const float amount)
        {
            if (x < 0 || y < 0 || x >= width_ || y >= height_) {
                return;
            }
            const auto offset =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)) * 4U;
            pixels_[offset + 0U] = (std::min)(1.0F, pixels_[offset + 0U] + color.x * amount);
            pixels_[offset + 1U] = (std::min)(1.0F, pixels_[offset + 1U] + color.y * amount);
            pixels_[offset + 2U] = (std::min)(1.0F, pixels_[offset + 2U] + color.z * amount);
            pixels_[offset + 3U] = (std::min)(0.92F, pixels_[offset + 3U] + amount);
        }

        void accumulateCpuLine(const float x0,
                               const float y0,
                               const float x1,
                               const float y1,
                               const WavePhosphorStrokeStyle& style)
        {
            const float dx = x1 - x0;
            const float dy = y1 - y0;
            const int steps = (std::max)(1, static_cast<int>(std::ceil((std::max)(std::abs(dx), std::abs(dy)))));
            const ImVec4 color = style.color;
            const float amount = 0.20F * (std::clamp)(color.w, 0.0F, 1.0F);
            const float radius = (std::max)(0.5F, plot::sanitizeChannelLineWidth(style.lineWidth) * 0.5F);
            const int pixelRadius = static_cast<int>(std::ceil(radius));
            for (int step = 0; step <= steps; ++step) {
                const float ratio = static_cast<float>(step) / static_cast<float>(steps);
                const int x = static_cast<int>(std::lround(x0 + dx * ratio));
                const int y = static_cast<int>(std::lround(y0 + dy * ratio));
                for (int yOffset = -pixelRadius; yOffset <= pixelRadius; ++yOffset) {
                    for (int xOffset = -pixelRadius; xOffset <= pixelRadius; ++xOffset) {
                        const float distance = std::hypot(static_cast<float>(xOffset), static_cast<float>(yOffset));
                        if (distance <= radius) {
                            accumulateCpuPixel(x + xOffset, y + yOffset, color, amount);
                        }
                    }
                }
            }
        }

        void appendGpuVertex(const float x, const float y, const ImVec4& color, const float amount)
        {
            const float safeWidth = static_cast<float>((std::max)(width_ - 1, 1));
            const float safeHeight = static_cast<float>((std::max)(height_ - 1, 1));
            const float ndcX = (x / safeWidth) * 2.0F - 1.0F;
            const float ndcY = -1.0F + (y / safeHeight) * 2.0F;
            gpuVertices_.push_back(ndcX);
            gpuVertices_.push_back(ndcY);
            gpuVertices_.push_back((std::clamp)(color.x * amount, 0.0F, 1.0F));
            gpuVertices_.push_back((std::clamp)(color.y * amount, 0.0F, 1.0F));
            gpuVertices_.push_back((std::clamp)(color.z * amount, 0.0F, 1.0F));
            gpuVertices_.push_back((std::clamp)(amount, 0.0F, 1.0F));
        }

        void accumulateGpuLine(const float x0,
                               const float y0,
                               const float x1,
                               const float y1,
                               const WavePhosphorStrokeStyle& style)
        {
            const ImVec4 color = style.color;
            const float amount = 0.20F * (std::clamp)(color.w, 0.0F, 1.0F);
            const float lineWidth = plot::sanitizeChannelLineWidth(style.lineWidth);
            if (!gpuVertices_.empty() && std::abs(gpuLineWidth_ - lineWidth) > 1e-3F) {
                flushGpuLines();
            }
            gpuLineWidth_ = lineWidth;
            appendGpuVertex(x0, y0, color, amount);
            appendGpuVertex(x1, y1, color, amount);
        }

        void flushGpuLines()
        {
            if (gpuVertices_.empty()) {
                return;
            }
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
            fboApi().lineWidth(gpuLineWidth_);
            drawGpuVertices(gpuVertices_.data(), gpuVertices_.size(), kGlLines);
            gpuVertices_.clear();
        }

        void uploadTexture()
        {
            if (texture_ == 0 || pixels_.empty() || uploadBytes_.size() != pixels_.size()) {
                return;
            }
            for (std::size_t index = 0; index < pixels_.size(); ++index) {
                uploadBytes_[index] = static_cast<std::uint8_t>((std::clamp)(pixels_[index], 0.0F, 1.0F) * 255.0F);
            }
            glBindTexture(GL_TEXTURE_2D, texture_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, uploadBytes_.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        void drawTexture() const
        {
            if (texture_ == 0) {
                return;
            }
            auto* drawList = ImPlot::GetPlotDrawList();
            const ImVec2 plotPos = ImPlot::GetPlotPos();
            const ImVec2 plotSize = ImPlot::GetPlotSize();
            drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(texture_)),
                               plotPos,
                               ImVec2(plotPos.x + plotSize.x, plotPos.y + plotSize.y),
                               ImVec2(0.0F, 0.0F),
                               ImVec2(1.0F, 1.0F),
                               IM_COL32_WHITE);
        }

        GLuint texture_{0};
        GLuint fbo_{0};
        GLuint program_{0};
        GLuint vao_{0};
        GLuint vbo_{0};
        GLint positionLocation_{-1};
        GLint colorLocation_{-1};
        int width_{0};
        int height_{0};
        plot::WavePhosphorBackend activeBackend_{plot::WavePhosphorBackend::CpuTexture};
        bool textureNeedsClear_{true};
        bool gpuFrameActive_{false};
        float gpuLineWidth_{1.0F};
        SavedGlState savedGpuState_{};
        std::vector<float> pixels_;
        std::vector<float> gpuVertices_;
        std::vector<std::uint8_t> uploadBytes_;
    };

    WavePhosphorRenderer& phosphorRenderer()
    {
        static WavePhosphorRenderer renderer;
        return renderer;
    }

} // namespace

std::vector<WavePhosphorTrigger> findWavePhosphorTriggers(const std::vector<plot::WaveSample>& samples,
                                                          const double minTime,
                                                          const double maxTime,
                                                          const plot::WavePhosphorTriggerEdge edge,
                                                          const double threshold)
{
    std::vector<WavePhosphorTrigger> triggers;
    if (samples.size() < 2 || maxTime <= minTime || !std::isfinite(threshold)) {
        return triggers;
    }

    for (std::size_t index = 1; index < samples.size(); ++index) {
        const auto& previous = samples[index - 1U];
        const auto& current = samples[index];
        if (!std::isfinite(previous.time) || !std::isfinite(previous.value) || !std::isfinite(current.time) ||
            !std::isfinite(current.value) || current.time < minTime || previous.time > maxTime) {
            continue;
        }
        const bool matched =
            edge == plot::WavePhosphorTriggerEdge::Rising
                ? (previous.value < threshold && current.value >= threshold)
                : (previous.value > threshold && current.value <= threshold);
        if (!matched || std::abs(current.value - previous.value) <= 1e-12) {
            continue;
        }
        const double ratio = (std::clamp)((threshold - previous.value) / (current.value - previous.value), 0.0, 1.0);
        const double time = previous.time + (current.time - previous.time) * ratio;
        if (time < minTime || time > maxTime) {
            continue;
        }
        triggers.push_back({.time = time, .value = threshold});
    }
    return triggers;
}

WavePhosphorTriggerWindow makeWavePhosphorTriggerWindow(const double triggerTime,
                                                        const double visibleMinTime,
                                                        const double visibleDuration,
                                                        const double triggerPositionRatio)
{
    const double duration = std::isfinite(visibleDuration) && visibleDuration > 0.0 ? visibleDuration : 1.0;
    const double ratio = (std::clamp)(triggerPositionRatio, 0.0, 1.0);
    const double sourceMinTime = triggerTime - duration * ratio;
    return {
        .sourceMinTime = sourceMinTime,
        .sourceMaxTime = sourceMinTime + duration,
        .targetMinTime = visibleMinTime,
        .targetMaxTime = visibleMinTime + duration,
    };
}

double alignWavePhosphorSampleTime(const WavePhosphorTriggerWindow& window, const double sourceTime)
{
    return window.targetMinTime + (sourceTime - window.sourceMinTime);
}

bool wavePhosphorShouldAdvance(const plot::WaveViewState& view)
{
    return view.phosphorEnabled && view.autoFollowLatest;
}

bool renderWavePhosphor(plot::WaveViewState& view,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const std::vector<std::size_t>& visibleChannelIndices,
                        const ImPlotRect& limits)
{
    return phosphorRenderer().render(view, snapshot, displayData, visibleChannelIndices, limits);
}

} // namespace protoscope::ui
