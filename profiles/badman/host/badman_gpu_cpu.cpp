// CPU executor of the GPU raster's pixel program (badman_gpu_shader.hlsl):
// the same batched sprite packets, tile bins and fixed-point coordinates, run
// on helper threads by rows. It follows the shader line for line, so the
// D3D11, Vulkan and CPU backends share one validated pixel pipeline; every
// pixel writes only its own word, so rows run independently.
#include "badman_gpu_device.hpp"
#include "badman_workers.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace badman {
namespace {
using Words = std::span<const std::uint32_t>;
std::uint32_t byte_at(Words in, std::uint32_t base, std::uint32_t address) {
    return (in[base + address / 4] >> ((address & 3u) * 8u)) & 255u;
}
int floor16(int x) { return x / 16 - ((x < 0 && x % 16 != 0) ? 1 : 0); }
int wrap_index(int x, std::uint32_t size, bool clamp_mode) {
    if (clamp_mode) return std::clamp(x, 0, static_cast<int>(size) - 1);
    const int n = x % static_cast<int>(size);
    return n < 0 ? n + static_cast<int>(size) : n;
}
std::uint32_t expand5(std::uint32_t x) { return (x << 3) | (x >> 2); }
std::uint32_t expand6(std::uint32_t x) { return (x << 2) | (x >> 4); }
// A texture's six metadata words, read once per packet instead of per texel.
struct TextureView {
    std::uint32_t width, height, pitch, format, flags, length, base;
    TextureView(Words in, std::uint32_t t)
        : width(in[t]), height(in[t + 1]), pitch(in[t + 2]), format(in[t + 3]), flags(in[t + 4]), length(in[t + 5]), base(t + 6) {}
};
std::uint32_t texel(Words in, const TextureView &t, int x, int y) {
    const auto format = t.format, flags = t.flags, pitch = t.pitch, base = t.base;
    x = wrap_index(x, t.width, (flags & 2u) != 0); y = wrap_index(y, t.height, (flags & 4u) != 0);
    const std::uint32_t bpp = format == 3 ? 4 : (format >= 4 ? 1 : 2);
    const std::uint32_t bx = format == 4 ? static_cast<std::uint32_t>(x) / 2 : static_cast<std::uint32_t>(x) * bpp;
    std::uint32_t offset = static_cast<std::uint32_t>(y) * pitch + bx;
    if (flags & 1u) offset = (static_cast<std::uint32_t>(y) / 8) * (pitch * 8) + (bx / 16) * 128 + (static_cast<std::uint32_t>(y) % 8) * 16 + (bx % 16);
    const auto a = byte_at(in, base, offset);
    if (format >= 4) {
        const auto index = format == 4 ? ((a >> ((static_cast<std::uint32_t>(x) & 1u) * 4)) & 15u) : a;
        return in[base + t.length + index];
    }
    const auto b = byte_at(in, base, offset + 1);
    const auto word = a | (b << 8);
    if (format == 3) return word | (byte_at(in, base, offset + 2) << 16) | (byte_at(in, base, offset + 3) << 24);
    if (format == 0) return 0xFF000000u | expand5(word & 31u) | (expand6((word >> 5) & 63u) << 8) | (expand5((word >> 11) & 31u) << 16);
    if (format == 1) return ((word & 0x8000u) ? 0xFF000000u : 0u) | expand5(word & 31u) | (expand5((word >> 5) & 31u) << 8) | (expand5((word >> 10) & 31u) << 16);
    return ((word & 15u) * 17u) | (((word >> 4) & 15u) * 17u << 8) | (((word >> 8) & 15u) * 17u << 16) | (((word >> 12) & 15u) * 17u << 24);
}
std::uint32_t interpolate(std::uint32_t a, std::uint32_t b, std::uint32_t fraction) {
    std::uint32_t value = 0;
    for (std::uint32_t shift = 0; shift < 32; shift += 8)
        value |= ((((a >> shift) & 255u) * (16 - fraction) + ((b >> shift) & 255u) * fraction) >> 4) << shift;
    return value;
}
std::uint32_t sample_texture(Words in, const TextureView &t, int u, int v, bool minifying) {
    if (!(t.flags & (minifying ? 8u : 16u))) return texel(in, t, floor16(u), floor16(v));
    const int left = floor16(u - 8), top = floor16(v - 8);
    const auto fx = static_cast<std::uint32_t>(u - 8 - left * 16), fy = static_cast<std::uint32_t>(v - 8 - top * 16);
    return interpolate(interpolate(texel(in, t, left, top), texel(in, t, left + 1, top), fx),
                       interpolate(texel(in, t, left, top + 1), texel(in, t, left + 1, top + 1), fx), fy);
}
std::uint32_t modulate(std::uint32_t tex, std::uint32_t color, bool alpha) {
    std::uint32_t result = 0;
    for (std::uint32_t shift = 0; shift < 24; shift += 8)
        result |= ((((tex >> shift) & 255u) * (((color >> shift) & 255u) + 1)) >> 8) << shift;
    return result | ((alpha ? (((color >> 24) + 1) * (tex >> 24) >> 8) : (color >> 24)) << 24);
}
int factor(std::uint32_t code, int other, std::uint32_t sa, std::uint32_t da, std::uint32_t fixed_value) {
    if (code == 0) return other;
    if (code == 1) return 255 - other;
    if (code == 2) return static_cast<int>(sa);
    if (code == 3) return 255 - static_cast<int>(sa);
    if (code == 4) return static_cast<int>(da);
    if (code == 5) return 255 - static_cast<int>(da);
    return static_cast<int>(fixed_value);
}
std::uint32_t blend(std::uint32_t src, std::uint32_t dst, std::uint32_t mode, std::uint32_t fixed_a, std::uint32_t fixed_b) {
    std::uint32_t output = dst & 0xFF000000u;
    const auto equation = (mode >> 8) & 7u;
    for (std::uint32_t shift = 0; shift < 24; shift += 8) {
        const int s = static_cast<int>((src >> shift) & 255u), d = static_cast<int>((dst >> shift) & 255u);
        int value;
        if (equation == 3) value = std::min(s, d);
        else if (equation == 4) value = std::max(s, d);
        else if (equation == 5) value = std::abs(s - d);
        else {
            const int a = ((2 * s + 1) * (2 * factor(mode & 15u, d, src >> 24, dst >> 24, (fixed_a >> shift) & 255u) + 1)) >> 10;
            const int b = ((2 * d + 1) * (2 * factor((mode >> 4) & 15u, s, src >> 24, dst >> 24, (fixed_b >> shift) & 255u) + 1)) >> 10;
            value = equation == 0 ? a + b : (equation == 1 ? a - b : b - a);
        }
        output |= static_cast<std::uint32_t>(std::clamp(value, 0, 255)) << shift;
    }
    return output;
}
// One sprite packet's twenty words, decoded once per tile.
struct Packet {
    std::uint32_t x0, y0, x1, y1, color, preserve, mode, fixed_a, fixed_b, flags, u, v;
    std::uint32_t dither[4];
    std::optional<TextureView> texture;
    Packet(Words in, std::uint32_t p)
        : x0(in[p]), y0(in[p + 1]), x1(in[p + 2]), y1(in[p + 3]), color(in[p + 4]), preserve(in[p + 5]),
          mode(in[p + 6]), fixed_a(in[p + 7]), fixed_b(in[p + 8]), flags(in[p + 9]), u(in[p + 11]), v(in[p + 12]),
          dither{in[p + 13], in[p + 14], in[p + 15], in[p + 16]} {
        if (flags & 2u) texture.emplace(in, in[p + 10]);
    }
};
// The shader's main() loop body: one packet applied to the pixel (x, y).
std::uint32_t apply(Words in, const Packet &k, std::uint32_t x, std::uint32_t y, std::uint32_t destination) {
    auto color = k.color;
    if (k.flags & 2u) {
        const int u = static_cast<int>(in[k.u + ((k.flags & 16u) ? y - k.y0 : x - k.x0)]);
        const int v = static_cast<int>(in[k.v + ((k.flags & 16u) ? x - k.x0 : y - k.y0)]);
        color = modulate(sample_texture(in, *k.texture, u, v, (k.flags & 32u) != 0), color, (k.flags & 4u) != 0);
    }
    if (k.flags & 1u) color = blend(color, destination, k.mode, k.fixed_a, k.fixed_b);
    if (k.flags & 8u) {
        const auto nibble = (k.dither[y & 3u] >> ((x & 3u) * 4)) & 15u;
        const int delta = static_cast<int>(nibble) - (nibble >= 8 ? 16 : 0);
        std::uint32_t adjusted = color & 0xFF000000u;
        for (std::uint32_t shift = 0; shift < 24; shift += 8)
            adjusted |= static_cast<std::uint32_t>(std::clamp(static_cast<int>((color >> shift) & 255u) + delta, 0, 255)) << shift;
        color = adjusted;
    }
    return (destination & k.preserve) | (color & ~k.preserve);
}
// Destination words: the separate state buffer, or guest VRAM bytes in place.
struct WordState {
    std::span<std::uint32_t> words;
    std::uint32_t load(std::uint32_t index) const { return words[index]; }
    void store(std::uint32_t index, std::uint32_t value) const { words[index] = value; }
};
struct ByteState {
    std::uint8_t *bytes;
    std::uint32_t load(std::uint32_t index) const {
        std::uint32_t value; std::memcpy(&value, bytes + std::size_t{index} * 4, 4); return value; // Little-endian host.
    }
    void store(std::uint32_t index, std::uint32_t value) const { std::memcpy(bytes + std::size_t{index} * 4, &value, 4); }
};
// One 16x16 tile. The shader runs per pixel over the tile's bin and skips
// packets that do not cover it; here each packet visits only the pixels it
// covers, in bin order, so every pixel sees the same packets in the same order.
template<class State>
void tile(Words in, const State &state, std::uint32_t tx, std::uint32_t ty, std::uint32_t stride, std::uint32_t height) {
    const auto bin = in[3] + (ty * 64 + tx) * 2;
    const auto indices = in[bin], count = in[bin + 1];
    if (!count) return;
    const auto left = tx * 16, top = ty * 16;
    const auto right = std::min(stride, left + 16), bottom = std::min(height, top + 16);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Packet packet(in, in[4] + in[indices + i] * 20);
        const auto x0 = std::max(left, packet.x0), y0 = std::max(top, packet.y0);
        const auto x1 = std::min(right, packet.x1), y1 = std::min(bottom, packet.y1);
        for (auto y = y0; y < y1; ++y)
            for (auto x = x0; x < x1; ++x) {
                const auto address = in[0] + y * stride + x;
                if (address < 524288u) state.store(address, apply(in, packet, x, y, state.load(address)));
            }
    }
}

