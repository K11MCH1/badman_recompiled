#include "badman_raster.hpp"
#include "badman_texture.hpp"
#include "badman_clut.hpp"
#include "badman_gpu_raster.hpp"
#include "badman_workers.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <iostream>
#include <cfenv>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace badman {
namespace {
constexpr std::uint32_t vram_base = psprecomp::GuestMemory::kVramPhysicalBase;
constexpr std::uint32_t vram_size = psprecomp::GuestMemory::kVramSize;
constexpr std::uint32_t vertex_stride = 12;
constexpr std::uint64_t pixel_budget = 8u * 1024u * 1024u;
std::uint32_t data(std::uint32_t word) { return word & 0x00FFFFFFu; }
std::uint32_t rgba4444(std::uint16_t packed) {
    std::uint32_t result=0;
    for(unsigned channel=0;channel<4;++channel)
        result|=(((packed>>(channel*4))&15u)*17u)<<(channel*8);
    return result;
}
std::uint32_t buffer_offset(std::uint32_t low, std::uint32_t high) {
    auto address = data(low) | ((high & 0x00FF0000u) << 8);
    address = psprecomp::GuestMemory::canonical(address);
    if (address >= vram_base && address < vram_base + vram_size) address -= vram_base;
    if (address >= vram_size || (address & 0x1FFFu))
        throw psprecomp::Error("Unsupported clear buffer address/alignment");
    return address;
}
std::uint32_t depth_offset(std::uint32_t linear) {
    // Hardware T=0x400, 32-bit color: rotate address bits 5..9 left once,
    // then exchange the documented 8-KiB and 64-byte banks. This permutation
    // applies to the absolute VRAM byte offset, not to a buffer-relative one.
    const auto permuted = (linear & ~0x3E0u) | ((linear & 0x1E0u) << 1) | ((linear & 0x200u) >> 4);
    return permuted ^ 0x2040u;
}
struct Rectangle {
    std::uint32_t x0{}, y0{}, x1{}, y1{}, color{};
    std::uint16_t depth{};
};
using Sprite=GpuSprite;
std::uint32_t modulate(std::uint32_t texel, std::uint32_t color, bool texture_alpha) {
    std::uint32_t result = 0;
    for (std::uint32_t shift = 0; shift < 24; shift += 8)
        result |= (((texel >> shift) & 255u) * (((color >> shift) & 255u) + 1u) >> 8) << shift;
    const auto alpha = texture_alpha ? (((color >> 24) + 1u) * (texel >> 24) >> 8) : color >> 24;
    return result | (alpha << 24);
}
class Blend {
public:
    explicit Blend(const std::array<std::uint32_t,256> &registers)
        : enabled_(data(registers[0x21])!=0), mode_(data(registers[0xDF])),
          fixed_a_(data(registers[0xE0])), fixed_b_(data(registers[0xE1])) {
        if (!enabled_) return;
        const auto equation=(mode_>>8)&7u;
        if ((mode_&~0x7FFu) || equation>5) throw psprecomp::Error("Unsupported GE blend equation/reserved mode bits");
        // MIN/MAX/ABS use raw RGB, so their otherwise unused factor fields
        // must not reject a draw. Doubled-alpha factors await signed rounding.
        if (equation<3 && (unsupported_factor(mode_&15u) || unsupported_factor((mode_>>4)&15u)))
            throw psprecomp::Error("GE doubled-alpha blend factors 6..9 are not implemented");
    }
    std::uint32_t apply(std::uint32_t source,std::uint32_t destination) const {
        if (!enabled_) return source;
        auto result=destination&0xFF000000u; // Alpha is stencil; stencil writes are disabled in this subset.
        const auto equation=(mode_>>8)&7u;
        for (unsigned shift=0;shift<24;shift+=8) {
            const auto src=static_cast<int>((source>>shift)&255u), dst=static_cast<int>((destination>>shift)&255u);
            int value;
            if (equation==3) value=std::min(src,dst);
            else if (equation==4) value=std::max(src,dst);
            else if (equation==5) value=std::abs(src-dst);
            else {
                const auto a=product(src,factor(mode_&15u,dst,source>>24,destination>>24,(fixed_a_>>shift)&255u));
                const auto b=product(dst,factor((mode_>>4)&15u,src,source>>24,destination>>24,(fixed_b_>>shift)&255u));
                value=equation==0?a+b:equation==1?a-b:b-a;
            }
            result|=static_cast<std::uint32_t>(std::clamp(value,0,255))<<shift;
        }
        return result;
    }
private:
    static bool unsupported_factor(unsigned factor) { return factor>=6 && factor<=9; }
    static int factor(unsigned code,int other,unsigned source_alpha,unsigned destination_alpha,unsigned fixed) {
        switch (code) {
        case 0:return other;
        case 1:return 255-other;
        case 2:return static_cast<int>(source_alpha);
        case 3:return 255-static_cast<int>(source_alpha);
        case 4:return static_cast<int>(destination_alpha);
        case 5:return 255-static_cast<int>(destination_alpha);
        default:return static_cast<int>(fixed); // Hardware codes10..15 alias FIX.
        }
    }
    static int product(int color,int factor) {
        // Each measured GE term truncates independently. Clamp only after
        // applying the ADD/SUBTRACT/REVERSE_SUBTRACT equation to those terms.
        return ((2*color+1)*(2*factor+1))>>10;
    }
    bool enabled_;
    std::uint32_t mode_,fixed_a_,fixed_b_;
};
std::uint32_t dither(std::uint32_t rgba, const std::array<std::uint32_t, 256> &registers,
                     std::uint32_t x, std::uint32_t y) {
    if (!(registers[0x20] & 1u)) return rgba;
    const auto nibble = (registers[0xE2 + (y & 3u)] >> ((x & 3u) * 4)) & 15u;
    const auto adjustment = static_cast<int>(nibble) - (nibble >= 8u ? 16 : 0);
    auto result = rgba & 0xFF000000u;
    for (std::uint32_t shift = 0; shift < 24; shift += 8) {
        const auto channel = static_cast<int>((rgba >> shift) & 255u);
        result |= static_cast<std::uint32_t>(std::clamp(channel + adjustment, 0, 255)) << shift;
    }
    return result;
}
double quantize(double value, int bit) {
    // In this normal finite range both power-of-two scalings are exact.
    // Clearing the discarded fraction bits is truncation toward zero.
    const auto word = std::bit_cast<std::uint64_t>(value);
    const auto exponent = static_cast<unsigned>((word >> 52) & 0x7ffu);
    const auto retained = static_cast<std::int64_t>(exponent) - 1023 - bit;
    if (exponent != 0 && exponent != 0x7ffu && retained >= 0 && retained < 52) {
        const auto discarded = static_cast<unsigned>(52 - retained);
        return std::bit_cast<double>(word & ~((std::uint64_t{1} << discarded) - 1));
    }
    return std::ldexp(std::trunc(std::ldexp(value, -bit)), bit);
}
int normal_exponent(double value) {
    const auto exponent=static_cast<unsigned>((std::bit_cast<std::uint64_t>(value)>>52u)&0x7FFu);
    if(exponent!=0&&exponent!=0x7FFu)return static_cast<int>(exponent)-1023;
    // Preserve the CRT result/error/exception behavior for zero, subnormals,
    // infinities and NaNs. No arithmetic or call ordering is otherwise changed.
    return std::ilogb(std::abs(value));
}

double float24(double value) {
    if (!std::isfinite(value)) throw psprecomp::Error("Nonfinite transformed vertex arithmetic");
    if (value == 0 || std::abs(value) < std::numeric_limits<float>::min()) return 0;
    if (std::abs(value) > std::numeric_limits<float>::max())
        throw psprecomp::Error("Overflowed transformed vertex arithmetic");
    return quantize(value, normal_exponent(value) - 15);
}
double ge_add(double a, double b) {
    if (a == 0) return b;
    if (b == 0) return a;
    const auto bit = std::max(normal_exponent(a), normal_exponent(b)) - 15;
    return quantize(a, bit) + quantize(b, bit);
}
using Vector = std::array<double, 4>;
using Matrix = std::array<Vector, 4>; // rows, independently decoded from GE column-major storage
GpuTransformKey transform_key(const Raster::Matrices &matrices,const std::array<std::uint32_t,256> &registers) {
    GpuTransformKey key{};unsigned at=0;
    for(unsigned i=0;i<12;++i)key[at++]=matrices[1][i];
    for(unsigned i=0;i<12;++i)key[at++]=matrices[2][i];
    for(unsigned i=0;i<16;++i)key[at++]=matrices[3][i];
    for(unsigned i=0;i<6;++i)key[at++]=registers[0x42+i];
    return key;
}
double row_sum(const Vector &a, const Vector &b) {
    int bit = std::numeric_limits<int>::min();
    for (unsigned i = 0; i < 4; ++i)
        if (a[i] != 0 && b[i] != 0)
            bit = std::max(bit, normal_exponent(a[i]) + normal_exponent(b[i]) - 15);
    if (bit == std::numeric_limits<int>::min()) return 0;
    double result = 0;
    for (unsigned i = 0; i < 4; ++i) result += quantize(a[i] * b[i], bit);
    return float24(result);
}
Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix result{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            result[row][col] = row_sum(a[row], {b[0][col], b[1][col], b[2][col], b[3][col]});
    return result;
}
struct StripVertex {
    std::int32_t x{}, y{};
    Vector position{}, clip{};
    double u{}, v{};
    double depth{};
    std::uint32_t color{};
    bool outside_guard{};
};
std::int64_t edge(const StripVertex &a, const StripVertex &b, std::int32_t x, std::int32_t y) {
    return static_cast<std::int64_t>(b.x - a.x) * (y - a.y) -
           static_cast<std::int64_t>(b.y - a.y) * (x - a.x);
}
bool top_left(const StripVertex &a, const StripVertex &b) {
    return b.y < a.y || (b.y == a.y && b.x > a.x);
}
} // namespace
// Copies each frame's draws into a host-owned Scene, per target framebuffer.
// Recording reads only values the exact rasterizer already decoded; it never
// changes what is drawn into guest memory.
class SceneRecorder {
public:
    struct Building {
        std::shared_ptr<Scene> scene{std::make_shared<Scene>()};
        std::unordered_map<std::uint64_t, std::int32_t> textures, transforms;
    };
    Building &target(const std::array<std::uint32_t, 256> &registers) {
        const auto offset = buffer_offset(registers[0x9C], registers[0x9D]) & 0x1FFFFFu;
        auto &building = building_[offset];
        building.scene->target = offset;
        return building;
    }
    std::shared_ptr<Scene> take(std::uint32_t framebuffer_address, std::int64_t time_ns) {
        const auto found = building_.find(framebuffer_address & 0x1FFFFFu);
        texel_hashes_.clear(); // Texel memory is hashed again at least once per frame.
        if (found == building_.end()) return nullptr;
        auto scene = std::move(found->second.scene);
        scene->time_ns = time_ns;
        building_.erase(found);
        return scene;
    }
    void invalidate_textures() { texel_hashes_.clear(); for (auto &[offset, building] : building_) building.textures.clear(); }
    // Native pixels past the left and right screen edges for which culled
    // camera sprites are still recorded (for a wider view); 0 records none.
    double beyond{};
    void palette_loaded() { for (auto &[offset, building] : building_) building.textures.clear(); }
    static SceneDraw state(const std::array<std::uint32_t, 256> &registers, SceneDraw::Kind kind, bool through,
                           std::int32_t clip_x0, std::int32_t clip_y0, std::int32_t clip_x1, std::int32_t clip_y1) {
        SceneDraw draw;
        draw.kind = kind; draw.through = through;
        draw.textured = data(registers[0x1E]) != 0;
        draw.texture_alpha = data(registers[0xC9]) == 0x100;
        draw.dither = (registers[0x20] & 1u) != 0;
        draw.blend = data(registers[0x21]) != 0;
        draw.blend_mode = data(registers[0xDF]); draw.fixed_a = data(registers[0xE0]); draw.fixed_b = data(registers[0xE1]);
        draw.scissor = {static_cast<std::uint16_t>(std::max(0, clip_x0)), static_cast<std::uint16_t>(std::max(0, clip_y0)),
                        static_cast<std::uint16_t>(std::max(0, clip_x1)), static_cast<std::uint16_t>(std::max(0, clip_y1))};
        return draw;
    }
    std::int32_t texture(Building &building, const Texture &texture) {
        const auto layout = texture.layout_key();
        std::uint64_t palette = 0;
        if (const auto *clut = texture.palette()) {
            const auto memo = (clut->generation() << 33) ^ (std::uint64_t(texture.palette_format()) << 1) ^ (texture.format() == 4u);
            auto found = palettes_.find(memo);
            if (found == palettes_.end()) {
                if (palettes_.size() > 4096) palettes_.clear();
                found = palettes_.emplace(memo, texture.palette_hash()).first;
            }
            palette = found->second;
        }
        const auto memo = layout ^ (palette * 0x9E3779B97F4A7C15ull);
        if (const auto found = building.textures.find(memo); found != building.textures.end()) return found->second;
        auto texels = texel_hashes_.find(layout);
        if (texels == texel_hashes_.end()) texels = texel_hashes_.emplace(layout, texture.texel_hash()).first;
        const auto content = texels->second ^ (palette * 0xC2B2AE3D27D4EB4Full);
        if (cache_.size() > 1024 && !cache_.contains(content)) cache_.clear();
        auto &cached = cache_[content];
        if (!cached) {
            auto made = std::make_shared<SceneTexture>();
            made->key = content; made->width = texture.width(); made->height = texture.height();
            made->texels = texture.decode();
            cached = std::move(made);
        }
        building.scene->textures.push_back(cached);
        const auto index = static_cast<std::int32_t>(building.scene->textures.size() - 1);
        building.textures.emplace(memo, index);
        return index;
    }
    static Matrix decode(const Raster::Matrices &matrices, unsigned index, unsigned rows) {
        Matrix result{}; result[3][3] = 1;
        for (unsigned col = 0; col < 4; ++col)
            for (unsigned row = 0; row < rows; ++row)
                result[row][col] = float24(std::bit_cast<float>(matrices[index][col * rows + row]));
        return result;
    }
    std::int32_t transform(Building &building, const Raster::Matrices &matrices, const std::array<std::uint32_t, 256> &registers) {
        std::uint64_t key = 0xCBF29CE484222325ull;
        const auto add = [&](std::uint32_t word) { key = (key ^ word) * 0x100000001B3ull; };
        for (unsigned i = 0; i < 12; ++i) add(matrices[2][i]);
        for (unsigned i = 0; i < 16; ++i) add(matrices[3][i]);
        for (unsigned id : {0x42u, 0x43u, 0x45u, 0x46u, 0x4Cu, 0x4Du}) add(registers[id]);
        if (const auto found = building.transforms.find(key); found != building.transforms.end()) return found->second;
        const auto reg_float = [&](unsigned id) { return float24(std::bit_cast<float>(data(registers[id]) << 8)); };
        SceneTransform transform;
        transform.view = decode(matrices, 2, 3); transform.projection = decode(matrices, 3, 4);
        transform.scale = {reg_float(0x42), reg_float(0x43)}; transform.center = {reg_float(0x45), reg_float(0x46)};
        transform.offset = {(registers[0x4C] & 65535u) / 16.0, (registers[0x4D] & 65535u) / 16.0};
        building.scene->transforms.push_back(transform);
        const auto index = static_cast<std::int32_t>(building.scene->transforms.size() - 1);
        building.transforms.emplace(key, index);
        return index;
    }
    static std::array<double, 3> world(const Matrix &matrix, const Vector &position) {
        std::array<double, 3> result{};
        for (unsigned row = 0; row < 3; ++row)
            result[row] = matrix[row][0] * position[0] + matrix[row][1] * position[1] + matrix[row][2] * position[2] + matrix[row][3];
        return result;
    }
    // Animation frames of one creature often sit at different texture
    // addresses; pairing must not depend on which frame is shown.
    static std::uint64_t texture_shape(const Texture &texture) {
        return (std::uint64_t(texture.width()) << 32) | (std::uint64_t(texture.height()) << 8) | texture.format() | 0x80000000ull;
    }
    static std::uint64_t material(SceneDraw::Kind kind, bool through, std::uint64_t texture, std::uint32_t blend_mode,
                                  std::int64_t width, std::int64_t height, std::uint32_t vertices) {
        std::uint64_t key = 0x84222325CBF29CE4ull;
        for (const std::uint64_t value : {std::uint64_t(kind), std::uint64_t(through), texture, std::uint64_t(blend_mode),
                                          std::uint64_t(width), std::uint64_t(height), std::uint64_t(vertices)})
            key = (key ^ value) * 0x100000001B3ull;
        return key;
    }
private:
    std::unordered_map<std::uint32_t, Building> building_;
    std::unordered_map<std::uint64_t, std::shared_ptr<const SceneTexture>> cache_;
    std::unordered_map<std::uint64_t, std::uint64_t> texel_hashes_, palettes_;
};
namespace {
Raster::Result draw_strip(psprecomp::GuestMemory &memory, std::uint32_t count,
                          const std::array<std::uint32_t, 256> &registers,
                          std::uint32_t vertex_address, const Clut *palette,
                          const Raster::Matrices *matrices,GpuRaster *gpu,SceneRecorder *recorder,
                          WorkerPool *workers) {
    const auto type=data(registers[0x12]);
    const bool packed_color=type==0x198;
    const bool vertex_color=type==0x19F || packed_color;
    const bool textured=data(registers[0x1E])!=0;
    if (!matrices || (!vertex_color && data(registers[0x12]) != 0x1E3) || data(registers[0xD2]) != 3 ||
        data(registers[0xD3]) != 0 || count < 3 || count > 1024)
        throw psprecomp::Error("Unsupported transformed strip matrices/type/format/count/clear state");
    for (const auto state : {0x15, 0x17, 0x1D, 0x1F, 0x22, 0x23, 0x24, 0x25, 0x27, 0x28})
        if (data(registers[state])) throw psprecomp::Error("Unsupported enabled transformed strip state");
    if (data(registers[0x1C]) != 1 || data(registers[0x1E]) > 1 || (textured && packed_color) || data(registers[0x20]) > 1 ||
        data(registers[0x21]) > 1 || data(registers[0xE7]) > 1 || (textured && data(registers[0xC0]) != 0) ||
        (data(registers[0xE9]) & ~255u) || (data(registers[0x58]) & ~255u) ||
        (data(registers[0x4C]) & ~65535u) || (data(registers[0x4D]) & ~65535u))
        throw psprecomp::Error("Unsupported transformed strip enables/UV/mask/offset state");
    if (vertex_color && data(registers[0x50])>1) throw psprecomp::Error("Unsupported transformed strip shade mode");
    const Blend blend(registers);
    const auto texture_function = data(registers[0xC9]);
    if (textured && texture_function != 0 && texture_function != 0x100)
        throw psprecomp::Error("Unsupported transformed strip texture function");
    std::optional<Texture> texture;
    if(textured){
        const auto texture_address = psprecomp::GuestMemory::canonical(
            data(registers[0xA0]) | ((registers[0xA8] & 0x00FF0000u) << 8));
        if (texture_address < psprecomp::GuestMemory::kPhysicalBase)
            throw psprecomp::Error("VRAM/self-textured transformed strips are not implemented");
        texture.emplace(memory, registers, palette);
    }
    const std::uint32_t stride=packed_color?16u:vertex_color?24u:32u;
    const std::uint32_t position_offset=packed_color?4u:vertex_color?12u:20u;
    const auto primary = data(registers[0x55]) | ((registers[0x58] & 255u) << 24);
    const auto bytes = count * stride;
    if ((vertex_address & 3u) || bytes > std::numeric_limits<std::uint32_t>::max() - vertex_address ||
        !memory.contains(vertex_address, bytes))
        throw psprecomp::Error("Invalid transformed strip vertex buffer");
    const auto number = [](std::uint32_t word) { return float24(std::bit_cast<float>(word)); };
    const auto reg_float = [&](unsigned id) { return number(data(registers[id]) << 8); };
    const auto matrix = [&](unsigned index, unsigned rows) {
        Matrix result{}; result[3][3] = 1;
        for (unsigned col = 0; col < 4; ++col)
            for (unsigned row = 0; row < rows; ++row)
                result[row][col] = number((*matrices)[index][col * rows + row]);
        return result;
    };
    Matrix combined{};Vector scales{},centers{};
    if(gpu) {
        const auto key=transform_key(*matrices,registers);GpuTransform cached;
        if(gpu->cached_transform(key,cached)) {
            combined=cached.combined;scales=cached.scales;centers=cached.centers;
        } else {
            combined = multiply(matrix(3, 4), multiply(matrix(2, 3), matrix(1, 3)));
            scales = {reg_float(0x42), reg_float(0x43), reg_float(0x44), 1};
            centers = {reg_float(0x45), reg_float(0x46), reg_float(0x47), 0};
            gpu->cache_transform(key,GpuTransform{combined,scales,centers});
        }
    } else {
        combined = multiply(matrix(3, 4), multiply(matrix(2, 3), matrix(1, 3)));
        scales = {reg_float(0x42), reg_float(0x43), reg_float(0x44), 1};
        centers = {reg_float(0x45), reg_float(0x46), reg_float(0x47), 0};
    }
    const auto u_scale = textured?reg_float(0x48):0, v_scale = textured?reg_float(0x49):0;
    const auto u_offset = textured?reg_float(0x4A):0, v_offset = textured?reg_float(0x4B):0;
    std::vector<StripVertex> vertices;
    vertices.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto address = vertex_address + i * stride;
        const Vector position{number(memory.load32(address + position_offset)),number(memory.load32(address + position_offset+4)),
                              number(memory.load32(address + position_offset+8)),1};
        StripVertex vertex;vertex.position=position;vertex.color=packed_color?rgba4444(memory.load16(address)):
            vertex_color?memory.load32(address+8):primary;
        for (unsigned row = 0; row < 4; ++row) vertex.clip[row] = row_sum(combined[row], position);
        // Orthographic varying depth needs no pixel depth interpolation while
        // depth testing/writes are disabled and every vertex passes the range.
        // Other W still requires the measured reciprocal/perspective planes.
        if (vertex.clip[3] != 1)
            throw psprecomp::Error("Perspective transformed strips are not implemented");
        const auto sx = ge_add(float24(vertex.clip[0] * scales[0]), centers[0]);
        const auto sy = ge_add(float24(vertex.clip[1] * scales[1]), centers[1]);
        vertex.depth = std::floor(std::clamp(ge_add(float24(vertex.clip[2] * scales[2]), centers[2]), 0.0, 65535.0));
        vertex.outside_guard = sx < 0 || sx >= 4096 || sy < 0 || sy >= 4096;
        // Keep conversion defined even for an unsupported extreme transform.
        if (std::abs(sx) > 1048576 || std::abs(sy) > 1048576)
            throw psprecomp::Error("Extreme transformed strip screen coordinates");
        vertex.x = static_cast<std::int32_t>(std::floor(sx * 16)) - (registers[0x4C] & 65535u);
        vertex.y = static_cast<std::int32_t>(std::floor(sy * 16)) - (registers[0x4D] & 65535u);
        if(textured){
            const auto u = number(memory.load32(address)), v = number(memory.load32(address + 4));
            if (std::abs(u) > 32767 || std::abs(v) > 32767)
                throw psprecomp::Error("Extreme transformed strip UV coordinates");
            vertex.u = ge_add(float24(u * u_scale), u_offset) * texture->width();
            vertex.v = ge_add(float24(v * v_scale), v_offset) * texture->height();
        }
        vertices.push_back(vertex);
    }
    Raster::Result result{count, vertex_address + bytes, 0, 0};
    SceneRecorder::Building *building = nullptr;
    SceneDraw recorded;
    Matrix world_matrix{};
    if (recorder) {
        building = &recorder->target(registers);
        const auto clip_x1 = static_cast<int>(std::min(registers[0xD5] & 1023u, registers[0x16] & 1023u) + 1);
        const auto clip_y1 = static_cast<int>(std::min((registers[0xD5] >> 10) & 1023u, (registers[0x16] >> 10) & 1023u) + 1);
        recorded = SceneRecorder::state(registers, SceneDraw::Kind::triangles, false, static_cast<int>(registers[0xD4] & 1023u),
            static_cast<int>((registers[0xD4] >> 10) & 1023u), clip_x1, clip_y1);
        recorded.textured = textured;
        if (textured) { recorded.texture = recorder->texture(*building, *texture); recorded.linear = texture->linear_magnification(); }
        recorded.transform = recorder->transform(*building, *matrices, registers);
        recorded.first = static_cast<std::uint32_t>(building->scene->vertices.size());
        world_matrix = SceneRecorder::decode(*matrices, 1, 3);
    }
    const auto target = buffer_offset(registers[0x9C], registers[0x9D]);
    const auto target_stride = registers[0x9D] & 65535u;
    if (!target_stride || target_stride > 1024 || (target_stride & 63u))
        throw psprecomp::Error("Unsupported transformed strip framebuffer stride");
    const auto clip_x0 = static_cast<int>(registers[0xD4] & 1023u);
    const auto clip_y0 = static_cast<int>((registers[0xD4] >> 10) & 1023u);
    const auto clip_x1 = static_cast<int>(std::min(registers[0xD5] & 1023u, registers[0x16] & 1023u) + 1);
    const auto clip_y1 = static_cast<int>(std::min((registers[0xD5] >> 10) & 1023u, (registers[0x16] >> 10) & 1023u) + 1);
    struct Fragment { std::uint32_t x{}, y{}, source{}; };
    // Reused between strips; only its capacity carries over.
    static thread_local std::vector<Fragment> fragments;
    fragments.clear();
    // Each triangle's setup, recorded in strip order; its fragments are
    // generated afterwards, by row bands on helper threads for large strips.
    struct Triangle {
        StripVertex a, b, c;
        std::int64_t area{};
        int x0{}, y0{}, x1{}, y1{};
        bool minifying{}, smooth{}, color_plane{}, top_left0{}, top_left1{}, top_left2{};
        std::uint32_t flat_color{};
        std::array<std::int64_t,4> color_dx{}, color_dy{};
        std::int64_t step_x0{}, step_x1{}, step_x2{}, step_y0{}, step_y1{}, step_y2{}, row0{}, row1{}, row2{};
    };
    static thread_local std::vector<Triangle> triangle_storage;
    auto &triangles = triangle_storage; // Named through a reference, so helper threads use this thread's list.
    triangles.clear();
    std::uint64_t work = 0;
    for (std::uint32_t i = 2; i < count; ++i) {
        auto a = vertices[i - 2], b = vertices[i - 1], c = vertices[i];
        const auto flat_color=c.color; // Last submitted vertex, before any winding swaps.
        if (i & 1u) std::swap(a, b); // Preserve the strip's alternating winding.
        bool culled = a.outside_guard || b.outside_guard || c.outside_guard;
        for (unsigned axis = 0; axis < 3; ++axis)
            culled |= (a.clip[axis] < -1 && b.clip[axis] < -1 && c.clip[axis] < -1) ||
                      (a.clip[axis] > 1 && b.clip[axis] > 1 && c.clip[axis] > 1);
        if (culled) continue;
        if (a.clip[2] < -1 || b.clip[2] < -1 || c.clip[2] < -1)
            throw psprecomp::Error("Near-clipped transformed strips are not implemented");
        if (a.clip[2] > 1 || b.clip[2] > 1 || c.clip[2] > 1)
            throw psprecomp::Error("Far-clipped transformed strips are not implemented");
        const auto min_depth=std::min({a.depth,b.depth,c.depth}), max_depth=std::max({a.depth,b.depth,c.depth});
        const auto min_z=registers[0xD6]&65535u, max_z=registers[0xD7]&65535u;
        if(max_depth<min_z || min_depth>max_z)continue;
        if(min_depth<min_z || max_depth>max_z)
            throw psprecomp::Error("Partially depth-range-clipped transformed strips are not implemented");
        auto area = edge(a, b, c.x, c.y);
        if (!area) continue;
        if (building) for (const auto *corner : {&a, &b, &c}) {
            SceneVertex vertex;
            vertex.x = static_cast<float>(corner->x / 16.0); vertex.y = static_cast<float>(corner->y / 16.0);
            vertex.u = static_cast<float>(corner->u); vertex.v = static_cast<float>(corner->v); vertex.color = flat_color;
            if (data(registers[0x50]) == 1 && vertex_color) vertex.color = corner->color;
            vertex.world = SceneRecorder::world(world_matrix, corner->position);
            building->scene->vertices.push_back(vertex);
        }
        if (area < 0) { std::swap(b, c); area = -area; }
        const bool smooth=vertex_color && data(registers[0x50])==1;
        const bool color_plane=smooth && (a.color!=b.color || a.color!=c.color);
        std::array<std::int64_t,4> color_dx{},color_dy{};
        if (color_plane) {
            // The hardware setup reciprocal is exact for power-of-two areas.
            // Accept only gradients exactly representable in its14 fraction
            // bits, so the plane is independent of the hardware anchor choice.
            if (!std::has_single_bit(static_cast<std::uint64_t>(area)))
                throw psprecomp::Error("Nonuniform smooth strip color needs the GE setup reciprocal");
            for(unsigned channel=0;channel<4;++channel) {
                const auto ca=static_cast<std::int64_t>((a.color>>(channel*8))&255u);
                const auto cb=static_cast<std::int64_t>((b.color>>(channel*8))&255u);
                const auto cc=static_cast<std::int64_t>((c.color>>(channel*8))&255u);
                const auto dx=((cb-ca)*(c.y-a.y)-(cc-ca)*(b.y-a.y))*16384;
                const auto dy=((cc-ca)*(b.x-a.x)-(cb-ca)*(c.x-a.x))*16384;
                if (dx%area || dy%area) throw psprecomp::Error("Smooth strip color gradient precision is not implemented");
                color_dx[channel]=dx/area;color_dy[channel]=dy/area;
            }
        }
        const auto min_y = std::min({a.y, b.y, c.y}), max_y = std::max({a.y, b.y, c.y});
        if (max_y - min_y >= 43691) throw psprecomp::Error("Tall-triangle span coverage is not implemented");
        const auto x0 = std::max(clip_x0, std::min({a.x, b.x, c.x}) / 16);
        const auto y0 = std::max(clip_y0, min_y / 16);
        const auto x1 = std::min(clip_x1, (std::max({a.x, b.x, c.x}) + 15) / 16);
        const auto y1 = std::min(clip_y1, (max_y + 15) / 16);
        if (x0 >= x1 || y0 >= y1) continue;
        work += static_cast<std::uint64_t>(x1 - x0) * (y1 - y0);
        if (work > pixel_budget) throw psprecomp::Error("Transformed strip pixel budget exceeded");
        if (target + (static_cast<std::uint64_t>(y1 - 1) * target_stride + x1) * 4 > vram_size)
            throw psprecomp::Error("Transformed strip target range is outside VRAM");
        const auto du_dx = ((b.u-a.u)*(c.y-a.y)-(c.u-a.u)*(b.y-a.y))*16/area;
        const auto du_dy = ((c.u-a.u)*(b.x-a.x)-(b.u-a.u)*(c.x-a.x))*16/area;
        const auto dv_dx = ((b.v-a.v)*(c.y-a.y)-(c.v-a.v)*(b.y-a.y))*16/area;
        const auto dv_dy = ((c.v-a.v)*(b.x-a.x)-(b.v-a.v)*(c.x-a.x))*16/area;
        const bool minifying = std::max({std::abs(du_dx),std::abs(du_dy),std::abs(dv_dx),std::abs(dv_dy)}) > 1;
        // Edges are affine integer equations. One pixel changes x/y by16
        // subpixels, so incremental evaluation is exact without FP changes.
        const auto edge_step_x0 = -static_cast<std::int64_t>(c.y-b.y)*16;
        const auto edge_step_x1 = -static_cast<std::int64_t>(a.y-c.y)*16;
        const auto edge_step_x2 = -static_cast<std::int64_t>(b.y-a.y)*16;
        const auto edge_step_y0 = static_cast<std::int64_t>(c.x-b.x)*16;
        const auto edge_step_y1 = static_cast<std::int64_t>(a.x-c.x)*16;
        const auto edge_step_y2 = static_cast<std::int64_t>(b.x-a.x)*16;
        auto edge_row0 = edge(b,c,x0*16+8,y0*16+8);
        auto edge_row1 = edge(c,a,x0*16+8,y0*16+8);
        auto edge_row2 = edge(a,b,x0*16+8,y0*16+8);
        const bool edge_top_left0 = top_left(b,c), edge_top_left1 = top_left(c,a), edge_top_left2 = top_left(a,b);
        triangles.push_back({a, b, c, area, x0, y0, x1, y1, minifying, smooth, color_plane,
            edge_top_left0, edge_top_left1, edge_top_left2, flat_color, color_dx, color_dy,
            edge_step_x0, edge_step_x1, edge_step_x2, edge_step_y0, edge_step_y1, edge_step_y2, edge_row0, edge_row1, edge_row2});
    }
    // Fragments of rows [top, bottom) of every triangle, in strip order.
    const auto generate = [&](std::vector<Fragment> &out, int top, int bottom) {
        for (const auto &t : triangles) {
            const auto first = std::max(t.y0, top), last = std::min(t.y1, bottom);
            if (first >= last) continue;
            const auto &a = t.a, &b = t.b, &c = t.c;
            const auto area = t.area;
            const bool edge_top_left0 = t.top_left0, edge_top_left1 = t.top_left1, edge_top_left2 = t.top_left2;
            // The edge equations are integers: a band may start mid-triangle exactly.
            auto edge_row0 = t.row0 + (first - t.y0) * t.step_y0;
            auto edge_row1 = t.row1 + (first - t.y0) * t.step_y1;
            auto edge_row2 = t.row2 + (first - t.y0) * t.step_y2;
            for (auto y = first; y < last; ++y, edge_row0+=t.step_y0, edge_row1+=t.step_y1, edge_row2+=t.step_y2) {
            auto e0=edge_row0, e1=edge_row1, e2=edge_row2;
            for (auto x = t.x0; x < t.x1; ++x, e0+=t.step_x0, e1+=t.step_x1, e2+=t.step_x2) {
                if (e0 < 0 || (e0 == 0 && !edge_top_left0) || e1 < 0 || (e1 == 0 && !edge_top_left1) ||
                    e2 < 0 || (e2 == 0 && !edge_top_left2)) continue;
                // Exact top-left coverage; bounded affine UV interpolation. The
                // measured setup reciprocal/14-bit plane arithmetic is not claimed.
                const auto u = static_cast<float>((a.u*e0+b.u*e1+c.u*e2)/area);
                const auto v = static_cast<float>((a.v*e0+b.v*e1+c.v*e2)/area);
                auto color=t.smooth?a.color:t.flat_color;
                if (t.color_plane) {
                    color=0;
                    for(unsigned channel=0;channel<4;++channel) {
                        const auto at_a=static_cast<std::int64_t>((a.color>>(channel*8))&255u)*16384;
                        const auto value=(at_a+t.color_dx[channel]*(x*16+8-a.x)+t.color_dy[channel]*(y*16+8-a.y))>>14;
                        color|=static_cast<std::uint32_t>(std::clamp<std::int64_t>(value,0,255))<<(channel*8);
                    }
                }
                out.push_back({static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),
                    textured?modulate(texture->sample(u,v,t.minifying),color,texture_function==0x100):color});
            }
            }
        }
    };
    if (building) {
        recorded.count = static_cast<std::uint32_t>(building->scene->vertices.size()) - recorded.first;
        recorded.material = SceneRecorder::material(recorded.kind, false, textured ? SceneRecorder::texture_shape(*texture) : 0,
            recorded.blend_mode, 0, 0, recorded.count);
        recorded.family = SceneRecorder::material(recorded.kind, false, textured ? SceneRecorder::texture_shape(*texture) : 0,
            recorded.blend_mode, -1, -1, recorded.count);
        if (recorded.count) building->scene->draws.push_back(recorded);
    }
    const auto preserve = data(registers[0xE8]) | 0xFF000000u;
    int top = std::numeric_limits<int>::max(), bottom = std::numeric_limits<int>::min();
    bool aliased = false;
    for (const auto &t : triangles) {
        top = std::min(top, t.y0); bottom = std::max(bottom, t.y1);
        aliased |= t.x1 > static_cast<int>(target_stride);
    }
    // Large strips (debris and dust while digging) split their rows into bands
    // on helper threads, like sprites: every band generates its fragments,
    // then every band writes its own rows in strip order. Rows that alias past
    // the stride and diagnostic write watches keep the single-thread path.
    if (workers && work >= 4096 && bottom - top >= 8 && !aliased && !memory.write_watch_enabled()) {
        const std::size_t bands = std::min<std::size_t>(2u * (workers->helpers() + 1u), static_cast<std::size_t>(bottom - top));
        const auto rows = static_cast<int>((static_cast<std::size_t>(bottom - top) + bands - 1) / bands);
        std::vector<std::vector<Fragment>> banded(bands);
        workers->run(bands, [&](std::size_t band) {
            const auto first = top + static_cast<int>(band) * rows;
            generate(banded[band], first, std::min(bottom, first + rows));
        });
        if (preserve == 0xFFFFFFFFu) return result;
        std::atomic<std::uint64_t> written{0};
        workers->run(bands, [&](std::size_t band) {
            std::uint64_t count = 0;
            for (const auto &fragment : banded[band]) {
                const auto address = vram_base + target + (fragment.y * target_stride + fragment.x) * 4;
                const auto old = memory.load32(address);
                const auto next = dither(blend.apply(fragment.source,old),registers,fragment.x,fragment.y);
                memory.store32(address, (old&preserve)|(next&~preserve));
                ++count;
            }
            written += count;
        });
        result.color_pixels = written;
        return result;
    }
    if (!triangles.empty()) generate(fragments, top, bottom);
    std::uint8_t *unwatched_vram = nullptr;
    bool first_store = true;
    if (preserve != 0xFFFFFFFFu) for (const auto &fragment : fragments) {
        const auto offset = target + (fragment.y * target_stride + fragment.x) * 4;
        const auto address = vram_base + offset;
        std::uint32_t old;
        if (unwatched_vram) std::memcpy(&old, unwatched_vram + offset, sizeof(old));
        else old = memory.load32(address);
        const auto next = dither(blend.apply(fragment.source,old),registers,fragment.x,fragment.y);
        const auto value = (old&preserve)|(next&~preserve);
        if (first_store) {
            first_store = false;
            // Match the first scalar store's lazy watch initialization only
            // after its old value and blend/dither result were computed.
            // All fragments were validated/collected before this write loop.
            if constexpr (std::endian::native == std::endian::little) {
                if (memory.prepare_unwatched_write(address, sizeof(value)))
                    unwatched_vram = memory.raw_pointer(vram_base, vram_size);
            }
        }
        if (unwatched_vram) std::memcpy(unwatched_vram + offset, &value, sizeof(value));
        else memory.store32(address, value);
        ++result.color_pixels;
    }
    return result;
}
Raster::Result draw_sprites(psprecomp::GuestMemory &memory, std::uint32_t count,
                            const std::array<std::uint32_t, 256> &registers,
                            std::uint32_t vertex_address, const Clut *palette, const Raster::Matrices *matrices,
                            GpuRaster *gpu, SceneRecorder *recorder, WorkerPool *workers) {
    const auto type = data(registers[0x12]);
    const bool transformed = type == 0x19Fu;
    const bool has_uv = type != 0x0080019Cu;
    if ((count & 1u) || (!transformed && type != 0x0080019Fu && type != 0x0080019Cu) || data(registers[0xD2]) != 3)
        throw psprecomp::Error("Unsupported ordinary sprite vertex count/type or framebuffer format");
    if (data(registers[0x15]) != 0)
        throw psprecomp::Error("Nonzero sprite drawing-region origin is not implemented");
    // Backface culling does not apply to sprite primitives. Dither affects
    // even 8888 targets and is implemented below as signed 4-bit RGB offsets.
    if (data(registers[0x1D]) > 1 || data(registers[0x20]) > 1)
        throw psprecomp::Error("Unsupported sprite cull/dither enable bits");
    for (const auto state : {0x1F, 0x22, 0x23, 0x24, 0x25, 0x27, 0x28})
        if (data(registers[state]) != 0)
            throw psprecomp::Error("Unsupported enabled sprite test/fog/logic state");
    const bool textured = data(registers[0x1E]) != 0;
    if (data(registers[0x1E]) > 1 || (textured && !has_uv) || data(registers[0x21]) > 1 || data(registers[0xE7]) > 1)
        throw psprecomp::Error("Unsupported sprite texture/blend/depth-mask state");
    if (transformed && (!matrices || data(registers[0x17]) != 0 || data(registers[0x1C]) > 1 ||
        (data(registers[0x4C]) & ~65535u) || (data(registers[0x4D]) & ~65535u)))
        throw psprecomp::Error("Unsupported transformed sprite matrices/lighting/clip/offset state");
    const Blend blend(registers);
    const auto texture_function = data(registers[0xC9]);
    if (textured && texture_function != 0 && texture_function != 0x100)
        throw psprecomp::Error("Unsupported sprite texture function (requires MODULATE)");
    if (textured && data(registers[0xC0]) != 0)
        throw psprecomp::Error("Unsupported sprite texture-coordinate generation mode");
    if (data(registers[0xE9]) & ~255u)
        throw psprecomp::Error("Unsupported sprite alpha-mask bits");
    std::optional<Texture> texture;
    if (textured) {
        const auto texture_address = psprecomp::GuestMemory::canonical(
            data(registers[0xA0]) | ((registers[0xA8] & 0x00FF0000u) << 8));
        // Disabled texture state must not read stale addresses, formats or UVs.
        // Enabled sampling is staged before framebuffer writes and currently
        // restricted to RAM to preserve self-texturing ordering diagnostics.
        if (texture_address < psprecomp::GuestMemory::kPhysicalBase)
            throw psprecomp::Error("VRAM/self-textured ordinary sprites are not implemented");
        texture.emplace(memory, registers, palette);
    }
    const std::uint32_t stride = has_uv ? 24 : 16;
    const std::uint32_t color_offset = has_uv ? 8 : 0;
    const auto position_offset = color_offset + 4;
    const auto bytes = static_cast<std::uint64_t>(count) * stride;
    if ((vertex_address & 3u) || bytes > std::numeric_limits<std::uint32_t>::max() - vertex_address ||
        !memory.contains(vertex_address, static_cast<std::size_t>(bytes)))
        throw psprecomp::Error("Invalid ordinary sprite vertex buffer");
    const auto target = buffer_offset(registers[0x9C], registers[0x9D]);
    const auto target_stride = registers[0x9D] & 0xFFFFu;
    if (!target_stride || target_stride > 1024 || (target_stride & 63u))
        throw psprecomp::Error("Unsupported sprite framebuffer stride");
    const auto scissor0 = data(registers[0xD4]), scissor1 = data(registers[0xD5]);
    const auto region_end = data(registers[0x16]);
    const auto clip_x0 = static_cast<std::int32_t>(scissor0 & 1023u);
    const auto clip_y0 = static_cast<std::int32_t>((scissor0 >> 10) & 1023u);
    const auto clip_x1 = static_cast<std::int32_t>(std::min(scissor1 & 1023u, region_end & 1023u) + 1);
    const auto clip_y1 = static_cast<std::int32_t>(std::min((scissor1 >> 10) & 1023u, (region_end >> 10) & 1023u) + 1);
    const auto number = [&memory](std::uint32_t address) {
        const auto value = std::bit_cast<float>(memory.load32(address));
        if (!std::isfinite(value)) throw psprecomp::Error("Nonfinite ordinary sprite vertex value");
        return value;
    };
    const auto reg_float = [&](unsigned id) { return float24(std::bit_cast<float>(data(registers[id]) << 8)); };
    const auto matrix = [&](unsigned index, unsigned rows) {
        Matrix result{}; result[3][3] = 1;
        for (unsigned col = 0; col < 4; ++col) for (unsigned row = 0; row < rows; ++row)
            result[row][col] = float24(std::bit_cast<float>((*matrices)[index][col * rows + row]));
        return result;
    };
    Matrix combined{};
    Vector scales{}, centers{};
    if (transformed) {
        if(gpu) {
            const auto key=transform_key(*matrices,registers);GpuTransform cached;
            if(gpu->cached_transform(key,cached)) {
                combined=cached.combined;scales=cached.scales;centers=cached.centers;
            } else {
                combined = multiply(matrix(3,4), multiply(matrix(2,3),matrix(1,3)));
                scales = {reg_float(0x42),reg_float(0x43),reg_float(0x44),1};
                centers = {reg_float(0x45),reg_float(0x46),reg_float(0x47),0};
                // A failed matrix multiply or later viewport decode never
                // commits this key. Vertex/UV reads still occur below, live.
                gpu->cache_transform(key,GpuTransform{combined,scales,centers});
            }
        } else {
            // The combined matrix depends only on the key's matrix words and
            // viewport registers; reuse it while they stay the same.
            static thread_local std::optional<std::pair<GpuTransformKey, GpuTransform>> cache;
            const auto key = transform_key(*matrices, registers);
            const bool nearest = std::fegetround() == FE_TONEAREST;
            if (nearest && cache && cache->first == key) {
                combined = cache->second.combined; scales = cache->second.scales; centers = cache->second.centers;
            } else {
                combined = multiply(matrix(3,4), multiply(matrix(2,3),matrix(1,3)));
                scales = {reg_float(0x42),reg_float(0x43),reg_float(0x44),1};
                centers = {reg_float(0x45),reg_float(0x46),reg_float(0x47),0};
                if (nearest) cache.emplace(key, GpuTransform{combined, scales, centers});
            }
        }
    }
    struct Corner { Vector position{}, clip{}; std::int32_t x{}, y{}; double z{}; bool outside{}; };
    const auto corner = [&](std::uint32_t address) {
        Corner result;
        const Vector position{float24(number(address+position_offset)),float24(number(address+position_offset+4)),float24(number(address+position_offset+8)),1};
        result.position = position;
        for (unsigned row=0;row<4;++row) result.clip[row]=row_sum(combined[row],position);
        // The current orthographic path is exact at W=1. Other positive W
        // needs the hardware reciprocal; do not silently substitute host division.
        if (result.clip[3] < 0) { result.outside=true; return result; }
        if (result.clip[3] != 1) throw psprecomp::Error("Perspective transformed sprites are not implemented");
        const auto sx=ge_add(float24(result.clip[0]*scales[0]),centers[0]);
        const auto sy=ge_add(float24(result.clip[1]*scales[1]),centers[1]);
        result.z=ge_add(float24(result.clip[2]*scales[2]),centers[2]);
        result.outside=sx<0 || sx>=4096 || sy<0 || sy>=4096 ||
            (!data(registers[0x1C]) && (result.z<0 || result.z>=65536));
        if (std::abs(sx)>1048576 || std::abs(sy)>1048576)
            throw psprecomp::Error("Extreme transformed sprite screen coordinates");
        result.x=static_cast<std::int32_t>(std::floor(sx*16))-static_cast<std::int32_t>(registers[0x4C]&65535u);
        result.y=static_cast<std::int32_t>(std::floor(sy*16))-static_cast<std::int32_t>(registers[0x4D]&65535u);
        if (data(registers[0x1C])) result.z=std::clamp(result.z,0.0,65535.0);
        result.z=std::floor(result.z);
        return result;
    };
    // Reused per thread: the game issues thousands of small sprite draws per frame.
    // Named through references, so helper threads use this thread's lists.
    static thread_local std::vector<Sprite> sprite_storage;
    static thread_local std::vector<std::array<Vector, 2>> position_storage;
    static thread_local std::vector<std::pair<Sprite, bool>> recorded_storage;
    auto &sprites = sprite_storage;
    auto &positions = position_storage; // Model corners of transformed sprites, when recording.
    // Recorded sprites in draw order; true marks one kept only for a wider view.
    auto &recorded = recorded_storage;
    sprites.clear(); positions.clear(); recorded.clear();
    // Beyond sprites stay within the recorder's margin, under a scissor that
    // spans the native width.
    const auto margin = recorder && transformed && clip_x0 == 0 && clip_x1 >= 480 ? recorder->beyond * 16 : 0.0;
    const auto beyond_range = [&](const Sprite &sprite) {
        const auto lo = std::min(sprite.ax, sprite.bx), hi = std::max(sprite.ax, sprite.bx);
        return margin > 0 && hi > -margin && lo < 480 * 16 + margin;
    };
    std::uint64_t pixels = 0;
    for (std::uint32_t i = 0; i < count; i += 2) {
        const auto first = vertex_address + i * stride, second = first + stride;
        Sprite sprite;
        bool beyond = false;
        if (transformed) {
            const auto a=corner(first), b=corner(second);
            bool culled=a.outside || b.outside, culled_x=false;
            for (unsigned axis=0;axis<3;++axis) {
                const bool out=(a.clip[axis]<-1 && b.clip[axis]<-1) || (a.clip[axis]>1 && b.clip[axis]>1);
                if (axis==0) culled_x=out; else culled|=out;
            }
            // Rectangle depth/color come from vertex two, but both vertices
            // participate in clip/guard-band rejection.
            if (culled || b.z<(registers[0xD6]&65535u) || b.z>(registers[0xD7]&65535u)) continue;
            sprite.ax=a.x; sprite.ay=a.y; sprite.bx=b.x; sprite.by=b.y;
            // Culled only for lying left or right of the screen: a wider view may still show it.
            if (culled_x) { if (!beyond_range(sprite)) continue; beyond=true; }
            if (recorder) positions.push_back({a.position, b.position});
        } else {
            const auto ax=number(first+position_offset), ay=number(first+position_offset+4), az=number(first+position_offset+8);
            const auto bx=number(second+position_offset), by=number(second+position_offset+4), bz=number(second+position_offset+8);
            if (std::abs(ax)>=4096 || std::abs(ay)>=4096 || std::abs(bx)>=4096 || std::abs(by)>=4096 ||
                az<0 || az>65535 || bz<0 || bz>65535)
                throw psprecomp::Error("Unsupported wrapped ordinary sprite coordinates/depth");
            sprite.ax=static_cast<std::int32_t>(ax*16); sprite.ay=static_cast<std::int32_t>(ay*16);
            sprite.bx=static_cast<std::int32_t>(bx*16); sprite.by=static_cast<std::int32_t>(by*16);
        }
        // A culled sprite's UVs were never checked by the exact path; one that
        // would fail those checks is not recorded, and nothing throws for it.
        if (beyond && textured) {
            bool usable = true;
            for (const auto address : {first, first + 4, second, second + 4}) {
                const auto value = std::bit_cast<float>(memory.load32(address));
                usable = usable && std::isfinite(value) && std::abs(value) <= 32767;
            }
            if (!usable) { positions.pop_back(); continue; }
        }
        if (textured) {
            sprite.u0=number(first); sprite.v0=number(first+4);
            sprite.u1=number(second); sprite.v1=number(second+4);
            if (std::abs(sprite.u0)>32767 || std::abs(sprite.v0)>32767 || std::abs(sprite.u1)>32767 || std::abs(sprite.v1)>32767)
                throw psprecomp::Error("Unsupported ordinary sprite UV range");
            if (transformed) {
                const auto uv=[&](double value,unsigned scale,unsigned offset,unsigned size) {
                    return static_cast<float>(float24(ge_add(float24(float24(value)*reg_float(scale)),reg_float(offset)))*size);
                };
                sprite.u0=uv(sprite.u0,0x48,0x4A,texture->width());sprite.u1=uv(sprite.u1,0x48,0x4A,texture->width());
                sprite.v0=uv(sprite.v0,0x49,0x4B,texture->height());sprite.v1=uv(sprite.v1,0x49,0x4B,texture->height());
            }
        }
        sprite.color=memory.load32(second+color_offset);
        if (sprite.ax==sprite.bx || sprite.ay==sprite.by) { if (recorder && transformed) positions.pop_back(); continue; }
        sprite.rotated=(sprite.ax<sprite.bx)!=(sprite.ay<sprite.by);
        const auto x0=std::max((std::min(sprite.ax,sprite.bx)+6)>>4,clip_x0);
        const auto y0=std::max((std::min(sprite.ay,sprite.by)+(sprite.rotated?6:7))>>4,clip_y0);
        const auto x1=std::min((std::max(sprite.ax,sprite.bx)+7)>>4,clip_x1);
        const auto y1=std::min((std::max(sprite.ay,sprite.by)+7)>>4,clip_y1);
        // A sprite ending exactly at a screen edge passes the clip test but
        // covers no pixel; for a wider view it lies beyond the edge as well.
        if (!beyond && x0>=x1 && y0<y1 && transformed && beyond_range(sprite)) beyond=true;
        if (beyond) {
            // Only sprites on the screen's rows; the view is wider, not taller.
            if (y0<y1) recorded.emplace_back(sprite, true); else positions.pop_back();
            continue;
        }
        if (x0>=x1 || y0>=y1) { if (recorder && transformed) positions.pop_back(); continue; }
        sprite.x0=x0;sprite.y0=y0;sprite.x1=x1;sprite.y1=y1;
        sprite.minifying=std::abs(sprite.u1-sprite.u0)*16>std::abs(sprite.rotated?sprite.by-sprite.ay:sprite.bx-sprite.ax) ||
                          std::abs(sprite.v1-sprite.v0)*16>std::abs(sprite.rotated?sprite.bx-sprite.ax:sprite.by-sprite.ay);
        pixels += static_cast<std::uint64_t>(x1 - x0) * (y1 - y0);
        if (pixels > pixel_budget) throw psprecomp::Error("Ordinary sprite pixel budget exceeded");
        const auto end = static_cast<std::uint64_t>(target) +
            (static_cast<std::uint64_t>(y1 - 1) * target_stride + x1) * 4;
        if (end > vram_size) throw psprecomp::Error("Ordinary sprite target range is outside VRAM");
        sprites.push_back(sprite);
        if (recorder) recorded.emplace_back(sprite, false);
    }
    if (recorder && !recorded.empty()) {
        auto &building = recorder->target(registers);
        auto state = SceneRecorder::state(registers, SceneDraw::Kind::sprite, !transformed, clip_x0, clip_y0, clip_x1, clip_y1);
        if (textured) { state.texture = recorder->texture(building, *texture); state.linear = texture->linear_magnification(); }
        Matrix world_matrix{};
        if (transformed) { state.transform = recorder->transform(building, *matrices, registers); world_matrix = SceneRecorder::decode(*matrices, 1, 3); }
        // One draw per run of on-screen or beyond sprites, in their original order.
        for (std::size_t run = 0; run < recorded.size();) {
            auto end = run;
            while (end < recorded.size() && recorded[end].second == recorded[run].second) ++end;
            auto draw = state;
            draw.beyond = recorded[run].second;
            draw.first = static_cast<std::uint32_t>(building.scene->vertices.size());
            for (std::size_t i = run; i < end; ++i) {
                const auto &sprite = recorded[i].first;
                for (unsigned corner = 0; corner < 2; ++corner) {
                    SceneVertex vertex;
                    vertex.x = static_cast<float>((corner ? sprite.bx : sprite.ax) / 16.0);
                    vertex.y = static_cast<float>((corner ? sprite.by : sprite.ay) / 16.0);
                    vertex.u = corner ? sprite.u1 : sprite.u0; vertex.v = corner ? sprite.v1 : sprite.v0;
                    vertex.color = sprite.color;
                    if (transformed) vertex.world = SceneRecorder::world(world_matrix, positions[i][corner]);
                    building.scene->vertices.push_back(vertex);
                }
            }
            draw.count = static_cast<std::uint32_t>((end - run) * 2);
            // Identity size: exact world size for camera-transformed sprites (their
            // projected screen size jitters by 1/16 pixel as the camera moves),
            // exact screen size for 2D sprites.
            const auto &front = recorded[run].first;
            std::int64_t width = std::abs(front.bx - front.ax), height = std::abs(front.by - front.ay);
            if (transformed) {
                const auto &a = building.scene->vertices[draw.first].world, &b = building.scene->vertices[draw.first + 1].world;
                width = std::llround(std::abs(b[0] - a[0]) * 16); height = std::llround(std::abs(b[1] - a[1]) * 16);
            }
            draw.material = SceneRecorder::material(draw.kind, draw.through, textured ? SceneRecorder::texture_shape(*texture) : 0, draw.blend_mode,
                width, height, draw.count);
            draw.family = SceneRecorder::material(draw.kind, draw.through, textured ? SceneRecorder::texture_shape(*texture) : 0, draw.blend_mode,
                -1, -1, draw.count);
            building.scene->draws.push_back(draw);
            run = end;
        }
    }
    if(gpu) {
        const auto preserve=data(registers[0xE8])|0xFF000000u;
        if(sprites.empty())
            return {count,vertex_address+static_cast<std::uint32_t>(bytes),0,0};
        if(gpu->enqueue(sprites,registers,texture?&*texture:nullptr))
            return {count,vertex_address+static_cast<std::uint32_t>(bytes),preserve==0xFFFFFFFFu?0:pixels,0};
        gpu->flush();
    }
    struct Fragment { std::uint32_t address{}, source{}, x{}, y{}; };
    // All fragments of rows [top, bottom) in submission order; every one is
    // sampled before any framebuffer write.
    const auto collect = [&](std::vector<Fragment> &fragments, std::uint32_t top, std::uint32_t bottom) {
        // Bounded affine W=1/through interpolation at pixel centers. GE's
        // reciprocal/plane precision is not claimed bit-exact. A coordinate
        // depends only on its pixel's column or only on its row, so each one
        // is computed once per sprite with the per-pixel formula.
        static thread_local std::vector<int> columns, rows;
        for (const auto &sprite : sprites) {
            const auto row_begin = std::max(sprite.y0, top), row_end = std::min(sprite.y1, bottom);
            if (row_begin >= row_end) continue;
            if (textured) {
                columns.resize(sprite.x1 - sprite.x0);
                rows.resize(row_end - row_begin);
                for (auto x = sprite.x0; x < sprite.x1; ++x) {
                    const auto fx = (static_cast<double>(x * 16 + 8) - sprite.ax) / (sprite.bx - sprite.ax);
                    columns[x - sprite.x0] = sprite.rotated ? texture->fixed_v(static_cast<float>(sprite.v0 + (sprite.v1 - sprite.v0) * fx))
                                                            : texture->fixed_u(static_cast<float>(sprite.u0 + (sprite.u1 - sprite.u0) * fx));
                }
                for (auto y = row_begin; y < row_end; ++y) {
                    const auto fy = (static_cast<double>(y * 16 + 8) - sprite.ay) / (sprite.by - sprite.ay);
                    rows[y - row_begin] = sprite.rotated ? texture->fixed_u(static_cast<float>(sprite.u0 + (sprite.u1 - sprite.u0) * fy))
                                                         : texture->fixed_v(static_cast<float>(sprite.v0 + (sprite.v1 - sprite.v0) * fy));
                }
            }
            for (auto y = row_begin; y < row_end; ++y) {
                for (auto x = sprite.x0; x < sprite.x1; ++x) {
                    auto source = sprite.color;
                    if (textured) {
                        const auto column = columns[x - sprite.x0], row = rows[y - row_begin];
                        source = modulate(sprite.rotated ? texture->sample_fixed(row, column, sprite.minifying)
                                                         : texture->sample_fixed(column, row, sprite.minifying),
                                          sprite.color, texture_function == 0x100);
                    }
                    fragments.push_back({vram_base + target + (y * target_stride + x) * 4, source, x, y});
                }
            }
        }
    };
    // Stencil is disabled in this subset. The framebuffer alpha byte is the
    // stencil buffer and remains untouched by ordinary color/alpha blending.
    const auto preserve = data(registers[0xE8]) | 0xFF000000u;
    const auto write = [&](const std::vector<Fragment> &fragments) {
        std::uint64_t written = 0;
        if (preserve == 0xFFFFFFFFu) return written;
        for (const auto &fragment : fragments) {
            const auto old = memory.load32(fragment.address);
            const auto next = dither(blend.apply(fragment.source,old),
                                     registers, fragment.x, fragment.y);
            memory.store32(fragment.address, (old & preserve) | (next & ~preserve));
            ++written;
        }
        return written;
    };
    Raster::Result result{count, vertex_address + static_cast<std::uint32_t>(bytes), 0, 0};
    std::uint32_t top = std::numeric_limits<std::uint32_t>::max(), bottom = 0;
    for (const auto &sprite : sprites) { top = std::min(top, sprite.y0); bottom = std::max(bottom, sprite.y1); }
    // Large batches split their rows into bands on helper threads: every band
    // samples, then every band writes its own rows. Each pixel therefore sees
    // the same fragments in the same order as on one thread. Rows that alias
    // past the stride and diagnostic write watches keep the single-thread order.
    if (workers && pixels >= 16384 && bottom - top >= 8 && !memory.write_watch_enabled() &&
        std::all_of(sprites.begin(), sprites.end(), [&](const auto &sprite) { return sprite.x1 <= target_stride; })) {
        const std::size_t bands = std::min<std::size_t>(2u * (workers->helpers() + 1u), bottom - top);
        const auto rows = static_cast<std::uint32_t>((bottom - top + bands - 1) / bands);
        std::vector<std::vector<Fragment>> banded(bands);
        workers->run(bands, [&](std::size_t band) {
            const auto first = top + static_cast<std::uint32_t>(band) * rows;
            collect(banded[band], first, std::min(bottom, first + rows));
        });
        std::atomic<std::uint64_t> written{0};
        workers->run(bands, [&](std::size_t band) { written += write(banded[band]); });
        result.color_pixels = written;
        return result;
    }
    static thread_local std::vector<Fragment> fragments;
    fragments.clear();
    collect(fragments, top, bottom);
    result.color_pixels = write(fragments);
    // Depth testing is disabled; therefore this subset performs no depth writes.
    return result;
}
}
Raster::Raster(psprecomp::GuestMemory &memory):memory_(memory) {
    if(std::getenv("BADMAN_RASTER_STATS"))statistics_=std::make_unique<Statistics>();
    // Software sprite helpers: BADMAN_RASTER_THREADS (0 = none), else up to four.
    set_threads(WorkerPool::default_helpers());
    const auto selected=std::getenv("BADMAN_RENDERER");
    const std::string_view renderer=selected?selected:"software";
    if(renderer!="software"&&renderer!="d3d11"&&renderer!="vulkan"&&renderer!="cpu")
        throw psprecomp::Error("Unknown rendering backend: "+std::string(renderer));
    if(renderer!="software") {
        if(memory.write_watch_enabled())throw psprecomp::Error("GPU rendering cannot preserve per-pixel diagnostic write watches; select Software for write-watch analysis");
        gpu_=std::make_unique<GpuRaster>(memory,renderer);
    }
}
Raster::~Raster()=default;
void Raster::set_threads(unsigned helpers){workers_=helpers?std::make_unique<WorkerPool>(std::min(helpers,15u)):nullptr;}
void Raster::flush(){if(gpu_)gpu_->flush();}
void Raster::invalidate_textures(){if(gpu_)gpu_->invalidate_textures();if(recorder_)recorder_->invalidate_textures();}
void Raster::palette_loaded(){if(gpu_)gpu_->invalidate_textures();if(recorder_)recorder_->palette_loaded();}
void Raster::set_scene_recording(bool enabled,double beyond){
    if(!enabled)recorder_.reset();
    else if(!recorder_)recorder_=std::make_unique<SceneRecorder>();
    if(recorder_)recorder_->beyond=std::max(0.0,beyond);
}
std::shared_ptr<Scene> Raster::take_scene(std::uint32_t framebuffer_address,std::int64_t time_ns){
    return recorder_?recorder_->take(framebuffer_address,time_ns):nullptr;
}
std::string Raster::renderer_description()const{return gpu_?gpu_->description():"Software";}
struct Raster::Statistics {
    struct Kind { std::uint64_t draws{}, pixels{}, nanoseconds{}; } kinds[3]; // clears, sprites, strips
    std::uint64_t sprite_sizes[3]{}; // sprite draws under 1K, under 16K, from 16K pixels
    std::chrono::steady_clock::time_point next{std::chrono::steady_clock::now() + std::chrono::seconds(5)};
    void add(unsigned kind, std::uint64_t pixels, std::chrono::steady_clock::duration time) {
        auto &k = kinds[kind];
        ++k.draws; k.pixels += pixels; k.nanoseconds += static_cast<std::uint64_t>(std::chrono::nanoseconds(time).count());
        if (kind == 1) ++sprite_sizes[pixels < 1024 ? 0 : pixels < 16384 ? 1 : 2];
        const auto now = std::chrono::steady_clock::now();
        if (now < next) return;
        static const char *names[]{"clears", "sprites", "strips"};
        std::cout << "[raster-stats]";
        for (unsigned i = 0; i < 3; ++i)
            std::cout << ' ' << names[i] << '=' << kinds[i].draws << '/' << kinds[i].pixels << "px/" << kinds[i].nanoseconds / 1000000 << "ms";
        std::cout << " sprite_draws<1K/<16K/>=16K=" << sprite_sizes[0] << '/' << sprite_sizes[1] << '/' << sprite_sizes[2] << std::endl;
        *this = Statistics{};
    }
};
Raster::Result Raster::draw(std::uint32_t command,
                            const std::array<std::uint32_t, 256> &registers,
                            std::uint32_t vertex_address, const Clut *palette, const Matrices *matrices) {
    try {
        const auto started = statistics_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto result=draw_impl(command,registers,vertex_address,palette,matrices);
        if (statistics_)
            statistics_->add(data(registers[0xD3]) ? 0u : (command & 0xFFFF0000u) == 0x04040000u ? 2u : 1u,
                             result.color_pixels, std::chrono::steady_clock::now() - started);
        if(!deferred_)flush();
        return result;
    }catch(...){flush();throw;}
}
Raster::Result Raster::draw_impl(std::uint32_t command,
                            const std::array<std::uint32_t, 256> &registers,
                            std::uint32_t vertex_address, const Clut *palette, const Matrices *matrices) {
    const auto primitive = command & 0xFFFF0000u;
    if (primitive != 0x04060000u && primitive != 0x04040000u)
        throw psprecomp::Error("Raster supports only sprite/triangle-strip PRIM commands");
    const auto count = command & 0xFFFFu;
    vertex_address = psprecomp::GuestMemory::canonical(vertex_address);
    if(vertex_address>=psprecomp::GuestMemory::kVramPhysicalBase &&
       vertex_address<psprecomp::GuestMemory::kPhysicalBase)flush();
    if (!count) return {0, vertex_address, 0, 0};
    if (primitive == 0x04040000u) {flush();return draw_strip(memory_,count,registers,vertex_address,palette,matrices,gpu_.get(),recorder_.get(),workers_.get());}
    if (data(registers[0xD3]) == 0) return draw_sprites(memory_, count, registers, vertex_address, palette, matrices,gpu_.get(),recorder_.get(),workers_.get());
    flush();
    if ((count & 1u) || data(registers[0x12]) != 0x0080011Cu || data(registers[0xD2]) != 3)
        throw psprecomp::Error("Unsupported clear vertex count/type or framebuffer format");
    const auto clear = data(registers[0xD3]);
    if (!(clear & 1u) || (clear & ~0x701u))
        throw psprecomp::Error("Raster requires the supported clear mode");
    if (data(registers[0x15]) != 0)
        throw psprecomp::Error("Nonzero clear drawing-region origin is not implemented");
    const auto flags = (clear >> 8) & 7u;
    if ((flags & 4u) && data(registers[0xE7]) != 0)
        throw psprecomp::Error("Masked clear depth writes are not implemented");
    if (data(registers[0xE9]) & ~255u)
        throw psprecomp::Error("Unsupported clear alpha-mask bits");
    const auto bytes = static_cast<std::uint64_t>(count) * vertex_stride;
    if ((vertex_address & 3u) || bytes > std::numeric_limits<std::uint32_t>::max() - vertex_address ||
        !memory_.contains(vertex_address, static_cast<std::size_t>(bytes)))
        throw psprecomp::Error("Invalid clear vertex buffer");

    auto preserve = data(registers[0xE8]) | ((registers[0xE9] & 255u) << 24);
    if (!(flags & 1u)) preserve |= 0x00FFFFFFu;
    if (!(flags & 2u)) preserve |= 0xFF000000u;
    const bool color_write = preserve != 0xFFFFFFFFu, depth_write = (flags & 4u) != 0;
    const auto color_base = color_write ? buffer_offset(registers[0x9C], registers[0x9D]) : 0u;
    const auto depth_base = depth_write ? buffer_offset(registers[0x9E], registers[0x9F]) : 0u;
    const auto color_stride = registers[0x9D] & 0xFFFFu, depth_stride = registers[0x9F] & 0xFFFFu;
    if ((color_write && (!color_stride || color_stride > 1024 || (color_stride & 63u))) ||
        (depth_write && (!depth_stride || depth_stride > 1024 || (depth_stride & 63u))))
        throw psprecomp::Error("Unsupported clear buffer stride");
    const auto scissor0 = data(registers[0xD4]), scissor1 = data(registers[0xD5]);
    const auto region_end = data(registers[0x16]);
    const auto clip_x0 = scissor0 & 1023u, clip_y0 = (scissor0 >> 10) & 1023u;
    const auto clip_x1 = std::min(scissor1 & 1023u, region_end & 1023u) + 1;
    const auto clip_y1 = std::min((scissor1 >> 10) & 1023u, (region_end >> 10) & 1023u) + 1;

    // Decode and validate the complete submission before touching any target.
    // A malformed later pair must not leave a partially completed clear behind.
    std::vector<Rectangle> rectangles;
    rectangles.reserve(count / 2);
    std::uint64_t pixels = 0;
    for (std::uint32_t i = 0; i < count; i += 2) {
        const auto first = vertex_address + i * vertex_stride, second = first + vertex_stride;
        const auto ax = memory_.load16(first + 4), ay = memory_.load16(first + 6);
        const auto bx = memory_.load16(second + 4), by = memory_.load16(second + 6);
        // This first subset accepts the nonnegative through coordinates used
        // by the observed clears; signed/wrapped coordinates remain explicit.
        if (ax > 4096 || ay > 4096 || bx > 4096 || by > 4096)
            throw psprecomp::Error("Unsupported wrapped clear sprite coordinates");
        Rectangle rect{
            std::max<std::uint32_t>(std::min(ax, bx), clip_x0),
            std::max<std::uint32_t>(std::min(ay, by), clip_y0),
            std::min<std::uint32_t>(std::max(ax, bx), clip_x1),
            std::min<std::uint32_t>(std::max(ay, by), clip_y1),
            memory_.load32(second), memory_.load16(second + 8)};
        if (rect.x0 >= rect.x1 || rect.y0 >= rect.y1) continue;
        pixels += static_cast<std::uint64_t>(rect.x1 - rect.x0) * (rect.y1 - rect.y0);
        if (pixels > pixel_budget) throw psprecomp::Error("Clear raster pixel budget exceeded");
        const auto last_color = static_cast<std::uint64_t>(color_base) +
            (static_cast<std::uint64_t>(rect.y1 - 1) * color_stride + rect.x1) * 4;
        const auto last_depth = static_cast<std::uint64_t>(depth_base) +
            (static_cast<std::uint64_t>(rect.y1 - 1) * depth_stride + rect.x1) * 2;
        if ((color_write && last_color > vram_size) || (depth_write && last_depth > vram_size))
            throw psprecomp::Error("Clear target range is outside VRAM");
        rectangles.push_back(rect);
    }
    if (recorder_ && color_write && !rectangles.empty()) {
        auto &building = recorder_->target(registers);
        for (const auto &rect : rectangles) {
            auto draw = SceneRecorder::state(registers, SceneDraw::Kind::clear, true, static_cast<std::int32_t>(clip_x0),
                static_cast<std::int32_t>(clip_y0), static_cast<std::int32_t>(clip_x1), static_cast<std::int32_t>(clip_y1));
            draw.textured = false; draw.blend = false;
            draw.clear_color = rect.color; draw.clear_preserve = preserve;
            draw.first = static_cast<std::uint32_t>(building.scene->vertices.size()); draw.count = 2;
            SceneVertex first, second;
            first.x = static_cast<float>(rect.x0); first.y = static_cast<float>(rect.y0);
            second.x = static_cast<float>(rect.x1); second.y = static_cast<float>(rect.y1);
            building.scene->vertices.push_back(first); building.scene->vertices.push_back(second);
            building.scene->draws.push_back(draw);
        }
    }
    Result result{count, vertex_address + static_cast<std::uint32_t>(bytes), 0, 0};
    std::uint8_t *raw = nullptr;
    if (!rectangles.empty() && (color_write || depth_write)) {
        const auto &first = rectangles.front();
        const auto first_address = color_write
            ? vram_base + color_base + (first.y0 * color_stride + first.x0) * 4
            : vram_base + depth_offset(depth_base + (first.y0 * depth_stride + first.x0) * 2);
        // Consume the lazy watcher exactly where the first validated store would.
        // Empty, fully masked and rejected submissions must not latch it.
        if (memory_.prepare_unwatched_write(first_address, color_write ? 4u : 2u))
            raw = memory_.raw_pointer(vram_base, vram_size);
    }
    if (raw) {
        for (const auto &rect : rectangles) {
            const auto width = rect.x1 - rect.x0;
            // With depth enabled, color/depth may alias after the hardware
            // permutation. Keep the original color-then-depth pixel order.
            if (color_write && !depth_write && !preserve && !(registers[0x20] & 1u)) {
                std::array<std::uint8_t, 4096> row;
                for (std::uint32_t x = 0; x < width; ++x)
                    for (unsigned byte = 0; byte < 4; ++byte)
                        row[x * 4 + byte] = static_cast<std::uint8_t>(rect.color >> (byte * 8));
                for (auto y = rect.y0; y < rect.y1; ++y)
                    std::memcpy(raw + color_base + (y * color_stride + rect.x0) * 4, row.data(), width * 4);
                result.color_pixels += static_cast<std::uint64_t>(width) * (rect.y1 - rect.y0);
                continue;
            }
            for (auto y = rect.y0; y < rect.y1; ++y) {
                for (auto x = rect.x0; x < rect.x1; ++x) {
                    if (color_write) {
                        auto *p = raw + color_base + (y * color_stride + x) * 4;
                        const auto old = static_cast<std::uint32_t>(p[0]) |
                            (static_cast<std::uint32_t>(p[1]) << 8) |
                            (static_cast<std::uint32_t>(p[2]) << 16) |
                            (static_cast<std::uint32_t>(p[3]) << 24);
                        const auto next = dither(rect.color, registers, x, y);
                        const auto value = (old & preserve) | (next & ~preserve);
                        for (unsigned byte = 0; byte < 4; ++byte)
                            p[byte] = static_cast<std::uint8_t>(value >> (byte * 8));
                        ++result.color_pixels;
                    }
                    if (depth_write) {
                        const auto linear = depth_base + (y * depth_stride + x) * 2;
                        auto *p = raw + depth_offset(linear);
                        p[0] = static_cast<std::uint8_t>(rect.depth);
                        p[1] = static_cast<std::uint8_t>(rect.depth >> 8);
                        ++result.depth_pixels;
                    }
                }
            }
        }
        return result;
    }
    for (const auto &rect : rectangles) {
        for (auto y = rect.y0; y < rect.y1; ++y) {
            for (auto x = rect.x0; x < rect.x1; ++x) {
                if (color_write) {
                    const auto address = vram_base + color_base + (y * color_stride + x) * 4;
                    const auto old = memory_.load32(address);
                    const auto next = dither(rect.color, registers, x, y);
                    memory_.store32(address, (old & preserve) | (next & ~preserve));
                    ++result.color_pixels;
                }
                if (depth_write) {
                    const auto linear = depth_base + (y * depth_stride + x) * 2;
                    memory_.store16(vram_base + depth_offset(linear), rect.depth);
                    ++result.depth_pixels;
                }
            }
        }
    }
    return result;
}
}
