#pragma once
#include "badman_scene.hpp"
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace badman {
class Clut;
class GpuRaster;
// Bounded software rendering for8888 clears, through/transformed sprites and colored affine strips.
// Common RGB blend factors/equations share validation across sprites/strips.
// Unsupported transforms/pixel state fail before writing targets.
// The raw depth layout uses the PSP's initial 0x400 EDRAM translation mode.
class SceneRecorder;
class WorkerPool;
class Raster {
public:
    using Matrices = std::array<std::array<std::uint32_t, 128>, 5>;
    explicit Raster(psprecomp::GuestMemory &memory);
    ~Raster();
    void flush();
    void invalidate_textures();
    // A CLUT load changes palettes only; texel memory stays as it was.
    void palette_loaded();
    // Records each frame's draws per target framebuffer for the presenter
    // (in-between frames and higher-resolution rendering). Off by default.
    // beyond: native pixels past the left and right screen edges for which
    // camera sprites the GE culls are recorded too (a wider view); 0 for none.
    void set_scene_recording(bool enabled, double beyond = 0);
    std::shared_ptr<Scene> take_scene(std::uint32_t framebuffer_address, std::int64_t time_ns);
    void set_deferred(bool deferred) { deferred_=deferred; }
    // Helper threads for large software sprite batches (0 = draw on the calling
    // thread only). Results are identical either way.
    void set_threads(unsigned helpers);
    std::string renderer_description() const;
    struct Result {
        std::uint32_t vertices{}, next_vertex_address{};
        std::uint64_t color_pixels{}, depth_pixels{};
    };
    Result draw(std::uint32_t command, const std::array<std::uint32_t, 256> &registers,
                std::uint32_t vertex_address, const Clut *palette = nullptr,
                const Matrices *matrices = nullptr);
private:
    psprecomp::GuestMemory &memory_;
    std::unique_ptr<GpuRaster> gpu_;
    std::unique_ptr<SceneRecorder> recorder_;
    std::unique_ptr<WorkerPool> workers_;
    // Development statistics (BADMAN_RASTER_STATS): draws, pixels and time per primitive kind.
    struct Statistics;
    std::unique_ptr<Statistics> statistics_;
    bool deferred_{};
    Result draw_impl(std::uint32_t command,const std::array<std::uint32_t,256>& registers,
                     std::uint32_t vertex_address,const Clut *palette,const Matrices *matrices);
};
}