class CpuCompute final : public GpuDevice {
public:
    explicit CpuCompute(unsigned helpers) : pool_(helpers) {}
    std::string description() const override {
        return "CPU tiles (" + std::to_string(pool_.helpers() + 1) + " threads)";
    }
    void dispatch(std::span<const std::uint32_t> input, std::span<std::uint32_t> state, std::uint32_t) override {
        run(input, WordState{state});
    }
    // Pure integer work on validated packets cannot fail midway, so it may
    // write guest VRAM directly.
    bool dispatch_in_place(std::span<const std::uint32_t> input, std::span<std::uint8_t> vram) override {
        if (vram.size() < 524288u * 4) return false;
        run(input, ByteState{vram.data()});
        return true;
    }
private:
    template<class State> void run(Words input, const State &state) {
        const auto stride = input[1], height = input[2];
        if (!stride || !height) return;
        // Tile rows on the helpers; tiles never share a pixel. A small batch
        // (strips interleave many while digging) is cheaper on this thread
        // than waking every helper for it.
        const auto tile_rows = (height + 15) / 16, tile_columns = (stride + 15) / 16;
        if (input[5] <= 4) {
            for (std::uint32_t ty = 0; ty < tile_rows; ++ty)
                for (std::uint32_t tx = 0; tx < tile_columns; ++tx) tile(input, state, tx, ty, stride, height);
            return;
        }
        pool_.run(tile_rows, [&](std::size_t ty) {
            for (std::uint32_t tx = 0; tx < tile_columns; ++tx)
                tile(input, state, tx, static_cast<std::uint32_t>(ty), stride, height);
        });
    }
    WorkerPool pool_;
};
}
std::unique_ptr<GpuDevice> make_cpu_compute(unsigned helpers) { return std::make_unique<CpuCompute>(helpers); }
}
