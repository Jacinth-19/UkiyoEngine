// OpenGL 4.5 backend for the Ukiyo RHI.
// Uses DSA (named) entry points throughout, one shared VAO for the Vertex layout,
// and one FBO per render pass (created and destroyed around the pass).
#include "rhi/internal.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef UKIYO_SHADER_DIR
#error "UKIYO_SHADER_DIR must point at the generated shader directory"
#endif

namespace rhi {
namespace {

constexpr GLuint kPushUboBinding = 1;   // see common.glsl PUSH macro
constexpr GLsizei kVertexStride = 96;   // sizeof(rhi::Vertex)

GLenum toGLInternal(Format f) {
    switch (f) {
    case Format::RGBA8: return GL_RGBA8;
    case Format::RGBA8_SRGB: return GL_SRGB8_ALPHA8;
    case Format::RGBA16F: return GL_RGBA16F;
    case Format::D32F: return GL_DEPTH_COMPONENT32F;
    case Format::Backbuffer: return GL_RGBA8;
    }
    return GL_RGBA8;
}

GLuint compileStage(GLenum type, const std::string& source, const std::string& name) {
    GLuint s = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        glDeleteShader(s);
        throw std::runtime_error("GLSL compile error in " + name + ":\n" + log);
    }
    return s;
}

class GLBuffer final : public Buffer {
public:
    GLBuffer(size_t size) : size_(size) {
        glCreateBuffers(1, &id_);
        glNamedBufferStorage(id_, static_cast<GLsizeiptr>(size), nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    ~GLBuffer() override { glDeleteBuffers(1, &id_); }
    void upload(const void* data, size_t size, size_t offset) override {
        glNamedBufferSubData(id_, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
    }
    size_t size() const override { return size_; }
    GLuint id() const { return id_; }

private:
    GLuint id_ = 0;
    size_t size_ = 0;
};

class GLTexture final : public Texture {
public:
    GLTexture(const TextureDesc& d, const void* rgba) : desc_(d) {
        levels_ = d.mipmaps ? static_cast<GLsizei>(std::floor(std::log2(std::max(d.width, d.height)))) + 1 : 1;
        glCreateTextures(GL_TEXTURE_2D, 1, &id_);
        glTextureStorage2D(id_, levels_, toGLInternal(d.format), static_cast<GLsizei>(d.width), static_cast<GLsizei>(d.height));
        if (rgba) {
            glTextureSubImage2D(id_, 0, 0, 0, static_cast<GLsizei>(d.width), static_cast<GLsizei>(d.height),
                                GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            if (levels_ > 1) glGenerateTextureMipmap(id_);
        }
        GLenum filter = d.filter == Filter::Nearest ? GL_NEAREST : GL_LINEAR;
        GLenum minF = (levels_ > 1) ? (d.filter == Filter::Nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR) : filter;
        GLenum wrap = d.wrap == Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(minF));
        glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
        glTextureParameteri(id_, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap));
        glTextureParameteri(id_, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap));
    }
    ~GLTexture() override { glDeleteTextures(1, &id_); }
    uint32_t width() const override { return desc_.width; }
    uint32_t height() const override { return desc_.height; }
    Format format() const override { return desc_.format; }
    GLuint id() const { return id_; }

private:
    TextureDesc desc_;
    GLuint id_ = 0;
    GLsizei levels_ = 1;
};

class GLPipeline final : public Pipeline {
public:
    GLuint program = 0;
    PipelineDesc desc;
    ~GLPipeline() override { glDeleteProgram(program); }
};

class GLDescriptorSet final : public DescriptorSet {
public:
    void setBuffer(uint32_t binding, Buffer* buffer) override {
        GLuint id = static_cast<GLBuffer*>(buffer)->id();
        upsert(buffers_, binding, id);
    }
    void setTexture(uint32_t binding, Texture* texture) override {
        GLuint id = static_cast<GLTexture*>(texture)->id();
        upsert(textures_, binding, id);
    }
    std::vector<std::pair<uint32_t, GLuint>> buffers_;
    std::vector<std::pair<uint32_t, GLuint>> textures_;

private:
    static void upsert(std::vector<std::pair<uint32_t, GLuint>>& v, uint32_t b, GLuint id) {
        for (auto& e : v) if (e.first == b) { e.second = id; return; }
        v.emplace_back(b, id);
    }
};

class GLDevice final : public Device {
public:
    GLDevice(const DeviceConfig& cfg, GLFWwindow* window) : window_(window), headless_(cfg.headless) {
        if (!window_) throw std::runtime_error("GL backend needs a window for its context");
        glfwMakeContextCurrent(window_);
        if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress))
            throw std::runtime_error("gladLoadGL failed");
        if (!headless_) glfwSwapInterval(1);

