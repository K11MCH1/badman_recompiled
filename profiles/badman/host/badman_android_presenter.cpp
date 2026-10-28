#include "badman_android_presenter.hpp"
#include "badman_scene_render.hpp"
#include "badman_window.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace badman {
namespace {
using Clock = std::chrono::steady_clock;

// Scene replay: the GE's sprites, triangles and clears at the surface's
// resolution, with the exact rasterizer's texel addressing, 1/16-texel
// bilinear weights and modulate rounding (the Windows presenter's shader).
constexpr char scene_vertex_source[] = R"(#version 300 es
uniform vec4 native; // Width, height, left and top of the native area shown.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
out vec2 v_uv;
out vec4 v_color;
void main() {
    gl_Position = vec4((position - native.zw) / native.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
    v_uv = uv; v_color = color;
})";
constexpr char scene_fragment_source[] = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D scene_texture;
uniform ivec2 texture_size;
uniform uint flags;
in vec2 v_uv;
in vec4 v_color;
out vec4 fragment;
vec4 texel(ivec2 at) {
    if ((flags & 8u) != 0u) at = at & (texture_size - 1); else at = clamp(at, ivec2(0), texture_size - 1);
    return round(texelFetch(scene_texture, at, 0) * 255.0);
}
void main() {
    vec4 color = round(v_color * 255.0);
    if ((flags & 1u) == 0u) { fragment = color / 255.0; return; }
    vec4 sampled;
    if ((flags & 2u) != 0u) {
        vec2 position = v_uv - 0.5;
        vec2 weight = floor(fract(position) * 16.0);
        ivec2 at = ivec2(floor(position));
        vec4 top = floor((texel(at) * (16.0 - weight.x) + texel(at + ivec2(1, 0)) * weight.x) / 16.0);
        vec4 bottom = floor((texel(at + ivec2(0, 1)) * (16.0 - weight.x) + texel(at + ivec2(1, 1)) * weight.x) / 16.0);
        sampled = floor((top * (16.0 - weight.y) + bottom * weight.y) / 16.0);
    } else sampled = texel(ivec2(floor(v_uv)));
    vec3 rgb = floor(sampled.rgb * (color.rgb + 1.0) / 256.0);
    float alpha = (flags & 4u) != 0u ? floor(sampled.a * (color.a + 1.0) / 256.0) : color.a;
    fragment = vec4(rgb, alpha) / 255.0;
})";
// The exact frame: one triangle over the viewport. Its AARRGGBB words arrive
// as RGBA bytes, so red and blue swap back here.
constexpr char picture_vertex_source[] = R"(#version 300 es
out vec2 v_uv;
void main() {
    v_uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(v_uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
})";
constexpr char picture_fragment_source[] = R"(#version 300 es
precision mediump float;
uniform sampler2D picture;
in vec2 v_uv;
out vec4 fragment;
void main() { fragment = vec4(texture(picture, v_uv).bgr, 1.0); })";

void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(std::string("GPU scanout: cannot ") + what + " (EGL 0x" +
        [] { char text[16]; std::snprintf(text, sizeof(text), "%04X", eglGetError()); return std::string(text); }() + ")");
}
GLuint shader(GLenum type, const char *source) {
    const auto name = glCreateShader(type);
    glShaderSource(name, 1, &source, nullptr);
    glCompileShader(name);
    GLint ok = 0; glGetShaderiv(name, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]{}; glGetShaderInfoLog(name, sizeof(log) - 1, nullptr, log);
        throw std::runtime_error(std::string("GPU scanout: a shader failed to compile: ") + log);
    }
    return name;
}
GLuint program(const char *vertex, const char *fragment) {
    const auto name = glCreateProgram();
    const auto a = shader(GL_VERTEX_SHADER, vertex), b = shader(GL_FRAGMENT_SHADER, fragment);
    glAttachShader(name, a); glAttachShader(name, b);
    glLinkProgram(name);
    glDeleteShader(a); glDeleteShader(b);
    GLint ok = 0; glGetProgramiv(name, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]{}; glGetProgramInfoLog(name, sizeof(log) - 1, nullptr, log);
        throw std::runtime_error(std::string("GPU scanout: a program failed to link: ") + log);
    }
    return name;
}
GLenum source_factor(unsigned code) {
    switch (code) {
    case 0: return GL_DST_COLOR; case 1: return GL_ONE_MINUS_DST_COLOR;
    case 2: return GL_SRC_ALPHA; case 3: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: return GL_DST_ALPHA; case 5: return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_CONSTANT_COLOR;
    }
}
GLenum destination_factor(unsigned code) {
    switch (code) {
    case 0: return GL_SRC_COLOR; case 1: return GL_ONE_MINUS_SRC_COLOR;
    case 2: return GL_SRC_ALPHA; case 3: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: return GL_DST_ALPHA; case 5: return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_CONSTANT_COLOR;
    }
}
GLenum equation(unsigned code) {
    switch (code) {
    case 1: return GL_FUNC_SUBTRACT; case 2: return GL_FUNC_REVERSE_SUBTRACT;
    case 3: return GL_MIN; case 4: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}
}

