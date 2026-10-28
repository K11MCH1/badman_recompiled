#pragma once
#include "badman_gpu_device.hpp"
#include "badman_texture.hpp"
#include <array>
#include <map>
#include <optional>
#include <vector>

namespace badman {
struct GpuSprite {
    std::int32_t ax{}, ay{}, bx{}, by{};
    std::uint32_t x0{}, y0{}, x1{}, y1{}, color{};
    float u0{}, v0{}, u1{}, v1{};
    bool minifying{}, rotated{};
};
struct GpuTransform {
    std::array<std::array<double,4>,4> combined{};
    std::array<double,4> scales{},centers{};
};
using GpuTransformKey=std::array<std::uint32_t,46>;
class GpuRaster {
public:
    bool cached_transform(const GpuTransformKey &key,GpuTransform &result) const noexcept;
    void cache_transform(const GpuTransformKey &key,const GpuTransform &result) noexcept;
    explicit GpuRaster(psprecomp::GuestMemory &memory, std::string_view renderer);
    bool enqueue(std::span<const GpuSprite> sprites,
                 const std::array<std::uint32_t,256> &registers,
                 const Texture *texture);
    void flush();
    void invalidate_textures();
    std::string description() const { return device_->description(); }
private:
    struct TransformEntry {GpuTransformKey key{};GpuTransform value{};};
    std::optional<TransformEntry> transform_cache_;
    psprecomp::GuestMemory &memory_;
    std::unique_ptr<GpuDevice> device_;
    std::vector<std::uint32_t> data_, packets_;
    // Reused per flush: the transaction input and the whole-VRAM state copy.
    std::vector<std::uint32_t> input_, state_;
    std::array<std::vector<std::uint32_t>,4096> bins_;
    std::map<std::array<std::uint32_t,12>,std::uint32_t> textures_;
    std::uint32_t target_{}, stride_{}, height_{}, packet_count_{};
    bool diagnostics_{};
    std::uint64_t batches_{},submitted_sprites_{},uploaded_words_{},transaction_ns_{},next_report_ns_{};
    std::uint32_t texture_data(const Texture &texture,
                              const std::array<std::uint32_t,256> &registers);
};
}