        // Our projections use [0,1] depth (glm::*_ZO) and y-up window coordinates.
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        glCreateVertexArrays(1, &vao_);
        // Vertex layout: pos, normal, uv, joints (u16x4), weights.
        glEnableVertexArrayAttrib(vao_, 0);
        glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glEnableVertexArrayAttrib(vao_, 1);
        glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, 12);
        glEnableVertexArrayAttrib(vao_, 2);
        glVertexArrayAttribFormat(vao_, 2, 2, GL_FLOAT, GL_FALSE, 24);
        glEnableVertexArrayAttrib(vao_, 3);
        glVertexArrayAttribIFormat(vao_, 3, 4, GL_UNSIGNED_SHORT, 32);
        glEnableVertexArrayAttrib(vao_, 4);
        glVertexArrayAttribFormat(vao_, 4, 4, GL_FLOAT, GL_FALSE, 40);
        glEnableVertexArrayAttrib(vao_, 5);
        glVertexArrayAttribFormat(vao_, 5, 4, GL_FLOAT, GL_FALSE, 56);
        glEnableVertexArrayAttrib(vao_, 6);
        glVertexArrayAttribFormat(vao_, 6, 2, GL_FLOAT, GL_FALSE, 72);
        glEnableVertexArrayAttrib(vao_, 7);
        glVertexArrayAttribFormat(vao_, 7, 4, GL_FLOAT, GL_FALSE, 80);
        for (GLuint a = 0; a < 5; ++a) glVertexArrayAttribBinding(vao_, a, 0);
        glBindVertexArray(vao_);

        glCreateBuffers(1, &pushUbo_);
        glNamedBufferStorage(pushUbo_, 128, nullptr, GL_DYNAMIC_STORAGE_BIT);
        glBindBufferBase(GL_UNIFORM_BUFFER, kPushUboBinding, pushUbo_);