struct AndroidPresenter::Impl {
    const bool interpolate;
    std::atomic<bool> smooth;
    bool statistics_log{};

    // Handed over by the game and UI threads under the mutex.
    mutable std::mutex mutex;
    std::condition_variable wake, applied;
    ANativeWindow *requested_surface{};
    std::uint64_t surface_request{}, surface_applied{};
    std::vector<std::uint8_t> pending, raw;
    std::vector<std::uint32_t> working;
    std::uint32_t pending_stride{}, pending_format{}, pending_width{}, pending_height{};
    std::shared_ptr<const Scene> pending_scene;
    bool frame_ready{}, stopping{};
    std::string failure;
    std::atomic<bool> failed{false};
    std::atomic<std::uint64_t> submitted{}, presented{}, scenes{}, matching_scenes{}, interpolated{};
    std::thread thread;

    // Presentation thread only.
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLConfig config{};
    EGLContext context{EGL_NO_CONTEXT};
    EGLSurface idle{EGL_NO_SURFACE}, surface{EGL_NO_SURFACE};
    ANativeWindow *window{};
    int surface_width{}, surface_height{};
    GLuint scene_program{}, picture_program{}, vertex_buffer{}, vertex_array{}, empty_array{};
    GLint native_location{}, size_location{}, flags_location{};
    GLuint picture{}, check_framebuffer{}, check_target{};
    std::uint32_t picture_width{}, picture_height{};
    std::vector<std::uint8_t> check_pixels;
    struct CachedTexture { GLuint name{}; std::uint64_t used{}; };
    std::unordered_map<std::uint64_t, CachedTexture> textures;
    std::uint64_t texture_clock{};
    ScenePacer pacer;
    std::vector<SceneGpuVertex> vertices;
    std::vector<SceneBatch> batches;
    std::uint64_t mismatch_pixels{}, rate_presents{};
    Clock::time_point next_statistics{}, rate_start{};
    // Development: when BADMAN_MISMATCH_DIRECTORY names an existing folder, some
    // rejected scenes are saved there (the exact frame, the re-render and the
    // draw list), spread out so a long run of them is sampled throughout.
    std::string mismatch_directory;
    unsigned mismatch_dumps{};
    std::uint64_t last_dump_scene{};

    Impl(bool interpolation, bool smooth_filter)
        : interpolate(interpolation), smooth(smooth_filter), statistics_log(std::getenv("BADMAN_SCENE_STATS") != nullptr),
          pacer(interpolation) {
        struct stat folder{};
        if (const auto directory = std::getenv("BADMAN_MISMATCH_DIRECTORY");
            directory && *directory && stat(directory, &folder) == 0 && S_ISDIR(folder.st_mode)) mismatch_directory = directory;
        thread = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_all();
        if (thread.joinable()) thread.join();
        if (requested_surface) ANativeWindow_release(requested_surface);
    }

