#include "badman_raster.hpp"
#include "badman_clut.hpp"
#include "psprecomp/common.hpp"
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t vertices = 0x08810000u, color = 0x04020000u, depth = 0x04040000u;
constexpr std::uint32_t texture = 0x08820000u;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    GuestMemory memory;
    badman::Raster raster{memory};
    std::array<std::uint32_t, 256> regs{};
    badman::Raster::Matrices matrices{};
    Fixture() {
        memory.copy_in(GuestMemory::kVramPhysicalBase, std::vector<std::uint8_t>(GuestMemory::kVramSize, 0xA5));
        regs[0x12] = 0x1280011Cu; regs[0x16] = 63u | (63u << 10);
        regs[0x9C] = color - GuestMemory::kVramPhysicalBase; regs[0x9D] = 64;
        regs[0x9E] = depth - GuestMemory::kVramPhysicalBase; regs[0x9F] = 64;
        regs[0xD2] = 3; regs[0xD3] = 0x701; regs[0xD5] = 63u | (63u << 10);
    }
    void vertex(std::uint32_t index, std::uint16_t x, std::uint16_t y,
                std::uint32_t rgba, std::uint16_t z = 0x3456) {
        const auto at = vertices + index * 12;
        memory.store32(at, rgba); memory.store16(at + 4, x); memory.store16(at + 6, y);
        memory.store16(at + 8, z); memory.store16(at + 10, 0xDEAD); // padding must not be decoded as depth
    }
    std::uint32_t pixel(std::uint32_t x, std::uint32_t y) { return memory.load32(color + (y * 64 + x) * 4); }
    void ordinary() {
        regs[0x12] = 0x1280019F; regs[0xD3] = 0; regs[0x1C] = 1; regs[0x1E] = 1;
        regs[0xA0] = texture & 0xFFFFFF; regs[0xA8] = 0x080002;
        regs[0xB8] = 0x101; regs[0xC3] = 3; regs[0xC7] = 0x101; regs[0xC9] = 0x100;
        regs[0xDF] = 0x32;
    }
    void textured_vertex(std::uint32_t index, float x, float y, float u, float v,
                         std::uint32_t rgba = 0xFFFFFFFF) {
        const auto at = vertices + index * 24;
        memory.store32(at, std::bit_cast<std::uint32_t>(u));
        memory.store32(at + 4, std::bit_cast<std::uint32_t>(v));
        memory.store32(at + 8, rgba);
        memory.store32(at + 12, std::bit_cast<std::uint32_t>(x));
        memory.store32(at + 16, std::bit_cast<std::uint32_t>(y));
        memory.store32(at + 20, 0);
    }
    void colored_sprite_vertex(std::uint32_t index, float x, float y, std::uint32_t rgba) {
        const auto at = vertices + index * 16;
        memory.store32(at, rgba);
        memory.store32(at + 4, std::bit_cast<std::uint32_t>(x));
        memory.store32(at + 8, std::bit_cast<std::uint32_t>(y));
        memory.store32(at + 12, 0);
    }
    void texture_colors(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        memory.store32(texture, a); memory.store32(texture + 4, b);
        memory.store32(texture + 8, c); memory.store32(texture + 12, d);
    }
    void float_register(unsigned id, float value) { regs[id] = std::bit_cast<std::uint32_t>(value) >> 8; }
    void transformed() {
        ordinary(); regs[0x12] = 0x1E3; regs[0x1C] = 1;
        regs[0x55] = 0xFFFFFF; regs[0x58] = 255; regs[0xD7] = 65535;
        for (auto index : {1,2}) for (unsigned axis=0;axis<3;++axis)
            matrices[index][axis*3+axis] = std::bit_cast<std::uint32_t>(1.0f);
        for (unsigned axis=0;axis<4;++axis) matrices[3][axis*4+axis] = std::bit_cast<std::uint32_t>(1.0f);
        float_register(0x42,1);float_register(0x43,-1);
        float_register(0x45,1);float_register(0x46,1);
        float_register(0x48,1);float_register(0x49,1);
    }
    void transformed_vertex(unsigned index,float x,float y,float u,float v,float z=0) {
        const auto at=vertices+index*32;
        const std::array<float,8> values{u,v,0,0,1,x,y,z};
        for(unsigned word=0;word<values.size();++word)memory.store32(at+word*4,std::bit_cast<std::uint32_t>(values[word]));
    }
    void quad() {
        transformed_vertex(0,-1,1,0,0);transformed_vertex(1,-1,-1,0,1);
        transformed_vertex(2,1,1,1,0);transformed_vertex(3,1,-1,1,1);
    }
};
void color_flags_and_masks() {
    Fixture test;
    test.regs[0xD3] = 0x101; // RGB only; preserve stencil/alpha.
    test.regs[0x4C] = 0x7100; test.regs[0x4D] = 0x7780; // Through coordinates bypass transform offsets.
    test.vertex(0, 2, 3, 0x11223344); test.vertex(1, 6, 6, 0xC3ABCDEF);
    const auto result = test.raster.draw(0x04060002u, test.regs, vertices);
    require(result.vertices == 2 && result.next_vertex_address == vertices + 24 &&
            result.color_pixels == 12 && result.depth_pixels == 0, "Clear did not consume/complete the actual sprite");
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x)
            require(test.pixel(x, y) == ((x >= 2 && x < 6 && y >= 3 && y < 6) ? 0xA5ABCDEFu : 0xA5A5A5A5u),
                    "RGB clear coverage, last-vertex color or alpha preservation is wrong");
    require(test.memory.load16(depth + 0x2040) == 0xA5A5, "RGB-only clear changed depth");

    test.vertex(0, 0, 0, 0); test.vertex(1, 1, 1, 0x3CABCDEF);
    test.memory.store32(color, 0x5A406080);
    test.regs[0xD3] = 0x201; test.regs[0xE9] = 0x0F;
    test.raster.draw(0x04060002u, test.regs, vertices);
    require(test.pixel(0, 0) == 0x3A406080, "Stencil-only clear did not preserve RGB and masked stencil bits");
    test.memory.store32(color, 0x5A406080);
    test.regs[0xD3] = 0x101; test.regs[0xE8] = 0x00F00F55;
    test.raster.draw(0x04060002u, test.regs, vertices);
    require(test.pixel(0, 0) == 0x5A4BC0AA, "RGB clear ignored individual E8 write-protect bits");
    test.regs[0xD3] = 0x301; test.regs[0xE8] = 0xFFFFFF; test.regs[0xE9] = 0xFF;
    const auto before = test.memory.vram_bytes();
    const auto masked = test.raster.draw(0x04060002u, test.regs, vertices);
    require(test.memory.vram_bytes() == before && masked.color_pixels == 0 && masked.vertices == 2,
            "Fully masked clear changed pixels or lost vertex consumption");
}
void depth_layout_and_clear_range() {
    Fixture test;
    test.vertex(0, 0, 0, 0xFFFFFFFF, 0x1111); test.vertex(1, 33, 1, 0, 0x3456);
    test.regs[0xD3] = 0x401;
    test.regs[0xD6] = 0x5000; test.regs[0xD7] = 0x6000; // Ordinary depth-range test does not gate a clear.
    const auto result = test.raster.draw(0x04060002u, test.regs, vertices);
    require(result.depth_pixels == 33 && result.color_pixels == 0, "Depth clear did not write every covered pixel");
    // Independent T400 hardware examples for absolute offset0x40000:
    // x0 -> 0x42040; x16 -> 0x42000; x32 -> 0x420C0.
    require(test.memory.load16(0x04042040) == 0x3456 && test.memory.load16(0x04042000) == 0x3456 &&
            test.memory.load16(0x040420C0) == 0x3456 && test.memory.load16(0x040420C2) == 0xA5A5,
            "Depth clear did not use the documented raw EDRAM layout/bounds");
    require(test.memory.load16(depth) == 0xA5A5 && test.pixel(0, 0) == 0xA5A5A5A5,
            "Depth clear used linear storage or changed color");
}
void clipping_and_reversed_corners() {
    Fixture test;
    test.regs[0xD3] = 0x301;
    test.regs[0xD4] = 2 | (3 << 10); test.regs[0xD5] = 7 | (8 << 10); test.regs[0x16] = 5 | (6 << 10);
    test.vertex(0, 10, 10, 0); test.vertex(1, 0, 0, 0x87654321);
    const auto result = test.raster.draw(0x04060002u, test.regs, vertices);
    require(result.color_pixels == 16, "Clear did not intersect reversed corners, scissor and draw region");
    for (std::uint32_t y = 0; y < 10; ++y)
        for (std::uint32_t x = 0; x < 10; ++x)
            require(test.pixel(x, y) == ((x >= 2 && x <= 5 && y >= 3 && y <= 6) ? 0x87654321u : 0xA5A5A5A5u),
                    "Clear scissor/region uses incorrect inclusive endpoints");
    test.regs[0xD4] = 9 | (9 << 10); test.regs[0xD5] = 8 | (8 << 10);
    const auto before = test.memory.vram_bytes();
    const auto empty = test.raster.draw(0x04060002u, test.regs, vertices);
    require(test.memory.vram_bytes() == before && empty.color_pixels == 0 && empty.next_vertex_address == vertices + 24,
            "Empty scissor changed pixels or failed to consume vertices");
}
void rejects_before_writing() {
    Fixture test;
    test.vertex(0, 0, 0, 0); test.vertex(1, 1, 1, 0);
    const auto before = test.memory.vram_bytes();
    const auto reject = [&](std::uint32_t command, std::uint32_t address = vertices) {
        bool rejected = false;
        try { (void)test.raster.draw(command, test.regs, address); } catch (const Error &) { rejected = true; }
        require(rejected && test.memory.vram_bytes() == before, "Unsupported clear changed VRAM or fabricated completion");
    };
    reject(0x04030002); reject(0x04060003);
    test.regs[0x12] = 0x0080011E; reject(0x04060002); test.regs[0x12] = 0x0080011C;
    test.regs[0xD2] = 0; reject(0x04060002); test.regs[0xD2] = 3;
    test.regs[0xD3] = 0; reject(0x04060002); test.regs[0xD3] = 0x701;
    test.regs[0x15] = 1; reject(0x04060002); test.regs[0x15] = 0;
    test.regs[0xE7] = 1; reject(0x04060002); test.regs[0xE7] = 0;
    reject(0x04060002, 0x09FFFFF0); reject(0x04060002, vertices + 1);
    test.vertex(1, 0xFFFF, 1, 0); reject(0x04060002); test.vertex(1, 1, 1, 0);
    // First pair fits, later pair exceeds VRAM: no partial first-pair clear.
    test.regs[0xD3] = 0x301; test.regs[0x9C] = 0x1FE000; test.regs[0x9D] = 1024;
    test.regs[0xD5] = 1023 | (1023 << 10); test.regs[0x16] = test.regs[0xD5];
    test.vertex(2, 0, 5, 0); test.vertex(3, 1, 6, 0);
    reject(0x04060004);
}
void striped_frame_clear() {
    Fixture test;
    test.regs[0x9C] = 0x88000; test.regs[0x9D] = 512;
    test.regs[0x9E] = 0x110000; test.regs[0x9F] = 512;
    test.regs[0x16] = 479 | (271 << 10); test.regs[0xD5] = test.regs[0x16];
    for (std::uint16_t stripe = 0; stripe != 16; ++stripe) {
        test.vertex(stripe * 2, stripe * 32, 0, 0, 0);
        test.vertex(stripe * 2 + 1, (stripe + 1) * 32, 272, 0, 0);
    }
    const auto result = test.raster.draw(0x04060020u, test.regs, vertices);
    require(result.vertices == 32 && result.next_vertex_address == vertices + 384 &&
            result.color_pixels == 480 * 272 && result.depth_pixels == 480 * 272,
            "Strip clear failed to account for actual displayed coverage and pointer advance");
    for (std::uint32_t y = 0; y < 272; ++y)
        for (std::uint32_t x = 0; x < 512; ++x)
            require(test.memory.load32(0x04088000 + (y * 512 + x) * 4) == (x < 480 ? 0u : 0xA5A5A5A5u),
                    "Strip clear changed padding or left visible pixels uncleared");
    require(test.memory.load32(0x04000000) == 0xA5A5A5A5, "Back-buffer clear changed the displayed front buffer");
}
void ordinary_sampling_clipping_and_modulation() {
    Fixture test; test.ordinary();
    test.texture_colors(0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF);
    test.textured_vertex(0, 0, 0, 0, 0, 0); // Only second color applies to a sprite.
    test.textured_vertex(1, 2, 2, 2, 2);
    const auto result = test.raster.draw(0x04060002, test.regs, vertices);
    require(result.vertices == 2 && result.next_vertex_address == vertices + 48 &&
            result.color_pixels == 4 && result.depth_pixels == 0, "Ordinary sprite consumption/count is wrong");
    require(test.pixel(0, 0) == 0xA50000FF && test.pixel(1, 0) == 0xA500FF00 &&
            test.pixel(0, 1) == 0xA5FF0000 && test.pixel(1, 1) == 0xA5FFFFFF &&
            test.pixel(2, 0) == 0xA5A5A5A5, "Texture coordinates/colors or exclusive coverage is wrong");
    require(test.memory.load32(depth) == 0xA5A5A5A5, "Depth-disabled ordinary sprite changed depth");
    test.regs[0xD4] = 1; // Clipping must retain the original interpolated UV origin.
    test.textured_vertex(1, 2, 2, 2, 2, 0xFF7F7F7F);
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(0, 0) == 0xA50000FF && test.pixel(1, 0) == 0xA5007F00 &&
            test.pixel(1, 1) == 0xA57F7F7F, "Clip reset UVs or MODULATE did not use t*(c+1)>>8");
    test.regs[0xD4] = 0;
    test.textured_vertex(0, 0, 0, 2, 0); test.textured_vertex(1, 2, 2, 0, 2);
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(0, 0) == 0xA500FF00 && test.pixel(1, 0) == 0xA50000FF,
            "Reversed UV coordinates did not flip the texture independently of geometry");
    test.regs[0xC6] = 7; // Actual title filter: base-level linear minification, nearest magnification.
    test.textured_vertex(0, 0, 0, 0, 0); test.textured_vertex(1, 1, 1, 2, 2);
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(0, 0) == 0xA57F7F7F, "Minifying sprite did not select the base-level linear filter");
}
void untextured_sprite_layout_coverage_and_disabled_texture() {
    Fixture test; test.ordinary();
    test.regs[0x12] = 0x0080019C; test.regs[0x1E] = 0;
    test.regs[0xA0] = 0xFFFFFF; test.regs[0xA8] = 0xFFFFFF;
    test.regs[0xC3] = test.regs[0xC0] = test.regs[0xC9] = 0xFFFFFF;
    test.colored_sprite_vertex(0, 2, 3, 0xFFFFFFFF);
    test.colored_sprite_vertex(1, 6, 6, 0x80112233);
    test.colored_sprite_vertex(2, 12, 9, 0);
    test.colored_sprite_vertex(3, 10, 7, 0xFF445566);
    const auto result = test.raster.draw(0x04060004, test.regs, vertices);
    require(result.next_vertex_address == vertices + 64 && result.vertices == 4 &&
            result.color_pixels == 16 && result.depth_pixels == 0,
            "Untextured sprites did not consume their real16-byte layout");
    for (unsigned y=0;y<11;++y) for (unsigned x=0;x<14;++x) {
        const auto expected = x>=2&&x<6&&y>=3&&y<6 ? 0xA5112233u :
            x>=10&&x<12&&y>=7&&y<9 ? 0xA5445566u : 0xA5A5A5A5u;
        require(test.pixel(x,y)==expected,"Untextured sprite coverage/color/stencil differs");
    }
    // Exactly32 readable bytes at RAM end must suffice for the pair.
    const auto tail = GuestMemory::kPhysicalBase + test.memory.size() - 32;
    for (unsigned i=0;i<8;++i) test.memory.store32(tail+i*4,test.memory.load32(vertices+i*4));
    require(test.raster.draw(0x04060002,test.regs,tail).next_vertex_address==tail+32,
            "Untextured sprite read nonexistent UV/trailing vertex bytes");

    // A UV-bearing declaration still consumes24 bytes when texturing is off,
    // but inactive NaN/Inf UVs and stale texture configuration are irrelevant.
    test.regs[0x12] = 0x0080019F;
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    test.textured_vertex(0,0,0,nan,std::numeric_limits<float>::infinity(),0);
    test.textured_vertex(1,1,1,nan,nan,0xFF224466);
    require(test.raster.draw(0x04060002,test.regs,vertices).next_vertex_address==vertices+48 &&
            test.pixel(0,0)==0xA5224466,"Disabled texture state still used UVs or changed layout");
}
void untextured_sprite_blend_masks_and_atomic_rejection() {
    Fixture test; test.ordinary(); test.regs[0x12]=0x0080019C; test.regs[0x1E]=0;
    test.regs[0x21]=1; test.regs[0xC9]=0; // Texture function cannot discard primary alpha.
    test.colored_sprite_vertex(0,0,0,0);test.colored_sprite_vertex(1,1,1,0x800000FF);
    test.memory.store32(color,0x5AFF0000);
    test.raster.draw(0x04060002,test.regs,vertices);
    require(test.pixel(0,0)==0x5A7F0080,"Untextured blend did not use primary alpha");
    test.colored_sprite_vertex(2,0,0,0);test.colored_sprite_vertex(3,1,1,0x800000FF);
    test.memory.store32(color,0x5AFF0000);test.raster.draw(0x04060004,test.regs,vertices);
    require(test.pixel(0,0)==0x5A3F00BF,"Untextured overlap blended against stale destination");
    test.regs[0x21]=0;test.regs[0xE8]=0xF00F55;
    test.colored_sprite_vertex(1,1,1,0xFFABCDEF);test.memory.store32(color,0x5A406080);
    test.raster.draw(0x04060002,test.regs,vertices);
    require(test.pixel(0,0)==0x5A4BC0AA,"Untextured RGB mask/stencil preservation differs");
    test.regs[0xE8]=0;test.regs[0x20]=1;test.regs[0xE2]=1;
    test.colored_sprite_vertex(1,1,1,0xFF001022);
    test.raster.draw(0x04060002,test.regs,vertices);
    require(test.pixel(0,0)==0x5A011123,"Untextured sprite omitted signed RGB dither");
    test.regs[0x20]=0;
    const auto before = test.memory.vram_bytes();
    const auto reject = [&](unsigned count=2) {
        bool failed=false;try{test.raster.draw(0x04060000|count,test.regs,vertices);}catch(const Error &){failed=true;}
        require(failed&&test.memory.vram_bytes()==before,"Invalid untextured sprite partially wrote VRAM");
    };
    test.regs[0x1E]=1;reject();test.regs[0x1E]=2;reject();test.regs[0x1E]=0;
    test.colored_sprite_vertex(3,std::numeric_limits<float>::quiet_NaN(),1,0xFFFFFFFF);reject(4);
    test.colored_sprite_vertex(3,1,1,0xFFFFFFFF);
    test.regs[0x9C]=0x1FE000;test.regs[0x9D]=1024;
    test.colored_sprite_vertex(2,0,5,0);test.colored_sprite_vertex(3,1,6,0xFFFFFFFF);reject(4);
}
void observed_untextured_defeat_separator() {
    Fixture test;test.ordinary();test.regs[0x12]=0x0080019C;test.regs[0x1E]=0;
    test.regs[0x9D]=512;test.regs[0x16]=479|(271<<10);test.regs[0xD5]=test.regs[0x16];
    test.colored_sprite_vertex(0,40,170,0xFFFFFFFF);test.colored_sprite_vertex(1,440,172,0xFFFFFFFF);
    const auto result=test.raster.draw(0x04060002,test.regs,vertices);
    require(result.color_pixels==800&&result.next_vertex_address==vertices+32,
            "Observed defeat-menu separator did not draw800 pixels");
    for(unsigned y=169;y<=172;++y)for(unsigned x=39;x<=440;++x)
        require(test.memory.load32(color+(y*512+x)*4)==
            (x>=40&&x<440&&y>=170&&y<172?0xA5FFFFFFu:0xA5A5A5A5u),
            "Observed untextured separator coverage changed adjacent pixels");
}
void ordinary_blending_masks_and_order() {
    Fixture test; test.ordinary(); test.regs[0x21] = 1;
    test.texture_colors(0x800000FF, 0, 0, 0);
    test.textured_vertex(0, 0, 0, 0, 0); test.textured_vertex(1, 1, 1, 0, 0);
    test.memory.store32(color, 0x5AFF0000);
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(0, 0) == 0x5A7F0080, "Alpha blend terms or disabled stencil preservation is wrong");
    test.textured_vertex(2, 0, 0, 0, 0); test.textured_vertex(3, 1, 1, 0, 0);
    test.memory.store32(color, 0x5AFF0000);
    test.raster.draw(0x04060004, test.regs, vertices);
    require(test.pixel(0, 0) == 0x5A3F00BF, "Overlapping sprites blended against stale destination pixels");
    test.regs[0xC9] = 0; // Ignore texture alpha, so opaque primitive replaces RGB.
    test.memory.store32(color, 0x5AFF0000); test.regs[0xE9] = 0x0F;
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(0, 0) == 0x5A0000FF, "RGB-only texture function used texture alpha or changed stencil");
    test.regs[0x21] = 0; test.regs[0xE8] = 0xF00F55;
    test.texture_colors(0xFFABCDEF, 0, 0, 0); test.memory.store32(color, 0x5A406080);
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(0, 0) == 0x5A4BC0AA, "Ordinary RGB write protection ignored individual E8 bits");
    test.regs[0xE8] = 0xFFFFFF;
    const auto before = test.memory.vram_bytes();
    const auto masked = test.raster.draw(0x04060002, test.regs, vertices);
    require(test.memory.vram_bytes() == before && masked.color_pixels == 0 && masked.vertices == 2,
            "Fully masked sprite modified VRAM or failed to consume vertices");
}
void banded_sprites_match_one_thread() {
    // Many overlapping, blended, dithered, textured sprites: the banded helper
    // threads must leave exactly the bytes the single-thread path leaves.
    const auto draw = [](unsigned helpers) {
        Fixture test; test.ordinary(); test.regs[0x21] = 1; test.regs[0x20] = 1;
        test.regs[0xE2] = 0x1F2E; test.regs[0xE3] = 0x3D4C; test.regs[0xE4] = 0x5B6A; test.regs[0xE5] = 0x7988;
        test.regs[0xB8] = 0x404; test.regs[0xA8] = 0x080010; // 16x16 texture, 16-texel rows
        std::uint32_t seed = 0x9E3779B9u;
        const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
        for (std::uint32_t i = 0; i < 16 * 16; ++i) test.memory.store32(texture + i * 4, next());
        constexpr unsigned sprites = 24;
        for (unsigned i = 0; i < sprites; ++i) {
            const auto x = static_cast<float>(next() % 24), y = static_cast<float>(next() % 24);
            const auto w = static_cast<float>(24 + next() % 16), h = static_cast<float>(24 + next() % 16);
            test.textured_vertex(i * 2, x, y, static_cast<float>(next() % 8), static_cast<float>(next() % 8), next() | 0x40000000u);
            test.textured_vertex(i * 2 + 1, x + w, y + h, static_cast<float>(8 + next() % 8), static_cast<float>(8 + next() % 8), next());
        }
        test.raster.set_threads(helpers);
        const auto result = test.raster.draw(0x04060000 | (sprites * 2), test.regs, vertices);
        return std::make_pair(result.color_pixels, test.memory.vram_bytes());
    };
    const auto single = draw(0), banded = draw(3);
    require(single.first >= 16384, "The banded comparison batch is too small to use helper threads");
    require(single == banded, "Banded sprite rasterization differs from the single-thread result");
}
void ordinary_rejects_atomically() {
    Fixture test; test.ordinary();
    test.texture_colors(0xFFFFFFFF, 0, 0, 0);
    test.textured_vertex(0, 0, 0, 0, 0); test.textured_vertex(1, 1, 1, 1, 1);
    const auto before = test.memory.vram_bytes();
    const auto reject = [&](std::uint32_t count = 2) {
        bool rejected = false;
        try { test.raster.draw(0x04060000 | count, test.regs, vertices); } catch (const Error &) { rejected = true; }
        require(rejected && test.memory.vram_bytes() == before, "Unsupported sprite left partial VRAM writes");
    };
    for (const auto state : {0x1F, 0x22, 0x23, 0x24, 0x25, 0x27, 0x28}) {
        test.regs[state] = 1; reject(); test.regs[state] = 0;
    }
    for (const auto state : {0x1D, 0x20}) {
        test.regs[state] = 2; reject(); test.regs[state] = 0;
    }
    test.regs[0x21] = 1; test.regs[0xDF] = 0x632; reject(); test.regs[0xDF] = 0x32;
    test.regs[0xC9] = 0x101; reject(); test.regs[0xC9] = 0x100;
    test.regs[0xC0] = 1; reject(); test.regs[0xC0] = 0;
    test.regs[0xA0] = 0xFFFFFF; reject(); test.regs[0xA0] = texture & 0xFFFFFF;
    test.textured_vertex(2, 0, 0, 0, 0);
    test.textured_vertex(3, std::numeric_limits<float>::infinity(), 1, 1, 1); reject(4);
    test.textured_vertex(3, 1, 1, std::numeric_limits<float>::quiet_NaN(), 1); reject(4);
    test.textured_vertex(3, -4096, 1, 1, 1); reject(4);
    test.textured_vertex(3, 1, 1, 1, 1);
    test.regs[0x9C] = 0x1FE000; test.regs[0x9D] = 1024;
    test.textured_vertex(2, 0, 5, 0, 0); test.textured_vertex(3, 1, 6, 1, 1); reject(4);
}
void observed_transparent_frame_sprite() {
    Fixture test; test.ordinary();
    test.regs[0x9C] = 0x88000; test.regs[0x9D] = 512;
    test.regs[0x16] = 479 | (271 << 10); test.regs[0xD5] = test.regs[0x16];
    test.regs[0xA8] = 0x080008; test.regs[0xB8] = 0x303;
    test.regs[0xC6] = 7; test.regs[0x21] = 1;
    test.regs[0xD6] = 0x1000; test.regs[0xD7] = 0xE000;
    test.memory.copy_in(texture, std::vector<std::uint8_t>(256, 0xFF));
    test.textured_vertex(0, 0, 0, 0, 0, 0); test.textured_vertex(1, 480, 272, 4, 4, 0);
    // The captured first ordinary sprite has zero primary color/alpha. It must
    // execute the blend and consume vertices even though RGB remains black.
    test.memory.copy_in(0x04088000, std::vector<std::uint8_t>(512 * 272 * 4, 0));
    const auto before = test.memory.vram_bytes();
    const auto result = test.raster.draw(0x04060002, test.regs, vertices);
    require(result.vertices == 2 && result.next_vertex_address == vertices + 48 &&
            result.color_pixels == 480 * 272 && result.depth_pixels == 0 &&
            test.memory.vram_bytes() == before, "Captured transparent sprite did not complete faithful coverage/blending");
}
void sprite_backface_culling_is_inert() {
    Fixture test; test.ordinary(); test.regs[0x21] = 1;
    test.texture_colors(0x802D7993, 0xFFA45713, 0xFF826F32, 0xFFADBCE1);
    test.textured_vertex(0, 0, 0, 0, 0); test.textured_vertex(1, 2, 2, 2, 2);
    const auto initial = test.memory.vram_bytes();
    const auto baseline = test.raster.draw(0x04060002, test.regs, vertices);
    const auto expected = test.memory.vram_bytes();
    require(expected != initial && baseline.color_pixels == 4, "Inert-flag fixture must actually draw distinguishable pixels");
    // Compare every VRAM byte and completion count with nontrivial channel
    // values, each cull enable and both front-face directions. Dither remains
    // disabled: a programmed matrix alone must not change any pixels.
    test.regs[0xE2] = 0x7777; test.regs[0xE3] = 0x8888;
    test.regs[0xE4] = 0x7F08; test.regs[0xE5] = 0x170F;
    for (std::uint32_t flags = 0; flags < 2; ++flags) {
        for (std::uint32_t face = 0; face < 2; ++face) {
            test.memory.copy_in(GuestMemory::kVramPhysicalBase, initial);
            test.regs[0x1D] = flags; test.regs[0x9B] = face;
            const auto result = test.raster.draw(0x04060002, test.regs, vertices);
            require(test.memory.vram_bytes() == expected && result.vertices == baseline.vertices &&
                    result.next_vertex_address == baseline.next_vertex_address &&
                    result.color_pixels == baseline.color_pixels && result.depth_pixels == baseline.depth_pixels,
                    "Sprite cull-face flags changed pixel results or completion");
        }
    }
    test.regs[0xD2] = 0;
    bool rejected = false;
    try { test.raster.draw(0x04060002, test.regs, vertices); } catch (const Error &) { rejected = true; }
    require(rejected && test.memory.vram_bytes() == expected, "Cull exemption broadened ordinary framebuffer format support");
}
void dither_8888_signed_offsets_clamp_and_masks() {
    Fixture test; test.ordinary(); test.regs[0x20] = 1;
    test.regs[0xE2] = 0x3210; test.regs[0xE3] = 0x7654;
    test.regs[0xE4] = 0xBA98; test.regs[0xE5] = 0xFEDC;
    test.texture_colors(0xFF03FD10, 0, 0, 0);
    test.textured_vertex(0, 0, 0, 0, 0); test.textured_vertex(1, 4, 4, 0, 0);
    test.raster.draw(0x04060002, test.regs, vertices);
    // Four-bit signed offsets0..7,-8..-1, with independent per-channel
    // saturation and unchanged alpha/stencil, even on an8888 framebuffer.
    constexpr std::uint32_t expected[] = {
        0xA503FD10, 0xA504FE11, 0xA505FF12, 0xA506FF13,
        0xA507FF14, 0xA508FF15, 0xA509FF16, 0xA50AFF17,
        0xA500F508, 0xA500F609, 0xA500F70A, 0xA500F80B,
        0xA500F90C, 0xA500FA0D, 0xA501FB0E, 0xA502FC0F};
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
            require(test.pixel(x, y) == expected[y * 4 + x], "8888 dither signed matrix/row/column/clamp is wrong");
    test.regs[0xD4] = 1 | (1 << 10); // Matrix is anchored to framebuffer position, not clipped primitive origin.
    test.memory.store32(color + (64 + 1) * 4, 0x5A201080);
    test.regs[0xE8] = 0x0000FF;
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(1, 1) == 0x5A08FF80, "Dither reset after clipping or ignored RGB protection/stencil");
    test.regs[0xE8] = 0; test.regs[0xD4] = 0; test.regs[0x21] = 1;
    test.texture_colors(0x800000FF, 0, 0, 0);
    test.memory.store32(color + 4, 0x5AFF0000);
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(1, 0) == 0x5A800181, "Dither was not applied to blended RGB before framebuffer protection");
    test.texture_colors(0x00FFFFFF, 0, 0, 0);
    test.memory.store32(color + 12, 0x5AFE0100);
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(3, 0) == 0x5AFF0403, "Zero-alpha fragments incorrectly skipped post-blend dithering");
    // Clear uses the same RGB dither stage, including when ordinary blending
    // is left enabled; alpha/stencil is selected solely by clear flags.
    test.regs[0x12] = 0x1280011C; test.regs[0xD3] = 0x301;
    test.vertex(0, 0, 0, 0); test.vertex(1, 4, 4, 0xCC03FD10);
    test.raster.draw(0x04060002, test.regs, vertices);
    require(test.pixel(1, 1) == 0xCC08FF15 && test.pixel(0, 2) == 0xCC00F508,
            "Clear8888 failed to dither RGB independently of blending and stencil");
}
void indexed_palette_reaches_pixels_and_rejects_atomically() {
    Fixture test;test.ordinary();test.regs[0xC3]=4;test.regs[0xC5]=0xFF03;
    test.regs[0xB0]=0x030000;test.regs[0xB1]=0x080000;
    test.memory.store32(0x08030000,0xFF0000FF);test.memory.store32(0x08030004,0xFF00FF00);
    badman::Clut palette;palette.load(test.memory,test.regs,1);
    test.memory.store8(texture,0x10);
    test.textured_vertex(0,0,0,0,0);test.textured_vertex(1,2,1,2,1);
    test.raster.draw(0x04060002,test.regs,vertices,&palette);
    require(test.pixel(0,0)==0xA50000FF&&test.pixel(1,0)==0xA500FF00,"Raster did not pass the latched CLUT into indexed sampling");
    const auto before=test.memory.vram_bytes();test.memory.store8(texture,0x80);
    bool rejected=false;
    try{test.raster.draw(0x04060002,test.regs,vertices,&palette);}catch(const Error &){rejected=true;}
    require(rejected&&test.memory.vram_bytes()==before,"Unloaded later palette entry left an earlier sprite pixel written");
}
void common_blend_factors_and_fixed_color_aliases() {
    Fixture test;test.ordinary();test.regs[0x21]=1;
    test.texture_colors(0x80C04080,0x80C04080,0x80C04080,0x80C04080);
    test.textured_vertex(0,0,0,0,0);test.textured_vertex(1,1,1,0,0);
    // Individually isolate each term with the other factor fixed to zero.
    // Expected RGB values distinguish OTHER_COLOR, source/destination alpha,
    // independent per-channel fixed colors, and term truncation at half steps.
    constexpr std::uint32_t source_rgb[]{0x3C3240,0x830D3F,0x602040,0x5F203F,0x44162D,0x7C2953,0xC02000};
    constexpr std::uint32_t destination_rgb[]{0x3C3240,0x13953F,0x286440,0x28633F,0x1C462D,0x348153,0x283280};
    for(unsigned code=0;code<16;++code) {
        if(code>=6 && code<=9)continue;
        const auto expected=code<6?code:6;
        test.memory.store32(color,0x5A50C880);test.regs[0xE0]=0xFF7F00;test.regs[0xE1]=0;test.regs[0xDF]=0xA0|code;
        test.raster.draw(0x04060002,test.regs,vertices);
        require(test.pixel(0,0)==(0x5A000000|source_rgb[expected]),"Source blend factor/FIX alias or stencil alpha is wrong");
        test.memory.store32(color,0x5A50C880);test.regs[0xE0]=0;test.regs[0xE1]=0x8040FF;test.regs[0xDF]=0xA|(code<<4);
        test.raster.draw(0x04060002,test.regs,vertices);
        require(test.pixel(0,0)==(0x5A000000|destination_rgb[expected]),"Destination blend factor/FIX alias used the wrong color/alpha");
    }
    test.regs[0xDF]=0xAA;test.regs[0xE0]=test.regs[0xE1]=0xFFFFFF;test.memory.store32(color,0x5A50C880);
    test.raster.draw(0x04060002,test.regs,vertices);
    require(test.pixel(0,0)==0x5AFFFFFF,"ADD did not saturate only after combining both full terms");
}
void blend_equations_reach_all_raster_paths() {
    constexpr std::uint32_t expected[]{0x4FC77F,0x290001,0x006300,0x504080,0xC0C880,0x708800};
    for(unsigned path=0;path<3;++path) {
        Fixture test;
        if(!path){test.ordinary();test.textured_vertex(0,0,0,0,0);test.textured_vertex(1,2,2,0,0);}
        else if(path==1){test.transformed();test.regs[0x12]=0x19F;test.textured_vertex(0,-1,1,0,0);test.textured_vertex(1,1,-1,0,0);}
        else{test.transformed();test.quad();}
        test.regs[0x21]=1;test.texture_colors(0x80C04080,0x80C04080,0x80C04080,0x80C04080);
        for(unsigned equation=0;equation<6;++equation) {
            // MIN/MAX/ABS must bypass factor multiplication, including otherwise
            // unsupported doubled-alpha codes in these irrelevant fields.
            test.regs[0xDF]=(equation<<8)|(equation<3?0x10:0x76);
            test.regs[0xE0]=0xFFFFFF;test.regs[0xE1]=0xFFFFFF;test.memory.store32(color,0x5A50C880);
            const auto result=test.raster.draw(path==2?0x04040004:0x04060002,test.regs,vertices,nullptr,&test.matrices);
            require(test.pixel(0,0)==(0x5A000000|expected[equation]) && result.color_pixels==4,
                    "RGB blend equation differs between sprite/strip paths or applied factors to MIN/MAX/ABS");
        }
    }
}
void subtract_blend_dither_masks_and_atomic_failures() {
    Fixture test;test.ordinary();test.regs[0x21]=1;test.regs[0xDF]=0x110;
    test.texture_colors(0x80C04080,0x80C04080,0x80C04080,0x80C04080);
    test.textured_vertex(0,0,0,0,0);test.textured_vertex(1,1,1,0,0);test.memory.store32(color,0x5A50C880);
    test.regs[0x20]=1;test.regs[0xE2]=7;test.regs[0xE8]=0xFF;test.regs[0xE9]=0xFF;
    test.raster.draw(0x04060002,test.regs,vertices);
    require(test.pixel(0,0)==0x5A300780,"SUBTRACT did not clamp before dither/RGB protection or changed stencil");
    // Reset to the captured Training blend; all failures preserve every pixel.
    test.regs[0x20]=test.regs[0xE8]=test.regs[0xE9]=0;
    const auto before=test.memory.vram_bytes();
    const auto reject=[&]{bool failed=false;try{test.raster.draw(0x04060002,test.regs,vertices);}catch(const Error&){failed=true;}
        require(failed && test.memory.vram_bytes()==before,"Unsupported blend state left partial framebuffer writes");};
    for(const auto invalid:{0x600u,0x700u,0x832u,0x100032u,0xA6u,0x6Au,0xA7u,0x7Au,0xA8u,0x8Au,0xA9u,0x9Au}){test.regs[0xDF]=invalid;reject();}
    test.regs[0xDF]=0x110;test.regs[0xC9]=0x101;reject();test.regs[0xC9]=0x100;
    test.textured_vertex(2,0,0,0,0);test.textured_vertex(3,1,1,std::numeric_limits<float>::quiet_NaN(),0);
    bool failed=false;try{test.raster.draw(0x04060004,test.regs,vertices);}catch(const Error&){failed=true;}
    require(failed && test.memory.vram_bytes()==before,"Later malformed sprite wrote earlier SUBTRACT fragments");
    test.regs[0x21]=0;test.regs[0xDF]=0xFFFEEE;
    test.raster.draw(0x04060002,test.regs,vertices);
    require(test.pixel(0,0)==0x5AC04080,"Disabled blending incorrectly validated its unused equation/factors");
}
void transformed_sprite_matrix_uv_color_and_clip() {
    Fixture test; test.transformed(); test.regs[0x12]=0x19F;
    test.texture_colors(0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF);
    test.textured_vertex(0,-1,1,0,0,0); test.textured_vertex(1,1,-1,1,1);
    const auto result=test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(result.vertices==2 && result.next_vertex_address==vertices+48 && result.color_pixels==4 && !result.depth_pixels,
            "Transformed sprite lost real vertex consumption/coverage");
    require(test.pixel(0,0)==0xA50000FF && test.pixel(1,0)==0xA500FF00 &&
            test.pixel(0,1)==0xA5FF0000 && test.pixel(1,1)==0xA5FFFFFF,
            "Sprite matrix/viewport/normalized UV or second-vertex color was wrong");
    // Combined world/view translation cancels before the vertex multiply:
    // large intermediate coordinates must not discard the low position bits.
    test.matrices[1][9]=std::bit_cast<std::uint32_t>(65536.0f);
    test.matrices[2][9]=std::bit_cast<std::uint32_t>(-65536.0f);
    test.memory.copy_in(color,std::vector<std::uint8_t>(64*4*4,0xA5));
    test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(0,0)==0xA50000FF,"Sprite staged transforms instead of combining float24 matrices");
    test.matrices[1][9]=std::bit_cast<std::uint32_t>(1.0f);test.matrices[2][9]=0;
    test.regs[0xD4]=2; // Clip the left column after translating; UV must not restart.
    test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(2,0)==0xA500FF00 && test.pixel(2,1)==0xA5FFFFFF,"Sprite clipping reset transformed UV origin");
    test.float_register(0x48,0);test.float_register(0x4A,0.25f);
    test.textured_vertex(1,1,-1,1,1,0xFF808080);
    test.regs[0x55]=0;test.regs[0x58]=0; // Present vertex color supersedes material ambient.
    test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(2,0)==0xA5000080 && test.pixel(2,1)==0xA5800000,"Sprite UV offset or vertex color modulation was wrong");
}
void sprite_reversed_axes_and_rotated_coverage() {
    Fixture test;test.ordinary();test.texture_colors(0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF);
    test.textured_vertex(0,2,2,0,0);test.textured_vertex(1,0,0,2,2);
    auto result=test.raster.draw(0x04060002,test.regs,vertices);
    require(result.color_pixels==4 && test.pixel(0,0)==0xA5FFFFFF && test.pixel(1,0)==0xA5FF0000 &&
            test.pixel(0,1)==0xA500FF00 && test.pixel(1,1)==0xA50000FF,"Both reversed sprite axes lost endpoint UV association");
    test.textured_vertex(0,2,0,0,0);test.textured_vertex(1,0,2,2,2);
    test.raster.draw(0x04060002,test.regs,vertices);
    require(test.pixel(0,0)==0xA5FF0000 && test.pixel(1,0)==0xA50000FF &&
            test.pixel(0,1)==0xA5FFFFFF && test.pixel(1,1)==0xA500FF00,"Opposite sprite axes failed to rotate the UV planes");
    test.textured_vertex(0,2,0.5625f,0,0);test.textured_vertex(1,0,2.5625f,2,2);
    result=test.raster.draw(0x04060002,test.regs,vertices);
    require(result.color_pixels==6,"Rotated sprite first-row subpixel bias was not column-style rounding");
}
void transformed_sprite_blend_and_clip_enable() {
    Fixture test;test.transformed();test.regs[0x12]=0x19F;test.regs[0x21]=1;
    test.texture_colors(0x800000FF,0x800000FF,0x800000FF,0x800000FF);
    test.textured_vertex(0,-1,1,0,0);test.textured_vertex(1,1,-1,1,1);
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)test.memory.store32(color+(y*64+x)*4,0x5AFF0000);
    test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(0,0)==0x5A7F0080 && test.pixel(1,1)==0x5A7F0080,"Transformed sprite bypassed alpha blend or modified stencil");
    const auto before=test.memory.vram_bytes();
    test.memory.store32(vertices+20,std::bit_cast<std::uint32_t>(-0.5f));test.float_register(0x44,100);
    test.regs[0x1C]=0;
    auto result=test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(!result.color_pixels && test.memory.vram_bytes()==before,"Unclipped first-corner negative screen depth did not reject sprite");
    test.regs[0x1C]=1;
    result=test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(result.color_pixels==4,"Clipping-enabled rectangle depth did not clamp its first corner");
}
void transformed_sprite_depth_guard_and_atomic_rejection() {
    Fixture test;test.transformed();test.regs[0x12]=0x19F;test.texture_colors(0xFFFFFFFF,0,0,0);
    test.textured_vertex(0,-1,1,0,0);test.textured_vertex(1,1,-1,0,0);
    test.memory.store32(vertices+20,std::bit_cast<std::uint32_t>(0.5f));
    test.regs[0xD7]=0; // Only the second corner supplies fragment depth.
    auto result=test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(result.color_pixels==4,"First-corner depth incorrectly supplied sprite fragment depth");
    const auto before=test.memory.vram_bytes();
    test.regs[0xD6]=1;
    result=test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(!result.color_pixels && test.memory.vram_bytes()==before,"Transformed sprite ignored MinZ without depth comparison");
    test.regs[0xD6]=0;test.regs[0xD7]=65535;
    test.matrices[1][9]=std::bit_cast<std::uint32_t>(3.0f);
    result=test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(!result.color_pixels && test.memory.vram_bytes()==before,"Sprite ignored both-corners clip-plane rejection");
    test.matrices[1][9]=0;test.float_register(0x45,4096);
    result=test.raster.draw(0x04060002,test.regs,vertices,nullptr,&test.matrices);
    require(!result.color_pixels && test.memory.vram_bytes()==before,"Sprite ignored viewport guard band");
    test.float_register(0x45,1);
    const auto reject=[&](unsigned count=2){ bool failed=false;
        try{test.raster.draw(0x04060000|count,test.regs,vertices,nullptr,&test.matrices);}catch(const Error&){failed=true;}
        require(failed && test.memory.vram_bytes()==before,"Unsupported later transformed sprite wrote partial target pixels");};
    test.matrices[3][15]=std::bit_cast<std::uint32_t>(2.0f);reject();test.matrices[3][15]=std::bit_cast<std::uint32_t>(1.0f);
    test.textured_vertex(2,-1,1,0,0);test.textured_vertex(3,1,-1,std::numeric_limits<float>::quiet_NaN(),0);reject(4);
    test.regs[0x17]=1;reject();
}
void colored_strip_layout_flat_provoking_vertex_and_uv() {
    Fixture test;test.transformed();test.regs[0x12]=0x19F;test.regs[0x55]=test.regs[0x58]=0;
    const auto quad=[&](const std::array<std::uint32_t,4> &colors){
        test.textured_vertex(0,-1,1,0,0,colors[0]);test.textured_vertex(1,-1,-1,0,1,colors[1]);
        test.textured_vertex(2,1,1,1,0,colors[2]);test.textured_vertex(3,1,-1,1,1,colors[3]);};
    quad({0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF});test.texture_colors(0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF);
    auto result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.vertices==4 && result.next_vertex_address==vertices+96 && result.color_pixels==4 && !result.depth_pixels,
            "Colored strip did not decode/consume four24-byte vertices");
    require(test.pixel(0,0)==0xA5FF0000 && test.pixel(1,0)==0xA5FFFFFF && test.pixel(0,1)==0xA5FFFFFF && test.pixel(1,1)==0xA5FFFFFF,
            "Flat strip did not preserve each triangle's last submitted color through odd winding/screen swaps");
    // A fresh target prevents stale pixels concealing a lost triangle.
    test.memory.copy_in(color,std::vector<std::uint8_t>(64*4*4,0xA5));
    test.textured_vertex(0,-1,1,0,0,0xFF0000FF);test.textured_vertex(1,1,1,1,0,0xFF00FF00);
    test.textured_vertex(2,-1,-1,0,1,0xFFFF0000);test.textured_vertex(3,1,-1,1,1);
    test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(0,0)==0xA5FF0000 && test.pixel(1,1)==0xA5FFFFFF,"Opposite strip winding changed its provoking color");
    quad({0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF});test.texture_colors(0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF);
    test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(0,0)==0xA50000FF && test.pixel(1,0)==0xA500FF00 && test.pixel(0,1)==0xA5FF0000 && test.pixel(1,1)==0xA5FFFFFF,
            "Colored strip lost normalized UV/layout mapping or applied zero material color");
}
void colored_strip_smooth_fixed_planes_and_alpha() {
    Fixture test;test.transformed();test.regs[0x12]=0x19F;test.regs[0x50]=1;
    test.texture_colors(0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF);
    const auto quad=[&](const std::array<std::uint32_t,4> &colors){
        test.textured_vertex(0,-1,1,0,0,colors[0]);test.textured_vertex(1,-1,-1,0,1,colors[1]);
        test.textured_vertex(2,1,1,1,0,colors[2]);test.textured_vertex(3,1,-1,1,1,colors[3]);};
    quad({0x80400000,0x80408000,0x80400080,0x80408080});
    auto result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.color_pixels==4 && test.pixel(0,0)==0xA5402020 && test.pixel(1,0)==0xA5402060 &&
            test.pixel(0,1)==0xA5406020 && test.pixel(1,1)==0xA5406060,"Smooth strip ignored/faultily interpolated RGB vertex channels");
    quad({0xFF400000,0xFF40FF00,0xFF4000FF,0xFF40FFFF});
    test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(0,0)==0xA5403F3F && test.pixel(1,0)==0xA5403FBF && test.pixel(0,1)==0xA540BF3F && test.pixel(1,1)==0xA540BFBF,
            "Smooth strip color did not floor fractional plane values at pixel centers");
    quad({0x000000FF,0x000000FF,0x800000FF,0x800000FF});test.regs[0x21]=1;
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)test.memory.store32(color+(y*64+x)*4,0x5AFF0000);
    test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(0,0)==0x5ADF0020 && test.pixel(1,0)==0x5A9F0060 && test.pixel(0,1)==0x5ADF0020 && test.pixel(1,1)==0x5A9F0060,
            "Smooth strip alpha was not interpolated into actual blend factors or stencil was changed");
    const auto before=test.memory.vram_bytes();
    const auto reject=[&]{bool failed=false;try{test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);}catch(const Error&){failed=true;}
        require(failed && test.memory.vram_bytes()==before,"Unsupported colored strip left partial framebuffer writes");};
    quad({0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFF0000FF}); // First triangle uniform; only the later one rejects.
    test.float_register(0x42,1.5f);test.float_register(0x45,1.5f);reject(); // Non-power-of-two setup reciprocal.
    quad({0xFF000000,0xFF000000,0xFF000001,0xFF000001});
    test.float_register(0x42,1024);test.float_register(0x45,1024);reject(); // Exact area, unrepresentable14-bit gradient.
    test.float_register(0x42,1);test.float_register(0x45,1);
    test.regs[0x50]=2;reject();test.regs[0x50]=0;
    test.memory.store32(vertices+3*24+20,std::bit_cast<std::uint32_t>(-1.5f));reject();
    test.memory.store32(vertices+3*24+20,0);test.memory.store32(vertices+3*24,std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));reject();
    quad({0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF});test.regs[0x50]=1;test.regs[0x21]=0;
    test.float_register(0x42,1.5f);test.float_register(0x45,1.5f);
    result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.color_pixels==6,"Uniform smooth colors were incorrectly subjected to varying-plane precision restrictions");
}
void untextured_4444_strip_layout_color_and_guards() {
    Fixture test;test.transformed();test.regs[0x12]=0x198;test.regs[0x1E]=0;
    test.regs[0xA0]=test.regs[0xA8]=test.regs[0xC3]=0xFFFFFF;
    test.regs[0xC0]=test.regs[0xC9]=0xFFFFFF;
    const auto quad=[&](std::array<std::uint32_t,4> colors){
        test.colored_sprite_vertex(0,-1,1,colors[0]);test.colored_sprite_vertex(1,-1,-1,colors[1]);
        test.colored_sprite_vertex(2,1,1,colors[2]);test.colored_sprite_vertex(3,1,-1,colors[3]);
    };
    quad({0xDEAD0000,0xBAD0FFFF,0xDEAD8421,0xA55A3579});
    auto result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.vertices==4&&result.next_vertex_address==vertices+64&&result.color_pixels==4,
            "Untextured4444 strip consumed wrong layout/coverage");
    require(test.pixel(0,0)==0xA5442211&&test.pixel(1,1)==0xA5557799,
            "Packed4444 channels/padding/last-vertex strip colors differ");
    quad({0x800F,0x800F,0x800F,0x800F});test.regs[0x21]=1;
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)test.memory.store32(color+(y*64+x)*4,0x5AFF0000);
    result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.color_pixels==4,"Untextured strip diagonal omitted or double counted");
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)
        require(test.pixel(x,y)==0x5A770088,"Untextured4444 alpha blend/stencil preservation differs");
    quad({0xF0FF,0xF0FF,0xF0FF,0xF0FF});test.regs[0x21]=0;test.regs[0x50]=1;
    test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(0,0)==0x5A00FFFF,"ObservedAlmanac F0FF did not expand to yellowRGBA");
    const auto before=test.memory.vram_bytes();
    const auto reject=[&]{bool failed=false;try{test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);}catch(const Error&){failed=true;}
        require(failed&&test.memory.vram_bytes()==before,"Invalid untextured strip partially changed targets");};
    test.regs[0x1E]=1;reject();test.regs[0x1E]=0;
    test.memory.store32(vertices+3*16+4,std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));reject();
    quad({0xF0FF,0xF0FF,0xF0FF,0xF0FF});test.regs[0x9C]=0x1FE000;test.regs[0x9D]=1024;
    test.float_register(0x46,6);reject();
    test.regs[0x9C]=color-GuestMemory::kVramPhysicalBase;test.regs[0x9D]=64;test.float_register(0x46,1);
    test.regs[0x12]=0x19F;
    const auto nan=std::numeric_limits<float>::quiet_NaN();
    test.textured_vertex(0,-1,1,nan,nan,0xFF123456);test.textured_vertex(1,-1,-1,nan,nan,0xFF123456);
    test.textured_vertex(2,1,1,nan,nan,0xFF123456);test.textured_vertex(3,1,-1,nan,nan,0xFF123456);
    test.float_register(0x48,nan);test.float_register(0x49,nan);
    result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.next_vertex_address==vertices+96&&test.pixel(0,0)==0x5A123456,
            "Untextured UV-bearing transformed strip still used disabled texture/UV state");
}
void transformed_strip_coverage_and_matrix_translation() {
    Fixture test;test.transformed();test.quad();
    test.texture_colors(0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF);
    auto result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.vertices==4&&result.next_vertex_address==vertices+128&&result.color_pixels==4&&result.depth_pixels==0,
            "Transformed strip consumption or shared-edge coverage is wrong");
    require(test.pixel(0,0)==0xA50000FF&&test.pixel(1,0)==0xA500FF00&&
            test.pixel(0,1)==0xA5FF0000&&test.pixel(1,1)==0xA5FFFFFF,
            "Transformed strip matrix/viewport/normalized UV mapping is wrong");
    test.memory.copy_in(color,std::vector<std::uint8_t>(64*4*4,0xA5));
    test.matrices[1][9]=std::bit_cast<std::uint32_t>(1.0f);
    result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.color_pixels==4&&test.pixel(0,0)==0xA5A5A5A5&&test.pixel(1,0)==0xA50000FF&&test.pixel(2,1)==0xA5FFFFFF,
            "World translation was omitted from actual raster coverage");
    test.regs[0xD4]=2;
    result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.color_pixels==2&&test.pixel(2,0)==0xA500FF00,"Triangle clipping reset UV origin or used inclusive end incorrectly");
    // Normalized UV scale/offset belong to the transformed path.
    test.float_register(0x48,0);test.float_register(0x4A,0.75f);
    test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(test.pixel(2,0)==0xA500FF00&&test.pixel(2,1)==0xA5FFFFFF,"Transformed texture scale/offset did not select the intended column");
}
void banded_strips_match_one_thread() {
    // A long random textured strip over the whole target, blended and
    // dithered: helper bands must leave exactly the single-thread bytes.
    const auto draw = [](unsigned helpers) {
        Fixture test; test.transformed();
        test.float_register(0x42, 32); test.float_register(0x43, -32);
        test.float_register(0x45, 32); test.float_register(0x46, 32);
        test.regs[0x21] = 1; test.regs[0x20] = 1;
        test.regs[0xE2] = 0x1F2E; test.regs[0xE3] = 0x3D4C; test.regs[0xE4] = 0x5B6A; test.regs[0xE5] = 0x7988;
        test.texture_colors(0x80FF00FFu, 0xC000FF00u, 0x40FF0000u, 0xFFFFFFFFu);
        std::uint32_t seed = 0x2545F491u;
        const auto unit = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
        constexpr unsigned count = 96;
        for (unsigned i = 0; i < count; ++i)
            test.transformed_vertex(i, unit() * 2 - 1, unit() * 2 - 1, unit(), unit());
        test.raster.set_threads(helpers);
        const auto result = test.raster.draw(0x04040000 | count, test.regs, vertices, nullptr, &test.matrices);
        return std::make_pair(result.color_pixels, test.memory.vram_bytes());
    };
    const auto single = draw(0), banded = draw(3);
    require(single.first >= 4096, "The banded strip comparison is too small to use helper threads");
    require(single == banded, "Banded strip rasterization differs from the single-thread result");
}
void orthographic_varying_depth_without_depth_test() {
    Fixture test;test.transformed();
    const auto quad=[&](float last_z=0.5f){
        test.transformed_vertex(0,-1,1,0,0,-0.5f);test.transformed_vertex(1,-1,-1,0,1,0.25f);
        test.transformed_vertex(2,1,1,1,0,-0.25f);test.transformed_vertex(3,1,-1,1,1,last_z);
    };
    quad();test.float_register(0x44,1000);test.float_register(0x47,2000);
    test.regs[0xD6]=1000;test.regs[0xD7]=3000;
    test.texture_colors(0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF);
    const auto result=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(result.vertices==4&&result.next_vertex_address==vertices+128&&result.color_pixels==4&&result.depth_pixels==0,
            "Bounded orthographic depth variation did not complete realstrip");
    require(test.pixel(0,0)==0xA50000FF&&test.pixel(1,0)==0xA500FF00&&
            test.pixel(0,1)==0xA5FF0000&&test.pixel(1,1)==0xA5FFFFFF,
            "Unused varyingdepth changed affine XY/UV sampling");
    require(test.memory.load32(depth)==0xA5A5A5A5,"Depth-disabled varying strip wrote depth");
    const auto before=test.memory.vram_bytes();
    test.regs[0xD6]=4000;test.regs[0xD7]=5000;
    require(test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices).color_pixels==0&&
            test.memory.vram_bytes()==before,"All-outside depth-range strip should discard");
    const auto reject=[&]{bool failed=false;try{test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);}catch(const Error&){failed=true;}
        require(failed&&test.memory.vram_bytes()==before,"Unsupported depth/clipping variation wrote partialpixels");};
    test.regs[0xD6]=1000;test.regs[0xD7]=2400;reject(); // First triangle valid, later triangle crosses range.
    test.regs[0xD7]=3000;quad(-1.5f);reject();quad(1.5f);reject();quad();
    test.matrices[3][15]=std::bit_cast<std::uint32_t>(2.0f);reject();test.matrices[3][15]=std::bit_cast<std::uint32_t>(1.0f);
    test.regs[0x23]=1;reject();
}
void transformed_strip_edges_blend_depth_and_rejection() {
    Fixture test;test.transformed();test.quad();test.regs[0x21]=1;
    test.texture_colors(0x800000FF,0x800000FF,0x800000FF,0x800000FF);
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)test.memory.store32(color+(y*64+x)*4,0x5AFF0000);
    test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)
        require(test.pixel(x,y)==0x5A7F0080,"Strip shared diagonal was omitted or alpha-blended twice");
    const auto before=test.memory.vram_bytes();
    test.regs[0xD6]=1; // Constant transformed depth0 must fail even with depth comparison disabled.
    const auto clipped=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(clipped.color_pixels==0&&clipped.vertices==4&&test.memory.vram_bytes()==before,"Transformed MinZ range did not discard the primitive");
    test.regs[0xD6]=0;
    const auto reject=[&] {
        bool failed=false;try{test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);}catch(const Error &){failed=true;}
        require(failed&&test.memory.vram_bytes()==before,"Unsupported transformed strip wrote partial pixels");
    };
    for(const auto state:{0x17,0x1D,0x1F,0x22,0x23,0x24,0x25,0x27,0x28}){test.regs[state]=1;reject();test.regs[state]=0;}
    test.matrices[3][15]=std::bit_cast<std::uint32_t>(2.0f);reject();test.matrices[3][15]=std::bit_cast<std::uint32_t>(1.0f);
    test.transformed_vertex(3,1,-1,1,1,-1.5f);reject();test.quad();
    test.transformed_vertex(3,1,-1,std::numeric_limits<float>::quiet_NaN(),1);reject();test.quad();
    test.regs[0x9C]=0x1FE000;test.regs[0x9D]=1024;
    test.float_register(0x46,6);reject();
}
void transformed_strip_degenerate_and_outside_plane() {
    Fixture test;test.transformed();test.texture_colors(0xFFFFFFFF,0,0,0);
    for(unsigned i=0;i<4;++i)test.transformed_vertex(i,0,0,0,0);
    const auto before=test.memory.vram_bytes();
    const auto flat=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(flat.color_pixels==0&&flat.vertices==4&&test.memory.vram_bytes()==before,"Degenerate strip fabricated pixel coverage");
    test.quad();test.matrices[1][9]=std::bit_cast<std::uint32_t>(3.0f);
    const auto outside=test.raster.draw(0x04040004,test.regs,vertices,nullptr,&test.matrices);
    require(outside.color_pixels==0&&test.memory.vram_bytes()==before,"All-X-plane transformed cull was skipped");
}
}
int main() {
    try {
        color_flags_and_masks(); depth_layout_and_clear_range(); clipping_and_reversed_corners();
        rejects_before_writing(); striped_frame_clear(); ordinary_sampling_clipping_and_modulation();
        untextured_sprite_layout_coverage_and_disabled_texture();
        untextured_sprite_blend_masks_and_atomic_rejection();observed_untextured_defeat_separator();
        ordinary_blending_masks_and_order(); banded_sprites_match_one_thread();
        ordinary_rejects_atomically(); observed_transparent_frame_sprite();
        sprite_backface_culling_is_inert(); dither_8888_signed_offsets_clamp_and_masks();
        indexed_palette_reaches_pixels_and_rejects_atomically();
        common_blend_factors_and_fixed_color_aliases();blend_equations_reach_all_raster_paths();
        subtract_blend_dither_masks_and_atomic_failures();
        transformed_sprite_matrix_uv_color_and_clip();sprite_reversed_axes_and_rotated_coverage();
        transformed_sprite_blend_and_clip_enable();transformed_sprite_depth_guard_and_atomic_rejection();
        colored_strip_layout_flat_provoking_vertex_and_uv();colored_strip_smooth_fixed_planes_and_alpha();
        untextured_4444_strip_layout_color_and_guards();
        transformed_strip_coverage_and_matrix_translation();transformed_strip_edges_blend_depth_and_rejection();
        orthographic_varying_depth_without_depth_test(); banded_strips_match_one_thread();
        transformed_strip_degenerate_and_outside_plane();
        std::cout << "Badman validated clear/textured-sprite raster tests passed\n"; return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