        width_ = cfg.width;
        height_ = cfg.height;
        if (headless_) {
            // Offscreen backbuffer with the same contract as the swapchain.
            TextureDesc d;
            d.width = cfg.width;
            d.height = cfg.height;
            d.format = Format::RGBA8;
            d.filter = Filter::Nearest;
            offscreenColor_ = std::make_unique<GLTexture>(d, nullptr);
            glCreateFramebuffers(1, &offscreenFbo_);
            glNamedFramebufferTexture(offscreenFbo_, GL_COLOR_ATTACHMENT0, offscreenColor_->id(), 0);
        } else {
            int fw = 0, fh = 0;
            glfwGetFramebufferSize(window_, &fw, &fh);
            width_ = static_cast<uint32_t>(fw);
            height_ = static_cast<uint32_t>(fh);
        }
    }

    ~GLDevice() override {
        glFinish();
        if (offscreenFbo_) glDeleteFramebuffers(1, &offscreenFbo_);
        glDeleteBuffers(1, &pushUbo_);
        glDeleteVertexArrays(1, &vao_);
    }

    Backend backend() const override { return Backend::OpenGL; }
    std::string deviceName() const override {
        return std::string("OpenGL ") + reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    }
    bool headless() const override { return headless_; }
    uint32_t backbufferWidth() const override { return width_; }
    uint32_t backbufferHeight() const override { return height_; }
    Format backbufferFormat() const override { return Format::RGBA8; }
    bool presentFlipsY() const override { return false; }

    std::unique_ptr<Buffer> createBuffer(BufferUsage, size_t size) override {
        return std::make_unique<GLBuffer>(std::max<size_t>(size, 16));
    }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc, const void* rgba8) override {
        return std::make_unique<GLTexture>(desc, rgba8);
    }

    std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc) override {
        auto p = std::make_unique<GLPipeline>();
        p->desc = desc;
        std::string dir = UKIYO_SHADER_DIR "/gl/";
        std::string vsrc = readText(dir + desc.vertexShader + ".vert.glsl");
        std::string fsrc;
        if (!desc.fragmentGlsl.empty()) {
            // Generated materials: the same text Vulkan compiles, with the set index removed. GL
            // binds by binding number alone, and the generated bindings are unique across sets.
            static const std::regex setQualifier(R"(set\s*=\s*\d+\s*,\s*)");
            fsrc = std::regex_replace(desc.fragmentGlsl, setQualifier, "");
        } else {
            fsrc = readText(dir + desc.fragmentShader + ".frag.glsl");
        }
        GLuint vs = compileStage(GL_VERTEX_SHADER, vsrc, desc.vertexShader + ".vert");
        GLuint fs = compileStage(GL_FRAGMENT_SHADER, fsrc, desc.fragmentShader + ".frag");
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            glDeleteProgram(prog);
            throw std::runtime_error("GL link error (" + desc.vertexShader + "/" + desc.fragmentShader + "):\n" + log);
        }
        p->program = prog;
        return p;
    }

    std::unique_ptr<DescriptorSet> createDescriptorSet(SetType) override {
        return std::make_unique<GLDescriptorSet>();
    }

    bool beginFrame() override {
        if (!headless_) {
            int fw = 0, fh = 0;
            glfwGetFramebufferSize(window_, &fw, &fh);
            if (fw == 0 || fh == 0) return false;   // minimised
            width_ = static_cast<uint32_t>(fw);
            height_ = static_cast<uint32_t>(fh);
        }
        return true;
    }

    void endFrame(bool capture) override {
        if (capture) {
            GLuint src = headless_ ? offscreenFbo_ : 0;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            std::vector<uint8_t> px(static_cast<size_t>(width_) * height_ * 4);
            glReadPixels(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_), GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            // GL rows run bottom-up; captures are returned top row first.
            const size_t stride = static_cast<size_t>(width_) * 4;
            capture_.resize(px.size());
            for (uint32_t y = 0; y < height_; ++y)
                std::memcpy(capture_.data() + y * stride, px.data() + (height_ - 1 - y) * stride, stride);
        }
        if (!headless_) glfwSwapBuffers(window_);
    }

    std::vector<uint8_t> takeCapture() override {
        std::vector<uint8_t> out;
        out.swap(capture_);
        return out;
    }

    void waitIdle() override { glFinish(); }

    void beginPass(const PassDesc& pass) override {
        GLuint fbo = 0;
        uint32_t w = width_, h = height_;
        ownFbo_ = false;
        if (pass.backbuffer) {
            fbo = headless_ ? offscreenFbo_ : 0;
            if (headless_) { w = backbufferWidth(); h = backbufferHeight(); }
        } else {
            glCreateFramebuffers(1, &fbo);
            ownFbo_ = true;
            std::vector<GLenum> drawBufs;
            for (size_t i = 0; i < pass.colors.size(); ++i) {
                auto* t = static_cast<GLTexture*>(pass.colors[i]);
                glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i), t->id(), 0);
                drawBufs.push_back(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i));
                w = t->width();
                h = t->height();
            }
            if (pass.depth) {
                auto* t = static_cast<GLTexture*>(pass.depth);
                glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, t->id(), 0);
                if (pass.colors.empty()) { w = t->width(); h = t->height(); }
            }
            if (drawBufs.empty()) glNamedFramebufferDrawBuffer(fbo, GL_NONE);
            else glNamedFramebufferDrawBuffers(fbo, static_cast<GLsizei>(drawBufs.size()), drawBufs.data());
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        fbo_ = fbo;
        passW_ = w;
        passH_ = h;
        glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));

        glDepthMask(GL_TRUE);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (pass.clearColor) {
            const GLfloat c[4] = {pass.clearValue.r, pass.clearValue.g, pass.clearValue.b, pass.clearValue.a};
            size_t count = pass.colors.empty() ? 1 : pass.colors.size();   // default FB / offscreen backbuffer: one colour
            for (size_t i = 0; i < count; ++i) glClearNamedFramebufferfv(fbo, GL_COLOR, static_cast<GLint>(i), c);
        }
        if (pass.clearDepth && (pass.depth || (pass.backbuffer && fbo == 0))) {
            const GLfloat one = 1.0f;
            glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &one);
        }
    }

    void endPass() override {
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (ownFbo_) glDeleteFramebuffers(1, &fbo_);
        ownFbo_ = false;
        fbo_ = 0;
    }

    void setViewport(int x, int y, int w, int h) override {
        glViewport(x, y, w, h);
    }

    void bindPipeline(Pipeline* pipeline) override {
        auto* p = static_cast<GLPipeline*>(pipeline);
        const PipelineDesc& d = p->desc;
        glUseProgram(p->program);
        if (d.cull == Cull::None) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(d.cull == Cull::Back ? GL_BACK : GL_FRONT); }
        if (d.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(d.depthWrite ? GL_TRUE : GL_FALSE);
        switch (d.blend) {
        case Blend::None: glDisable(GL_BLEND); break;
        case Blend::Alpha:
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case Blend::Additive:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            break;
        }
    }

    void bindDescriptorSet(uint32_t, DescriptorSet* set) override {
        auto* s = static_cast<GLDescriptorSet*>(set);
        for (auto& [binding, id] : s->buffers_) glBindBufferBase(GL_UNIFORM_BUFFER, binding, id);
        for (auto& [binding, id] : s->textures_) glBindTextureUnit(binding, id);
    }

    void bindVertexBuffer(Buffer* buffer) override {
        glVertexArrayVertexBuffer(vao_, 0, static_cast<GLBuffer*>(buffer)->id(), 0, kVertexStride);
    }

    void bindIndexBuffer(Buffer* buffer) override {
        glVertexArrayElementBuffer(vao_, static_cast<GLBuffer*>(buffer)->id());
    }

    void pushConstants(const void* data, size_t size) override {
        glNamedBufferSubData(pushUbo_, 0, static_cast<GLsizeiptr>(size), data);
    }

    void drawIndexed(uint32_t indexCount, uint32_t firstIndex) override {
        glBindVertexArray(vao_);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(static_cast<uintptr_t>(firstIndex) * 4));
    }

    void drawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount) override {
        glBindVertexArray(vao_);
        glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, nullptr,
                                static_cast<GLsizei>(instanceCount));
    }

    void drawFullscreen() override {
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void drawInstanced(uint32_t vertexCount, uint32_t instanceCount) override {
        glBindVertexArray(vao_);
        glDrawArraysInstanced(GL_TRIANGLES, 0, static_cast<GLsizei>(vertexCount),
                              static_cast<GLsizei>(instanceCount));
    }

private:
    static std::string readText(const std::string& path) {
        auto bytes = readFileBytes(path);
        return std::string(bytes.begin(), bytes.end());
    }

    GLFWwindow* window_ = nullptr;
    bool headless_ = false;
    uint32_t width_ = 0, height_ = 0;
    uint32_t passW_ = 0, passH_ = 0;
    GLuint vao_ = 0;
    GLuint pushUbo_ = 0;
    GLuint fbo_ = 0;
    bool ownFbo_ = false;
    GLuint offscreenFbo_ = 0;
    std::unique_ptr<GLTexture> offscreenColor_;
    std::vector<uint8_t> capture_;
};

} // namespace

std::unique_ptr<Device> createGLDevice(const DeviceConfig& config, GLFWwindow* window) {
    return std::make_unique<GLDevice>(config, window);
}

} // namespace rhi