    void create_context() {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "open the EGL display");
        const EGLint attributes[]{EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint count = 0;
        require(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "find an RGBA8888 OpenGL ES 3 configuration");
        const EGLint version[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
        require(context != EGL_NO_CONTEXT, "create an OpenGL ES 3 context");
        // A tiny off-screen surface keeps the context usable while the app has no surface.
        const EGLint size[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        idle = eglCreatePbufferSurface(display, config, size);
        require(idle != EGL_NO_SURFACE && eglMakeCurrent(display, idle, idle, context), "make the context current");
        scene_program = program(scene_vertex_source, scene_fragment_source);
        native_location = glGetUniformLocation(scene_program, "native");
        size_location = glGetUniformLocation(scene_program, "texture_size");
        flags_location = glGetUniformLocation(scene_program, "flags");
        glUseProgram(scene_program);
        glUniform1i(glGetUniformLocation(scene_program, "scene_texture"), 0);
        picture_program = program(picture_vertex_source, picture_fragment_source);
        glUseProgram(picture_program);
        glUniform1i(glGetUniformLocation(picture_program, "picture"), 0);
        glGenBuffers(1, &vertex_buffer);
        glGenVertexArrays(1, &vertex_array);
        glBindVertexArray(vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(SceneGpuVertex), reinterpret_cast<void *>(0));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SceneGpuVertex), reinterpret_cast<void *>(8));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(SceneGpuVertex), reinterpret_cast<void *>(16));
        glGenVertexArrays(1, &empty_array);
        glBindVertexArray(0);
        // The native-size check target and its readback.
        glGenTextures(1, &check_target);
        glBindTexture(GL_TEXTURE_2D, check_target);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 480, 272);
        glGenFramebuffers(1, &check_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, check_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, check_target, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("GPU scanout: the scene check target is incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        check_pixels.resize(480u * 272u * 4u);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DITHER);
    }
    void release_window() {
        if (surface != EGL_NO_SURFACE) {
            eglMakeCurrent(display, idle, idle, context);
            eglDestroySurface(display, surface);
            surface = EGL_NO_SURFACE;
        }
        if (window) { ANativeWindow_release(window); window = nullptr; }
        surface_width = surface_height = 0;
    }
    void adopt_window(ANativeWindow *next) {
        release_window();
        window = next;
        if (!window) return;
        EGLint format = 0;
        eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);
        ANativeWindow_setBuffersGeometry(window, 0, 0, format); // The window's own size; undo the CPU path's scaling.
        surface = eglCreateWindowSurface(display, config, window, nullptr);
        require(surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context), "attach to the game surface");
        eglSwapInterval(display, 1); // Wait for the display refresh: one new picture per refresh.
    }
    void query_size() {
        if (surface == EGL_NO_SURFACE) return;
        EGLint width = 0, height = 0;
        eglQuerySurface(display, surface, EGL_WIDTH, &width);
        eglQuerySurface(display, surface, EGL_HEIGHT, &height);
        surface_width = width; surface_height = height;
    }

    GLuint scene_texture(const SceneTexture &texture) {
        auto found = textures.find(texture.key);
        if (found == textures.end()) {
            if (textures.size() >= 384) { // Drop the least recently used texture.
                auto oldest = textures.begin();
                for (auto it = textures.begin(); it != textures.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
                glDeleteTextures(1, &oldest->second.name);
                textures.erase(oldest);
            }
            CachedTexture cached;
            glGenTextures(1, &cached.name);
            glBindTexture(GL_TEXTURE_2D, cached.name);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            // Same bytes as the Direct3D 11 presenter's R8G8B8A8 upload.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(texture.width), static_cast<GLsizei>(texture.height), 0,
                GL_RGBA, GL_UNSIGNED_BYTE, texture.texels.data());
            found = textures.emplace(texture.key, cached).first;
        }
        found->second.used = ++texture_clock;
        return found->second.name;
    }
    // Draws the built batches into a viewport given in top-down pixels of a
    // target target_height tall, showing native x from left over width pixels.
    void render_batches(const Scene &scene, int x, int y, int w, int h, int target_height, double left = 0, double width = 480) {
        if (vertices.empty()) return;
        glUseProgram(scene_program);
        glBindVertexArray(vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(SceneGpuVertex)), vertices.data(), GL_STREAM_DRAW);
        glUniform4f(native_location, static_cast<float>(width), 272.0f, static_cast<float>(left), 0.0f);
        glViewport(x, target_height - (y + h), w, h);
        glEnable(GL_SCISSOR_TEST);
        glActiveTexture(GL_TEXTURE0);
        const auto sx = static_cast<float>(w) / static_cast<float>(width), sy = static_cast<float>(h) / 272.0f;
        const auto origin = static_cast<float>(left);
        for (const auto &batch : batches) {
            GLint size[2]{1, 1};
            if (batch.texture >= 0) {
                const auto &texture = *scene.textures[batch.texture];
                glBindTexture(GL_TEXTURE_2D, scene_texture(texture));
                size[0] = static_cast<GLint>(texture.width); size[1] = static_cast<GLint>(texture.height);
            }
            glUniform2i(size_location, size[0], size[1]);
            glUniform1ui(flags_location, batch.flags);
            const auto mask = scene_write_mask(batch.blend_key);
            glColorMask((mask & 1u) != 0, (mask & 2u) != 0, (mask & 4u) != 0, (mask & 8u) != 0);
            if (scene_blend_on(batch.blend_key)) {
                const auto mode = scene_blend_mode(batch.blend_key);
                glEnable(GL_BLEND);
                glBlendFuncSeparate(source_factor(mode & 15u), destination_factor((mode >> 4) & 15u), GL_ONE, GL_ZERO);
                glBlendEquationSeparate(equation(std::min((mode >> 8) & 7u, 5u)), GL_FUNC_ADD);
                const auto factor = batch.blend_factor;
                glBlendColor((factor & 255u) / 255.0f, ((factor >> 8) & 255u) / 255.0f, ((factor >> 16) & 255u) / 255.0f, 1.0f);
            } else glDisable(GL_BLEND);
            const auto left_edge = std::lround(x + (batch.scissor[0] - origin) * sx), top_edge = std::lround(y + batch.scissor[1] * sy);
            const auto right_edge = std::lround(x + (batch.scissor[2] - origin) * sx), bottom_edge = std::lround(y + batch.scissor[3] * sy);
            glScissor(static_cast<GLint>(left_edge), static_cast<GLint>(target_height - bottom_edge),
                static_cast<GLsizei>(std::max(0L, right_edge - left_edge)), static_cast<GLsizei>(std::max(0L, bottom_edge - top_edge)));
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(batch.first), static_cast<GLsizei>(batch.count));
        }
        glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    // A scene is used only if it re-renders to the exact frame at native size.
    bool scene_matches(const Scene &scene, const std::vector<std::uint32_t> &exact, std::uint32_t width, std::uint32_t height) {
        const char *reason = nullptr;
        std::uint64_t different = 0;
        if (!scene.complete) reason = "a draw was not recorded";
        else if (width != 480 || height != 272 || exact.size() != 480u * 272u) reason = "frame size";
        else if (!build_scene_geometry(vertices, batches, scene, nullptr, 1, false)) reason = "draw state the presenter cannot reproduce";
        else {
            glBindFramebuffer(GL_FRAMEBUFFER, check_framebuffer);
            glViewport(0, 0, 480, 272);
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            render_batches(scene, 0, 0, 480, 272, 272);
            glReadPixels(0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, check_pixels.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            different = scene_mismatch(check_pixels.data(), 480u * 4u, true, exact.data());
            mismatch_pixels += different;
            if (different > scene_mismatch_limit) reason = "pixels differ";
        }
        const auto index = scenes.load(std::memory_order_relaxed);
        if (reason && !mismatch_directory.empty() && mismatch_dumps < 24 && (!mismatch_dumps || index >= last_dump_scene + 30)) {
            last_dump_scene = index;
            dump_rejected(scene, exact, width, height, reason, different);
        }
        return reason == nullptr;
    }
    void dump_rejected(const Scene &scene, const std::vector<std::uint32_t> &exact, std::uint32_t width, std::uint32_t height,
                       const char *reason, std::uint64_t different) {
        const auto base = mismatch_directory + "/scene-" + std::to_string(mismatch_dumps++);
        const bool rendered = different != 0;
        // Bounds of the pixels that differ (the same test as scene_mismatch).
        int x0 = 480, y0 = 272, x1 = -1, y1 = -1;
        if (rendered)
            for (int y = 0; y < 272; ++y)
                for (int x = 0; x < 480; ++x) {
                    const auto *got = &check_pixels[(static_cast<std::size_t>(271 - y) * 480u + x) * 4u];
                    const auto wanted = exact[static_cast<std::size_t>(y) * 480u + x];
                    const int dr = int(got[0]) - int((wanted >> 16) & 255u), dg = int(got[1]) - int((wanted >> 8) & 255u),
                              db = int(got[2]) - int(wanted & 255u);
                    if (std::max({std::abs(dr), std::abs(dg), std::abs(db)}) <= 40) continue;
                    x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
        if (std::FILE *text = std::fopen((base + ".txt").c_str(), "w")) {
            std::fprintf(text, "reason=%s different=%llu frame=%ux%u complete=%d draws=%zu textures=%zu\n", reason,
                static_cast<unsigned long long>(different), width, height, scene.complete ? 1 : 0, scene.draws.size(), scene.textures.size());
            if (x1 >= 0) std::fprintf(text, "differing_bounds=%d,%d..%d,%d\n", x0, y0, x1, y1);
            for (std::size_t i = 0; i < scene.draws.size(); ++i) {
                const auto &draw = scene.draws[i];
                float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
                for (std::uint32_t v = draw.first; v < draw.first + draw.count && v < scene.vertices.size(); ++v) {
                    left = std::min(left, scene.vertices[v].x); right = std::max(right, scene.vertices[v].x);
                    top = std::min(top, scene.vertices[v].y); bottom = std::max(bottom, scene.vertices[v].y);
                }
                const bool overlaps = x1 >= 0 && draw.count && right >= x0 && left <= x1 + 1 && bottom >= y0 && top <= y1 + 1;
                // Only the draws over the differing area, and every clear, keep the list short.
                if (x1 >= 0 && !overlaps && draw.kind != SceneDraw::Kind::clear) continue;
                const SceneTexture *texture = draw.texture >= 0 ? scene.textures[draw.texture].get() : nullptr;
                std::fprintf(text, "%zu kind=%d through=%d textured=%d tex=%ux%u alpha=%d linear=%d dither=%d blend=%d mode=%03x "
                    "fix=%06x,%06x scissor=%u,%u,%u,%u count=%u box=%.1f,%.1f..%.1f,%.1f color0=%08x beyond=%d clear=%08x/%x\n",
                    i, static_cast<int>(draw.kind), draw.through, draw.textured, texture ? texture->width : 0u, texture ? texture->height : 0u,
                    draw.texture_alpha, draw.linear, draw.dither, draw.blend, draw.blend_mode, draw.fixed_a, draw.fixed_b,
                    draw.scissor[0], draw.scissor[1], draw.scissor[2], draw.scissor[3], draw.count, left, top, right, bottom,
                    draw.count && draw.first < scene.vertices.size() ? scene.vertices[draw.first].color : 0u, draw.beyond,
                    draw.clear_color, draw.clear_preserve);
            }
            std::fclose(text);
        }
        const auto picture = [&](const std::string &name, auto pixel) {
            std::FILE *file = std::fopen(name.c_str(), "wb");
            if (!file) return;
            std::fprintf(file, "P6\n480 272\n255\n");
            std::vector<std::uint8_t> row(480u * 3u);
            for (int y = 0; y < 272; ++y) {
                for (int x = 0; x < 480; ++x) pixel(x, y, &row[static_cast<std::size_t>(x) * 3u]);
                std::fwrite(row.data(), 1, row.size(), file);
            }
            std::fclose(file);
        };
        if (exact.size() == 480u * 272u)
            picture(base + "-exact.ppm", [&](int x, int y, std::uint8_t *out) {
                const auto value = exact[static_cast<std::size_t>(y) * 480u + x];
                out[0] = static_cast<std::uint8_t>(value >> 16); out[1] = static_cast<std::uint8_t>(value >> 8); out[2] = static_cast<std::uint8_t>(value);
            });
        if (rendered)
            picture(base + "-render.ppm", [&](int x, int y, std::uint8_t *out) {
                const auto *in = &check_pixels[(static_cast<std::size_t>(271 - y) * 480u + x) * 4u];
                out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
            });
    }
    void upload(const std::vector<std::uint32_t> &pixels, std::uint32_t width, std::uint32_t height) {
        if (!picture || width != picture_width || height != picture_height) {
            if (picture) glDeleteTextures(1, &picture);
            glGenTextures(1, &picture);
            glBindTexture(GL_TEXTURE_2D, picture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            picture_width = width; picture_height = height;
        }
        glBindTexture(GL_TEXTURE_2D, picture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    void accept_scene(std::shared_ptr<const Scene> scene, const std::vector<std::uint32_t> &exact, std::uint32_t width, std::uint32_t height) {
        const auto now = Clock::now();
        const bool matches = scene && scene_matches(*scene, exact, width, height);
        scenes.fetch_add(1, std::memory_order_relaxed);
        if (matches) matching_scenes.fetch_add(1, std::memory_order_relaxed);
        pacer.accept(matches ? std::move(scene) : nullptr, now);
    }
    void draw(const ScenePacer::Entry *entry, float t) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, surface_width, surface_height);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        // The SurfaceView keeps the PSP's 480:272 shape; the picture fills it.
        bool use_scene = entry != nullptr;
        if (use_scene) {
            const auto motion = entry->motion.valid ? &entry->motion : nullptr;
            if (build_scene_geometry(vertices, batches, *entry->scene, motion, t, smooth.load()))
                render_batches(*entry->scene, 0, 0, surface_width, surface_height, surface_height);
            else use_scene = false;
        }
        if (!use_scene && picture) {
            glUseProgram(picture_program);
            glBindVertexArray(empty_array);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, picture);
            const auto filter = smooth.load() ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            glViewport(0, 0, surface_width, surface_height);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindVertexArray(0);
    }
    void report(Clock::time_point now) {
        if (now < next_statistics) return;
        if (next_statistics != Clock::time_point{}) {
            const auto elapsed = std::chrono::duration<double>(now - rate_start).count();
            const auto total = std::max<std::uint64_t>(1, scenes.load());
            std::cout << "[android-present] presents_per_second=" << rate_presents / std::max(elapsed, 0.001)
                      << " surface=" << surface_width << "x" << surface_height << " scenes=" << scenes.load()
                      << " matching=" << matching_scenes.load() << " mismatch_pixels_per_scene=" << mismatch_pixels / total
                      << " interpolated=" << interpolated.load() << " presented=" << presented.load()
                      << " interval_ms=" << pacer.interval() * 1000 << " lateness_ms=" << pacer.lateness() * 1000
                      << " holds=" << pacer.holds() << std::endl;
        }
        rate_presents = 0; rate_start = now;
        next_statistics = now + std::chrono::seconds(statistics_log ? 5 : 30);
    }
    void run() noexcept {
        try {
            create_context();
            std::cout << "OpenGL ES scanout: " << reinterpret_cast<const char *>(glGetString(GL_RENDERER))
                      << (interpolate ? ", in-between frames up to the display refresh" : "") << std::endl;
        } catch (const std::exception &error) {
            std::lock_guard lock(mutex); failure = error.what(); failed = true; applied.notify_all(); return;
        }
        std::unique_lock lock(mutex);
        for (;;) {
            // While in-between frames are due, draw on every display refresh;
            // the buffer swap waits for it. Otherwise sleep until something changes.
            const auto drawing = [&] { return pacer.animating(); };
            if (!drawing() || surface == EGL_NO_SURFACE)
                wake.wait(lock, [&] { return stopping || frame_ready || surface_request != surface_applied ||
                                             (drawing() && surface != EGL_NO_SURFACE); });
            // Taken right after the previous swap returned (a display refresh)
            // and before any scene work, so in-between positions stay evenly spaced.
            const auto refresh_time = Clock::now();
            if (stopping) break;
            const bool surface_changed = surface_request != surface_applied;
            ANativeWindow *next = std::exchange(requested_surface, nullptr);
            const auto request = surface_request;
            const bool frame = std::exchange(frame_ready, false);
            const auto stride = pending_stride, format = pending_format, width = pending_width, height = pending_height;
            std::shared_ptr<const Scene> scene;
            if (frame) { raw.swap(pending); scene = std::move(pending_scene); }
            lock.unlock();
            try {
                if (surface_changed) {
                    adopt_window(next);
                    { std::lock_guard applied_lock(mutex); surface_applied = request; }
                    applied.notify_all();
                }
                if (frame) {
                    working = decode_framebuffer(raw, stride, format, width, height);
                    upload(working, width, height);
                    if (interpolate) accept_scene(std::move(scene), working, width, height);
                }
                const auto pick = interpolate ? pacer.pick(refresh_time) : ScenePacer::Pick{};
                if (pick.in_between) interpolated.fetch_add(1, std::memory_order_relaxed);
                if (surface != EGL_NO_SURFACE && (frame || surface_changed || drawing())) {
                    query_size();
                    if (surface_width > 0 && surface_height > 0) {
                        draw(pick.entry, pick.t);
                        if (!eglSwapBuffers(display, surface)) {
                            // The surface went away under us; wait for the app's next one.
                            const auto error = eglGetError();
                            if (error != EGL_BAD_SURFACE && error != EGL_BAD_NATIVE_WINDOW) require(false, "present");
                        }
                        presented.fetch_add(1, std::memory_order_relaxed);
                        ++rate_presents;
                    }
                }
                report(refresh_time);
            } catch (const std::exception &error) {
                release_window();
                lock.lock(); failure = error.what(); failed = true; applied.notify_all(); break;
            }
            lock.lock();
        }
        if (lock.owns_lock()) lock.unlock();
        release_window();
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (idle != EGL_NO_SURFACE) eglDestroySurface(display, idle);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            eglTerminate(display);
        }
    }
};

AndroidPresenter::AndroidPresenter(bool interpolate, bool smooth) : impl_(std::make_unique<Impl>(interpolate, smooth)) {}
AndroidPresenter::~AndroidPresenter() = default;
void AndroidPresenter::set_surface(ANativeWindow *surface) {
    std::unique_lock lock(impl_->mutex);
    if (impl_->failed) { if (surface) ANativeWindow_release(surface); return; }
    if (impl_->requested_surface) ANativeWindow_release(impl_->requested_surface); // Superseded before it was used.
    impl_->requested_surface = surface;
    const auto request = ++impl_->surface_request;
    impl_->wake.notify_all();
    // Android expects the old surface released before surfaceDestroyed returns.
    impl_->applied.wait_for(lock, std::chrono::seconds(2), [&] { return impl_->surface_applied >= request || impl_->failed; });
}
void AndroidPresenter::set_smooth(bool smooth) { impl_->smooth = smooth; }
void AndroidPresenter::submit(std::vector<std::uint8_t> raw, std::uint32_t stride, std::uint32_t format,
                              std::uint32_t width, std::uint32_t height, std::shared_ptr<const Scene> scene) {
    if (impl_->failed || raw.empty() || !width || !height) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->pending = std::move(raw);
        impl_->pending_stride = stride; impl_->pending_format = format;
        impl_->pending_width = width; impl_->pending_height = height;
        impl_->pending_scene = std::move(scene);
        impl_->frame_ready = true;
    }
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    impl_->wake.notify_one();
}
bool AndroidPresenter::interpolating() const noexcept { return impl_->interpolate; }
bool AndroidPresenter::failed() const noexcept { return impl_->failed; }
std::string AndroidPresenter::error() const { std::lock_guard lock(impl_->mutex); return impl_->failure; }
AndroidPresenter::Statistics AndroidPresenter::statistics() const {
    return {impl_->submitted.load(), impl_->presented.load(), impl_->scenes.load(), impl_->matching_scenes.load(),
            impl_->interpolated.load()};
}
}
