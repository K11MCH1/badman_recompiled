#include "badman_host.hpp"
#include "badman_native_fs.hpp"
#include "psprecomp/common.hpp"
#include <bit>
#include <ostream>
#include <ctime>

namespace badman {
using namespace psprecomp;
void Host::install_io() {
    bind("IoFileMgrForUser", 0x779103A0u, "sceIoRename", [this](Context &ctx) {
        if (in_interrupt_context()) throw Error("File rename is invalid inside an interrupt callback");
        ctx.gpr[2] = static_cast<std::uint32_t>(rename_native_file_guest(runtime_.memory(),
            memory_stick_root_, ctx.gpr[4], ctx.gpr[5]));
        trace_ << "[file-rename] result=" << hex32(ctx.gpr[2]) << '\n';
    });
    bind("sceUmdUser", 0xC6183D47u, "sceUmdActivate", [this](Context &ctx) {
        if (ctx.gpr[4] != 1) throw Error("Unsupported UMD unit");
        disc_.activate(string_at(ctx.gpr[5])); ctx.gpr[2] = 0;
    });
    bind("sceUmdUser", 0xE83742BAu, "sceUmdDeactivate", [this](Context &ctx) {
        disc_.deactivate(); ctx.gpr[2] = 0;
    });
    bind("sceUmdUser", 0x6B4A146Cu, "sceUmdGetDriveStat", [this](Context &ctx) { ctx.gpr[2] = disc_.status(); });
    const auto wait = [this](Context &ctx) {
        if ((disc_.status() & ctx.gpr[4]) == 0) throw Error("UMD wait condition is not satisfied by the extracted disc");
        ctx.gpr[2] = 0;
    };
    bind("sceUmdUser", 0x8EF08FCEu, "sceUmdWaitDriveStat", wait);
    bind("sceUmdUser", 0x56202973u, "sceUmdWaitDriveStatWithTimer", wait);
    bind("sceUmdUser", 0x4A9E5E29u, "sceUmdWaitDriveStatCB", wait);
    bind("sceUmdUser", 0x6AF9B50Au, "sceUmdCancelWaitDriveStat", [](Context &ctx) { ctx.gpr[2] = 0; });
    const auto open = [this](Context &ctx, bool async) {
        const auto name = string_at(ctx.gpr[4]);
        const auto fd = disc_.open(name, ctx.gpr[5], async);
        trace_ << "[disc-open] " << name << " fd=" << fd << '\n';
        ctx.gpr[2] = static_cast<std::uint32_t>(fd);
    };
    bind("IoFileMgrForUser", 0x109F50BCu, "sceIoOpen", [open](Context &ctx) { open(ctx, false); });
    bind("IoFileMgrForUser", 0x89AA9906u, "sceIoOpenAsync", [open](Context &ctx) { open(ctx, true); });
    const auto read = [this](Context &ctx, bool async) {
        if (ctx.gpr[6] > runtime_.memory().size() || !runtime_.memory().contains(ctx.gpr[5], ctx.gpr[6]))
            throw Error("Invalid disc read destination/length");
        std::vector<std::uint8_t> bytes;
        const auto result = disc_.read(static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[6], bytes, async);
        if (result >= 0) runtime_.memory().copy_in(ctx.gpr[5], bytes);
        trace_ << "[disc-read] fd=" << static_cast<std::int32_t>(ctx.gpr[4]) << " bytes=" << result
               << " async=" << async << '\n';
        ctx.gpr[2] = static_cast<std::uint32_t>(async && result >= 0 ? 0 : result);
    };
    bind("IoFileMgrForUser", 0x6A638D83u, "sceIoRead", [read](Context &ctx) { read(ctx, false); });
    bind("IoFileMgrForUser", 0xA0B5A7C2u, "sceIoReadAsync", [read](Context &ctx) { read(ctx, true); });
    bind("IoFileMgrForUser", 0x810C4BC3u, "sceIoClose", [this](Context &ctx) {
        ctx.gpr[2] = static_cast<std::uint32_t>(disc_.close(static_cast<std::int32_t>(ctx.gpr[4])));
    });
    bind("IoFileMgrForUser", 0x27EB27B8u, "sceIoLseek", [this](Context &ctx) {
        const auto bits = static_cast<std::uint64_t>(ctx.gpr[6]) | (static_cast<std::uint64_t>(ctx.gpr[7]) << 32);
        const auto result = disc_.seek(static_cast<std::int32_t>(ctx.gpr[4]), std::bit_cast<std::int64_t>(bits), ctx.gpr[8]);
        const auto value = static_cast<std::uint64_t>(result);
        ctx.gpr[2] = static_cast<std::uint32_t>(value); ctx.gpr[3] = static_cast<std::uint32_t>(value >> 32);
    });
    const auto completion = [this](Context &ctx) {
        if (!runtime_.memory().contains(ctx.gpr[5], 8)) throw Error("Invalid asynchronous completion result pointer");
        std::int64_t result = 0;
        const auto status = disc_.completion(static_cast<std::int32_t>(ctx.gpr[4]), result);
        if (status < 0) { ctx.gpr[2] = static_cast<std::uint32_t>(status); return; }
        const auto value = static_cast<std::uint64_t>(result);
        runtime_.memory().store32(ctx.gpr[5], static_cast<std::uint32_t>(value));
        runtime_.memory().store32(ctx.gpr[5] + 4, static_cast<std::uint32_t>(value >> 32));
        ctx.gpr[2] = 0;
    };
    bind("IoFileMgrForUser", 0xE23EEC33u, "sceIoWaitAsync", completion);
    bind("IoFileMgrForUser", 0x35DBD746u, "sceIoWaitAsyncCB", completion);
    bind("IoFileMgrForUser", 0xACE946E8u, "sceIoGetstat", [this](Context &ctx) {
        const auto name = string_at(ctx.gpr[4]);
        const auto out = ctx.gpr[5];
        if (!runtime_.memory().contains(out, 88)) throw Error("Invalid PSP stat output buffer");
        Disc::Stat stat{};
        const auto result = disc_.stat(name, stat);
        if (result < 0) { ctx.gpr[2] = static_cast<std::uint32_t>(result); return; }
        // Serialize the PSP ABI explicitly; Windows native stat layout differs.
        // Creation/access times and device-specific fields remain unspecified 0.
        std::array<std::uint8_t, 88> bytes{};
        runtime_.memory().copy_in(out, bytes);
        runtime_.memory().store32(out, (stat.directory ? 0x1000u : 0x2000u) | 0444u);
        runtime_.memory().store32(out + 4, stat.directory ? 0x10u : 0x20u);
        runtime_.memory().store32(out + 8, static_cast<std::uint32_t>(stat.size));
        runtime_.memory().store32(out + 12, static_cast<std::uint32_t>(stat.size >> 32));
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(disc_.resolve(name), error);
        if (!error) {
#if defined(_LIBCPP_VERSION)
            // libc++ (Android NDK) provides file_clock::to_sys but not clock_cast.
            const auto system = std::chrono::floor<std::chrono::system_clock::duration>(
                std::chrono::file_clock::to_sys(stamp));
#else
            const auto system = std::chrono::clock_cast<std::chrono::system_clock>(stamp);
#endif
            const auto seconds = std::chrono::system_clock::to_time_t(system);
            std::tm utc{};
#if defined(_WIN32)
            const bool valid = gmtime_s(&utc, &seconds) == 0;
#else
            const bool valid = gmtime_r(&seconds, &utc) != nullptr;
#endif
            if (valid) {
                const std::array<std::uint16_t, 6> fields{
                    static_cast<std::uint16_t>(utc.tm_year + 1900), static_cast<std::uint16_t>(utc.tm_mon + 1),
                    static_cast<std::uint16_t>(utc.tm_mday), static_cast<std::uint16_t>(utc.tm_hour),
                    static_cast<std::uint16_t>(utc.tm_min), static_cast<std::uint16_t>(utc.tm_sec)};
                for (std::uint32_t i = 0; i < fields.size(); ++i)
                    runtime_.memory().store16(out + 48 + i * 2, fields[i]);
            }
        }
        trace_ << "[file-stat] " << name << " size=" << stat.size << '\n';
        ctx.gpr[2] = 0;
    });
}
}
