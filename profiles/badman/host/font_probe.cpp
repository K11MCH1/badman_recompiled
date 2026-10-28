#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>

namespace psprecomp { void register_badman_font_functions(Runtime &); }
namespace {
using namespace psprecomp;
constexpr std::uint32_t main_gp = 0x0891F420u, returned = 0x08804010u;
constexpr std::uint32_t allocate_pc = 0x08804020u, free_pc = 0x08804040u, user_data = 0xF07u;
constexpr const char *return_reason = "Font probe guest call returned";
badman::Host *host{};
std::ostream *callback_trace{};
std::map<std::uint32_t, std::uint32_t> callback_allocations;
std::uint64_t allocation_calls{}, free_calls{};
std::uint32_t dispatch_samples{};
void trace_dispatch(Runtime &runtime, AllegrexContext &ctx, std::uint32_t before, std::int32_t) {
    if (dispatch_samples++ >= 400) return;
    *callback_trace << "[probe-dispatch] from=" << hex32(before) << " to=" << hex32(ctx.pc)
        << " a0=" << hex32(ctx.gpr[4]) << " v0=" << hex32(ctx.gpr[2]) << " s0=" << hex32(ctx.gpr[16])
        << " ra=" << hex32(ctx.gpr[31]);
    if (runtime.memory().contains(ctx.gpr[16], 68))
        *callback_trace << " s0+40=" << hex32(runtime.memory().load32(ctx.gpr[16] + 64));
    *callback_trace << '\n';
}
void require(bool value, const std::string &message) { if (!value) throw Error(message); }
void zero(GuestMemory &memory, std::uint32_t address, std::uint32_t bytes, std::uint8_t value = 0) {
    require(memory.contains(address, bytes), "Probe output range is outside guest RAM");
    for (std::uint32_t i = 0; i < bytes; ++i) memory.store8(address + i, value);
}
void text(GuestMemory &memory, std::uint32_t address, const std::string &value) {
    require(memory.contains(address, value.size() + 1), "Probe string is outside guest RAM");
    for (std::size_t i = 0; i <= value.size(); ++i)
        memory.store8(address + static_cast<std::uint32_t>(i), i == value.size() ? 0 : value[i]);
}
void callback_owner(const AllegrexContext &ctx) {
    require(ctx.gpr[28] == main_gp && runtime_thread_uid() == 1 && ctx.gpr[4] == user_data,
            "Font callback lost main GP, PSP thread, or callback user data");
}
void allocate_callback(Runtime &, AllegrexContext &ctx) {
    callback_owner(ctx);
    const auto address = host->allocate(ctx.gpr[5]);
    callback_allocations.emplace(address, ctx.gpr[5]); ++allocation_calls;
    *callback_trace << "[probe-allocate] bytes=" << ctx.gpr[5] << " address=" << hex32(address)
        << " ra=" << hex32(ctx.gpr[31]) << " sp=" << hex32(ctx.gpr[29]) << '\n';
    ctx.gpr[2] = address; ctx.pc = ctx.gpr[31];
}
void free_callback(Runtime &, AllegrexContext &ctx) {
    callback_owner(ctx);
    if (ctx.gpr[5]) {
        require(callback_allocations.erase(ctx.gpr[5]) == 1, "Font freed an unknown callback allocation");
        host->release(ctx.gpr[5]); ++free_calls;
        *callback_trace << "[probe-free] address=" << hex32(ctx.gpr[5]) << " ra=" << hex32(ctx.gpr[31]) << '\n';
    }
    ctx.gpr[2] = 0; ctx.pc = ctx.gpr[31];
}
void return_callback(Runtime &runtime, AllegrexContext &ctx) {
    require(ctx.gpr[28] == main_gp && runtime_thread_uid() == 1, "Font return lost main context ownership");
    runtime.stop(return_reason);
}
class Calls {
public:
    Calls(Runtime &runtime, std::ostream &trace) : runtime_(runtime), trace_(trace), sp_(runtime.cpu().gpr[29]),
        tls_(runtime.cpu().gpr[26]) {}
    std::uint32_t font(std::uint32_t nid, std::initializer_list<std::uint32_t> args, const char *name) {
        require(args.size() <= 8, "Probe supports eight register arguments");
        auto &ctx = runtime_.cpu();
        require(ctx.gpr[29] == sp_ && ctx.gpr[26] == tls_, "Prior font call corrupted stack/TLS");
        std::fill(ctx.gpr.begin() + 4, ctx.gpr.begin() + 12, 0);
        std::copy(args.begin(), args.end(), ctx.gpr.begin() + 4); // Allegrex a0..a3,t0..t3.
        ctx.gpr[31] = returned; ctx.gpr[28] = main_gp; ctx.pc = 0x08804000u;
        runtime_.invoke_import("sceLibFont", nid, ctx);
        require(ctx.pc != 0x08804000u && runtime_.has_function(ctx.pc), std::string(name) + " export was not bound");
        trace_ << "[probe-call] " << name << " entry=" << hex32(ctx.pc) << '\n';
        // run() deliberately resets the previous sentinel stop before dispatch.
        runtime_.run(ctx.pc, 1000000);
        require(runtime_.stopped() && runtime_.stop_reason() == return_reason,
                std::string(name) + " failed: " + runtime_.stop_reason());
        require(ctx.pc == returned && ctx.gpr[29] == sp_ && ctx.gpr[26] == tls_ && runtime_thread_uid() == 1,
                std::string(name) + " did not restore its caller stack/thread/return flow");
        trace_ << "[probe-result] " << name << " v0=" << hex32(ctx.gpr[2]) << '\n';
        return ctx.gpr[2];
    }
private:
    Runtime &runtime_; std::ostream &trace_; std::uint32_t sp_, tls_;
};
}
int main(int argc, char **argv) {
    try {
        if (argc != 5) {
            std::cerr << "Usage: BadmanFontProbe <prepared ELF> <extracted game root> <font directory> <trace>\n";
            return 2;
        }
        std::ofstream trace(argv[4]); require(static_cast<bool>(trace), "Cannot open font probe trace"); callback_trace = &trace;
        auto runtime = std::make_unique<psprecomp::Runtime>(); runtime->set_game_root(argv[2]);
        badman::Host owner(*runtime, 0x08810000u, 0x08810000u, main_gp, trace); host = &owner;
        owner.configure_font_module(argv[1], BADMAN_FONT_SHA256, BADMAN_FONT_SOURCE_SHA256,
            &psprecomp::register_badman_font_functions);
        owner.configure_font_directory(argv[3]); owner.install({});
        runtime->register_function(allocate_pc, allocate_callback, "font_probe_allocator");
        runtime->register_function(free_pc, free_callback, "font_probe_free");
        runtime->register_function(returned, return_callback, "font_probe_return");
        owner.bootstrap(returned, "font probe");
        auto &memory = runtime->memory(); auto &ctx = runtime->cpu();
        const auto request = owner.allocate(256), error = owner.allocate(256), params = owner.allocate(256);
        const auto font_info = owner.allocate(512), char_info = owner.allocate(256), image = owner.allocate(256);
        constexpr std::uint32_t side = 128, pixels = side * side, guard = 64;
        const auto raster_block = owner.allocate(pixels + 2 * guard), raster = raster_block + guard;
        text(memory, request, "disc0:"); ctx.gpr[4] = 1; ctx.gpr[5] = request;
        runtime->invoke_import("sceUmdUser", 0xC6183D47u, ctx);
        text(memory, request, "disc0:/PSP_GAME/USRDIR/data_usa/module/libfont.prx");
        ctx.gpr[4] = request; ctx.gpr[5] = 0; ctx.gpr[6] = 0;
        runtime->invoke_import("ModuleMgrForUser", 0x977DE386u, ctx);
        require(owner.font_loaded(), "Font module did not load: " + runtime->stop_reason());
        const auto uid = ctx.gpr[2];
        ctx.gpr[4] = uid; ctx.gpr[5] = 0; ctx.gpr[6] = 0; ctx.gpr[7] = error; ctx.gpr[8] = 0;
        memory.store32(error, 0xFFFFFFFFu);
        runtime->invoke_import("ModuleMgrForUser", 0x50F0C1ECu, ctx);
        require(owner.font_started() && memory.load32(error) == 0, "Font module did not complete no-entry start");
        // SceFontLibData guest ABI: 11 words, optional cache/file callbacks null.
        // Public PSPSDK declarations plus the locally observed NewLib parameter copy.
        zero(memory, params, 44); memory.store32(params, user_data); memory.store32(params + 4, 4);
        memory.store32(params + 12, allocate_pc); memory.store32(params + 16, free_pc);
        Calls calls(*runtime, trace); const auto available = owner.free_bytes();
        set_runtime_post_dispatch_hook(trace_dispatch);
        memory.store32(error, 0xFFFFFFFFu);
        const auto library = calls.font(0x67F17ED7u, {params, error}, "sceFontNewLib");
        require(memory.load32(error) == 0 && library != 0 && memory.contains(library, 76),
                "sceFontNewLib error=" + hex32(memory.load32(error)));
        for (std::uint32_t offset = 0; offset < 76; offset += 4)
            trace << "[probe-library] offset=" << hex32(offset) << " word=" << hex32(memory.load32(library + offset)) << '\n';
        memory.store32(error, 0xFFFFFFFFu);
        const auto count = calls.font(0x27F6E642u, {library, error}, "sceFontGetNumFontList");
        require(memory.load32(error) == 0 && count > 0 && count <= 128, "No usable fonts enumerated");
        memory.store32(error, 0xFFFFFFFFu);
        const auto font = calls.font(0xA834319Du, {library, 0, 1, error}, "sceFontOpen");
        require(memory.load32(error) == 0 && font != 0 && memory.contains(font, 64),
                "sceFontOpen error=" + hex32(memory.load32(error)));
        zero(memory, font_info, 264);
        require(calls.font(0x0DA7535Eu, {font, font_info}, "sceFontGetFontInfo") == 0, "Font info failed");
        require(memory.load16(font_info + 80) > 0 && memory.load16(font_info + 82) > 0, "Font metrics are empty");
        constexpr std::uint32_t glyph = 'A';
        zero(memory, char_info, 60);
        require(calls.font(0xDCC80C2Fu, {font, glyph, char_info}, "sceFontGetCharInfo") == 0, "Character info failed");
        const auto width = memory.load32(char_info), height = memory.load32(char_info + 4);
        require(width > 0 && height > 0 && width < side - 32 && height < side - 32, "Glyph dimensions are invalid for probe raster");
        zero(memory, raster_block, pixels + 2 * guard, 0xA5); zero(memory, raster, pixels);
        zero(memory, image, 24); memory.store32(image, 2); // 8-bit coverage, not a host-rendered substitute.
        memory.store32(image + 4, 16 * 64); memory.store32(image + 8, 16 * 64);
        memory.store16(image + 12, side); memory.store16(image + 14, side);
        memory.store16(image + 16, side); memory.store32(image + 20, raster);
        require(calls.font(0xCA1E6945u, {font, glyph, image, 0, 0, side, side}, "sceFontGetCharGlyphImage_Clip") == 0,
                "Clipped glyph rasterization failed");
        std::uint32_t nonzero = 0;
        for (std::uint32_t i = 0; i < pixels; ++i) if (memory.load8(raster + i)) ++nonzero;
        for (std::uint32_t i = 0; i < guard; ++i)
            require(memory.load8(raster_block + i) == 0xA5 && memory.load8(raster + pixels + i) == 0xA5,
                    "Glyph rasterization crossed its guarded buffer");
        require(nonzero > 0 && nonzero < pixels, "Translated font returned an empty/invalid glyph raster");
        const std::filesystem::path raster_path = std::string(argv[4]) + ".glyph.pgm";
        std::ofstream out(raster_path, std::ios::binary); out << "P5\n" << side << ' ' << side << "\n255\n";
        for (std::uint32_t i = 0; i < pixels; ++i) out.put(static_cast<char>(memory.load8(raster + i)));
        require(static_cast<bool>(out), "Could not save probe glyph raster"); out.close();
        require(calls.font(0x3AEA8CB6u, {font}, "sceFontClose") == 0, "Font close failed");
        require(calls.font(0x574B6FBCu, {library}, "sceFontDoneLib") == 0, "Font library cleanup failed");
        set_runtime_post_dispatch_hook(nullptr);
        require(callback_allocations.empty() && owner.free_bytes() == available, "Font library leaked callback allocations");
        std::cout << "Translated libfont probe passed: fonts=" << count << " glyph=U+0041 dimensions=" << width << 'x' << height
                  << " nonzero_pixels=" << nonzero << " allocations/frees=" << allocation_calls << '/' << free_calls
                  << " files_opened=" << owner.disc_state().files_opened() << " bytes_read=" << owner.disc_state().bytes_read()
                  << "\nRaster: " << raster_path.string() << '\n';
        return 0;
    } catch (const std::exception &error) { std::cerr << "Font probe failed: " << error.what() << '\n'; return 1; }
}
