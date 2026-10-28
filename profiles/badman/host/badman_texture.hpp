#pragma once
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace badman {
class Clut;
// Level-zero direct-color and T4/T8 PSP texture sampling. Coordinates are texels, with
// centers at n+0.5 for linear filtering; output is PSP packed AABBGGRR.
// Logical wrap/clamp dimensions are independent of TBW row pitch. A smaller
// pitch deliberately aliases rows/tile bands; no padded texels are synthesized.
class Texture {
public:
    Texture(const psprecomp::GuestMemory &memory, const std::array<std::uint32_t,256> &commands,
            const Clut *palette = nullptr);
    std::uint32_t sample(float u, float v) const { return sample(u,v,false); }
    std::uint32_t sample(float u, float v, bool minifying) const;
    // The same sample from coordinates already in 1/16-texel units (fixed_u/fixed_v),
    // for callers that reuse them across a row or column.
    std::uint32_t sample_fixed(int x, int y, bool minifying) const;
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t format() const { return format_; }
    struct GpuSnapshot {
        std::array<std::uint32_t,6> metadata{};
        std::vector<std::uint32_t> words;
    };
    // A missing possible palette entry keeps this draw on the scalar path,
    // which validates only the entries actually sampled.
    std::optional<GpuSnapshot> gpu_snapshot() const;
    // Scene recording: identity of the texture's memory layout, hashes of its
    // current texel bytes and palette entries, and every texel decoded (PSP ABGR).
    std::uint64_t layout_key() const;
    std::uint64_t texel_hash() const;
    std::uint64_t palette_hash() const;
    const Clut *palette() const { return format_>=4u?palette_:nullptr; }
    std::uint32_t palette_format() const { return palette_format_; }
    std::vector<std::uint32_t> decode() const;
    bool linear_magnification() const { return linear_mag_; }
    int fixed_u(float value) const { return position(value,width_,clamp_u_); }
    int fixed_v(float value) const { return position(value,height_,clamp_v_); }
private:
    const psprecomp::GuestMemory &memory_;
    const Clut *palette_{};
    const std::uint8_t *contiguous_{}; // Non-owning, live backing; null for physical EDRAM wrap.
    std::uint32_t palette_format_{};
    std::uint32_t address_{},width_{},height_{},row_bytes_{},format_{},bytes_per_pixel_{};
    std::size_t footprint_{};
    bool swizzled_{},clamp_u_{},clamp_v_{},linear_min_{},linear_mag_{};
    std::uint32_t texel(int x, int y) const;
    // Byte offset of texel (x, y) after wrapping or clamping both in place.
    std::uint32_t texel_offset(int &x, int &y) const;
    // The palette entry a T4/T8 texel at a wrapped x and its offset selects.
    std::uint32_t palette_index(int x, std::uint32_t offset) const;
    static int index(int coordinate, std::uint32_t extent, bool clamp);
    static int position(float coordinate, std::uint32_t extent, bool clamp);
};
}
