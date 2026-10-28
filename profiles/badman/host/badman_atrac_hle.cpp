#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <bit>
#include <ostream>

namespace badman {
namespace {
struct Region { std::uint32_t address; std::uint64_t size; };
Region ram(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t bytes, unsigned alignment = 4) {
    const auto canonical = psprecomp::GuestMemory::canonical(address);
    if (!address || (address & (alignment - 1)) || !bytes || canonical < psprecomp::GuestMemory::kPhysicalBase ||
        static_cast<std::uint64_t>(canonical) + bytes > static_cast<std::uint64_t>(psprecomp::GuestMemory::kPhysicalBase) + memory.size() ||
        !memory.contains(address, bytes)) throw psprecomp::Error("Invalid ATRAC3plus RAM pointer/alignment/extent");
    return {canonical, bytes};
}
void distinct(std::span<const Region> regions) {
    for (std::size_t i = 0; i < regions.size(); ++i) for (std::size_t j = i + 1; j < regions.size(); ++j)
        if (regions[i].address < static_cast<std::uint64_t>(regions[j].address) + regions[j].size &&
            regions[j].address < static_cast<std::uint64_t>(regions[i].address) + regions[i].size)
            throw psprecomp::Error("Overlapping ATRAC3plus output ranges (including cached aliases)");
}
}
void Host::configure_atrac_decoder(const std::filesystem::path &directory) {
    set_atrac_decoder_factory([directory] { return make_ffmpeg_atrac_decoder(directory); });
    trace_ << "[atrac-config] directory=" << directory.string() << '\n';
}
void Host::set_atrac_decoder_factory(Atrac::DecoderFactory factory) {
    if (utility_modules_[0] || utility_modules_[2])
        throw psprecomp::Error("Cannot replace an ATRAC3plus backend while its utility module is loaded");
    atrac_.set_decoder_factory(std::move(factory));
}
void Host::install_atrac() {
    bind("sceAtrac3plus", 0x7A20E7AFu, "sceAtracSetDataAndGetID", [this](Context &ctx) {
        if (in_interrupt_context()) throw psprecomp::Error("ATRAC3plus decode initialization inside an interrupt is unsupported");
        const auto address = ctx.gpr[4], size = ctx.gpr[5];
        if (!size || size > Atrac::max_file_bytes) throw psprecomp::Error("Invalid resident ATRAC3plus input size");
        ram(runtime_.memory(), address, size);
        std::vector<std::uint8_t> bytes(size); runtime_.memory().copy_out(address, bytes);
        ctx.gpr[2] = static_cast<std::uint32_t>(atrac_.set_data_and_get_id(bytes));
        trace_ << "[atrac-stream] bytes=" << size << " result=" << psprecomp::hex32(ctx.gpr[2]) << '\n';
    });
    bind("sceAtrac3plus", 0x61EB33F5u, "sceAtracReleaseAtracID", [this](Context &ctx) {
        ctx.gpr[2] = static_cast<std::uint32_t>(atrac_.release(static_cast<std::int32_t>(ctx.gpr[4])));
        trace_ << "[atrac-release] id=" << ctx.gpr[4] << " result=" << psprecomp::hex32(ctx.gpr[2]) << '\n';
    });
    bind("sceAtrac3plus", 0x6A8C3CD5u, "sceAtracDecodeData", [this](Context &ctx) {
        if (in_interrupt_context()) throw psprecomp::Error("ATRAC3plus decoding inside an interrupt is unsupported");
        const auto id = static_cast<std::int32_t>(ctx.gpr[4]); std::uint32_t count = 0;
        const auto preflight = atrac_.next_sample_count(id, count);
        if (preflight) { ctx.gpr[2] = static_cast<std::uint32_t>(preflight); return; }
        auto &memory = runtime_.memory();
        // PSP EABI argument five is r8, verified in the translated title caller.
        std::array<Region,4> regions{ram(memory, ctx.gpr[6], 4), ram(memory, ctx.gpr[7], 4), ram(memory, ctx.gpr[8], 4), {}};
        if (ctx.gpr[5]) regions[3] = ram(memory, ctx.gpr[5], count * 4, 2);
        distinct(std::span(regions).first(ctx.gpr[5] ? 4 : 3));
        std::array<std::int16_t, Atrac::frame_samples * 2> pcm{}; Atrac::DecodeResult result{};
        const auto status = atrac_.decode(id, ctx.gpr[5] ? std::span(pcm) : std::span<std::int16_t>{}, result);
        if (status) throw psprecomp::Error("ATRAC3plus state changed during single-threaded decode preflight");
        if (ctx.gpr[5]) {
            // Explicit PSP little endian; write only the returned samples.
            std::array<std::uint8_t, Atrac::frame_samples * 4> bytes{};
            for (std::uint32_t i = 0; i < result.samples * 2; ++i) {
                const auto sample = std::bit_cast<std::uint16_t>(pcm[i]);
                bytes[i * 2] = static_cast<std::uint8_t>(sample); bytes[i * 2 + 1] = static_cast<std::uint8_t>(sample >> 8);
            }
            memory.copy_in(ctx.gpr[5], std::span(bytes).first(result.samples * 4));
        }
        memory.store32(ctx.gpr[6], result.samples); memory.store32(ctx.gpr[7], result.ended);
        memory.store32(ctx.gpr[8], static_cast<std::uint32_t>(result.remaining)); ctx.gpr[2] = 0;
        if (result.ended) trace_ << "[atrac-eof] id=" << id << " final_samples=" << result.samples << '\n';
    });
    bind("sceAtrac3plus", 0x9AE849A7u, "sceAtracGetRemainFrame", [this](Context &ctx) {
        std::int32_t value = 0; const auto status = atrac_.get_remain_frame(static_cast<std::int32_t>(ctx.gpr[4]), value);
        if (!status) { ram(runtime_.memory(), ctx.gpr[5], 4); runtime_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(value)); }
        ctx.gpr[2] = static_cast<std::uint32_t>(status);
    });
    bind("sceAtrac3plus", 0x83E85EA0u, "sceAtracGetSecondBufferInfo", [this](Context &ctx) {
        Atrac::SecondBuffer value{}; const auto status = atrac_.get_second_buffer_info(static_cast<std::int32_t>(ctx.gpr[4]), value);
        if (status == Atrac::no_second_buffer) {
            const std::array regions{ram(runtime_.memory(), ctx.gpr[5], 4), ram(runtime_.memory(), ctx.gpr[6], 4)}; distinct(regions);
            runtime_.memory().store32(ctx.gpr[5], value.position); runtime_.memory().store32(ctx.gpr[6], value.bytes);
        }
        ctx.gpr[2] = static_cast<std::uint32_t>(status);
    });
    bind("sceAtrac3plus", 0x868120B5u, "sceAtracSetLoopNum", [this](Context &ctx) {
        ctx.gpr[2] = static_cast<std::uint32_t>(atrac_.set_loop_num(static_cast<std::int32_t>(ctx.gpr[4]), static_cast<std::int32_t>(ctx.gpr[5])));
        trace_ << "[atrac-loop] id=" << ctx.gpr[4] << " loops=" << static_cast<std::int32_t>(ctx.gpr[5])
               << " result=" << psprecomp::hex32(ctx.gpr[2]) << '\n';
    });
    bind("sceAtrac3plus", 0xA2BBA8BEu, "sceAtracGetSoundSample", [this](Context &ctx) {
        Atrac::SoundSample value{}; const auto status = atrac_.get_sound_sample(static_cast<std::int32_t>(ctx.gpr[4]), value);
        if (!status) {
            const std::array regions{ram(runtime_.memory(), ctx.gpr[5], 4), ram(runtime_.memory(), ctx.gpr[6], 4), ram(runtime_.memory(), ctx.gpr[7], 4)};
            distinct(regions); runtime_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(value.end));
            runtime_.memory().store32(ctx.gpr[6], static_cast<std::uint32_t>(value.loop_start));
            runtime_.memory().store32(ctx.gpr[7], static_cast<std::uint32_t>(value.loop_end));
        }
        ctx.gpr[2] = static_cast<std::uint32_t>(status);
    });
    bind("sceAtrac3plus", 0xD6A5F2F7u, "sceAtracGetMaxSample", [this](Context &ctx) {
        std::uint32_t value = 0; const auto status = atrac_.get_max_sample(static_cast<std::int32_t>(ctx.gpr[4]), value);
        if (!status) { ram(runtime_.memory(), ctx.gpr[5], 4); runtime_.memory().store32(ctx.gpr[5], value); }
        ctx.gpr[2] = static_cast<std::uint32_t>(status);
    });
}
}
