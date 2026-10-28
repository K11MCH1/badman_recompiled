#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
using namespace psprecomp;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    const std::filesystem::path parent = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    const std::filesystem::path root = parent / ("badman-io-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { require(std::filesystem::create_directory(root), "Temporary fixture already exists"); }
    ~Fixture() {
        // Only this exact test-owned directory may be removed recursively.
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(root, error);
        if (!error && resolved == root && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("badman-io-test-"))
            std::filesystem::remove_all(resolved, error);
    }
};
void write_font(const std::filesystem::path &path) {
    // Metadata-only PGF fixture: no glyph program or commercial font content.
    std::array<std::uint8_t, 0x188> bytes{};
    bytes[2] = 0x88; bytes[3] = 1;
    bytes[4] = 'P'; bytes[5] = 'G'; bytes[6] = 'F'; bytes[7] = '0';
    bytes[8] = 2; bytes[12] = 6;
    const auto word = [&bytes](std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i != 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    word(0x24, 648); word(0x28, 648); word(0x2C, 8192); word(0x30, 8192);
    const std::string name = "Synthetic IO Test", style = "Regular";
    std::copy(name.begin(), name.end(), bytes.begin() + 0x35);
    std::copy(style.begin(), style.end(), bytes.begin() + 0x75);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    require(bool(out), "Unable to write synthetic font fixture");
}
bool make_sparse(const std::filesystem::path &path, std::uint64_t size) {
    { std::ofstream out(path, std::ios::binary); require(bool(out), "Unable to create sparse fixture"); }
#if defined(_WIN32)
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Unable to open sparse fixture handle");
    DWORD returned = 0;
    const bool sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0;
    CloseHandle(file);
    if (!sparse) { std::cout << "Sparse file unsupported; 64-bit size case skipped\n"; return false; }
#endif
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    if (error) { std::cout << "Sparse resize unavailable: " << error.message() << '\n'; return false; }
#if defined(_WIN32)
    // Verify the >4 GiB logical file did not allocate gigabytes of disk space.
    SetLastError(NO_ERROR);
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
    require(low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR, "Cannot query sparse allocated size");
    require(((static_cast<std::uint64_t>(high) << 32) | low) < 1024 * 1024,
            "Sparse fixture unexpectedly allocated physical disk space");
#endif
    return true;
}
constexpr std::uint32_t path_address = 0x08820000u, output_address = 0x08820404u;
void guest_string(Runtime &runtime, const std::string &text) {
    for (std::size_t i = 0; i <= text.size(); ++i)
        runtime.memory().store8(path_address + static_cast<std::uint32_t>(i),
                               i == text.size() ? 0 : static_cast<std::uint8_t>(text[i]));
}
void fill_output(Runtime &runtime) {
    for (std::uint32_t i = 0; i != 96; ++i) runtime.memory().store8(output_address - 4 + i, 0xA5);
}
void unchanged(Runtime &runtime) {
    for (std::uint32_t i = 0; i != 96; ++i)
        require(runtime.memory().load8(output_address - 4 + i) == 0xA5, "Failed getstat overwrote its output");
}
std::uint32_t query(Runtime &runtime, const std::string &path, std::uint32_t output = output_address) {
    guest_string(runtime, path);
    auto &ctx = runtime.cpu(); ctx.gpr[4] = path_address; ctx.gpr[5] = output;
    runtime.invoke_import("IoFileMgrForUser", 0xACE946E8u, ctx);
    return ctx.gpr[2];
}
void record(Runtime &runtime, std::uint64_t size, bool directory) {
    auto &memory = runtime.memory();
    require(memory.load32(output_address) == ((directory ? 0x1000u : 0x2000u) | 0444u),
            "PSP stat mode or read-only permissions are wrong");
    require(memory.load32(output_address + 4) == (directory ? 0x10u : 0x20u), "PSP stat attr is wrong");
    const auto actual = static_cast<std::uint64_t>(memory.load32(output_address + 8)) |
                        (static_cast<std::uint64_t>(memory.load32(output_address + 12)) << 32);
    require(actual == size, "PSP stat 64-bit file length is wrong");
    for (std::uint32_t i = 16; i != 48; ++i)
        require(memory.load8(output_address + i) == 0, "Unsupported creation/access timestamps were fabricated");
    for (std::uint32_t i = 60; i != 88; ++i)
        require(memory.load8(output_address + i) == 0, "PSP stat unsupported microseconds/device fields were fabricated");
    require(memory.load32(output_address - 4) == 0xA5A5A5A5u && memory.load32(output_address + 88) == 0xA5A5A5A5u,
            "PSP stat wrote outside its exact 88-byte record");
}
void run_case() {
    Fixture fixture;
    const auto user = fixture.root / "PSP_GAME/USRDIR", fonts = fixture.root / "fonts";
    std::filesystem::create_directories(user); std::filesystem::create_directory(fonts);
    { std::ofstream out(fixture.root / "PSP_GAME/PARAM.SFO"); out << "synthetic"; }
    { std::ofstream out(user / "asset.bin", std::ios::binary); out << "abcdef"; }
    write_font(fonts / "ltn0.pgf");
    // A fixed leap-day UTC timestamp catches field offsets and local-time conversion.
    const auto time = std::chrono::sys_days{std::chrono::year{2020}/2/29} +
                      std::chrono::hours{12} + std::chrono::minutes{34} + std::chrono::seconds{56};
    std::error_code time_error;
#if defined(_LIBCPP_VERSION)
    const auto file_time = std::chrono::file_clock::from_sys(time); // libc++ has no clock_cast.
#else
    const auto file_time = std::chrono::clock_cast<std::chrono::file_clock>(time);
#endif
    std::filesystem::last_write_time(user / "asset.bin", file_time, time_error);
    Runtime runtime; runtime.set_game_root(fixture.root);
    std::ostringstream trace;
    badman::Host host(runtime, 0x089F0000u, 0x089E0000u, 0x0891F420u, trace);
    host.configure_font_directory(fonts); host.install({});
    fill_output(runtime);
    require(query(runtime, "flash0:/font/ltn0.pgf") == 0, "Firmware font stat required UMD activation");
    record(runtime, 0x188, false);
    fill_output(runtime);
    require(query(runtime, "disc0:/PSP_GAME/USRDIR/asset.bin") == 0x80010005u,
            "Disc stat did not require ready media");
    unchanged(runtime);
    guest_string(runtime, "disc0:");
    auto &ctx = runtime.cpu(); ctx.gpr[4] = 1; ctx.gpr[5] = path_address;
    runtime.invoke_import("sceUmdUser", 0xC6183D47u, ctx);
    require(ctx.gpr[2] == 0 && host.disc_state().status() == 0x32u, "UMD fixture did not activate");
    fill_output(runtime);
    require(query(runtime, "disc0:/PSP_GAME/USRDIR/asset.bin") == 0, "Ready disc stat failed");
    record(runtime, 6, false);
    if (!time_error) {
        const std::array<std::uint16_t, 6> expected{2020, 2, 29, 12, 34, 56};
        for (std::uint32_t i = 0; i != expected.size(); ++i)
            require(runtime.memory().load16(output_address + 48 + i * 2) == expected[i],
                    "PSP stat modification time did not preserve actual UTC datetime fields");
    } else std::cout << "Fixture timestamp unavailable: " << time_error.message() << '\n';
    fill_output(runtime);
    require(query(runtime, "flash0:/font") == 0, "Directory stat failed");
    record(runtime, 0, true);
    for (const auto *path : {"disc0:/PSP_GAME/USRDIR/missing.bin", "flash0:/font/missing.pgf"}) {
        fill_output(runtime);
        require(query(runtime, path) == 0x80010002u, "Missing getstat file lost PSP ENOENT");
        unchanged(runtime);
    }
    for (const auto pointer : {0u, GuestMemory::kPhysicalBase + runtime.memory().size() - 87}) {
        fill_output(runtime);
        bool rejected = false;
        try { query(runtime, "flash0:/font/ltn0.pgf", pointer); }
        catch (const Error &error) { rejected = std::string(error.what()).find("Invalid PSP stat output buffer") != std::string::npos; }
        require(rejected, "Invalid or truncated 88-byte stat output lacked its diagnostic");
        unchanged(runtime);
    }
    constexpr std::uint64_t large_size = 0x100000123ULL;
    if (make_sparse(user / "large.bin", large_size)) {
        fill_output(runtime);
        require(query(runtime, "disc0:/PSP_GAME/USRDIR/large.bin") == 0, "Large sparse-file stat failed");
        record(runtime, large_size, false);
        std::cout << "Sparse file verified: logical length " << large_size << " bytes, no gigabyte writes\n";
    }
}
void rename_import_case() {
    Fixture fixture;
    const auto source=fixture.root/"source.bin",target=fixture.root/"target.bin";
    {std::ofstream file(source,std::ios::binary);file<<"native rename data";}
    Runtime runtime;std::ostringstream trace;
    badman::Host host(runtime,0x089f0000,0x089e0000,0x0891f420,trace);
    host.configure_memory_stick(fixture.root);host.install({});
    const auto call=[&](const std::string &old_name,const std::string &new_name){
        guest_string(runtime,old_name);
        for(std::size_t i=0;i<=new_name.size();++i)runtime.memory().store8(output_address+static_cast<std::uint32_t>(i),
            i==new_name.size()?0:static_cast<std::uint8_t>(new_name[i]));
        auto &ctx=runtime.cpu();ctx.gpr[4]=path_address;ctx.gpr[5]=output_address;
        runtime.invoke_import("IoFileMgrForUser",0x779103a0u,ctx);return ctx.gpr[2];
    };
    require(call("disc0:/source.bin","disc0:/target.bin")==0x8001001eu&&std::filesystem::exists(source),
        "Rename HLE accepted a read-only disc mutation");
#ifdef _WIN32
    require(call("ms0:/source.bin","ms0:/target.bin")==0&&!std::filesystem::exists(source),
        "Rename HLE did not complete a real file move");
    std::ifstream file(target,std::ios::binary);std::string bytes{std::istreambuf_iterator<char>(file),{}};
    require(bytes=="native rename data","Rename HLE changed file contents");file.close();
    {std::ofstream existing(source,std::ios::binary);existing<<"preserve destination";}
    require(call("ms0:/target.bin","ms0:/source.bin")!=0&&std::filesystem::exists(target),
        "Rename HLE replaced an existing file");
#endif
    const auto memory=runtime.memory().bytes();auto &ctx=runtime.cpu();ctx.gpr[4]=0;ctx.gpr[5]=output_address;
    runtime.invoke_import("IoFileMgrForUser",0x779103a0u,ctx);
    require(ctx.gpr[2]==0x8001000eu&&runtime.memory().bytes()==memory,"Invalid rename input was not rejected without guest writes");
}
}
int main() {
    try { run_case(); rename_import_case(); std::cout << "Badman PSP getstat ABI and native rename tests passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
