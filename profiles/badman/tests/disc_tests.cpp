#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <chrono>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
using namespace psprecomp;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path parent = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    std::filesystem::path root = parent / ("badman-disc-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Temporary fixture already exists");
        std::filesystem::create_directories(root / "PSP_GAME/USRDIR");
        { std::ofstream out(root / "PSP_GAME/PARAM.SFO", std::ios::binary); out << "synthetic"; }
        { std::ofstream out(root / "PSP_GAME/USRDIR/asset.bin", std::ios::binary); out << "abcdef"; }
    }
    ~Fixture() {
        // Resolve and check the exact test-owned path before recursive cleanup.
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(root, error);
        if (!error && resolved.parent_path() == parent && resolved.filename().string().starts_with("badman-disc-test-"))
            std::filesystem::remove_all(resolved, error);
    }
};
void stream_and_completion() {
    Fixture fixture;
    badman::Disc disc(fixture.root);
    require(disc.present() && disc.status() == 0x12, "Extracted disc is not present before activation");
    disc.activate("disc0:"); require(disc.status() == 0x32, "Activated UMD is not ready");
    const auto fd = disc.open("disc0:/PSP_GAME/USRDIR/asset.bin", 1);
    require(fd >= 3, "Read-only disc file did not open");
    std::vector<std::uint8_t> bytes;
    require(disc.read(fd, 3, bytes) == 3 && std::string(bytes.begin(), bytes.end()) == "abc", "Read returned wrong bytes");
    require(disc.seek(fd, -1, 2) == 5 && disc.read(fd, 20, bytes) == 1 && bytes[0] == 'f', "EOF-relative seek/read failed");
    require(disc.read(fd, 20, bytes) == 0 && bytes.empty(), "EOF read reused stale bytes");
    require(disc.seek(fd, std::numeric_limits<std::int64_t>::min(), 0) < 0, "Negative overflowing seek was accepted");
    require(disc.seek(fd, 0x100000000LL, 0) == 0x100000000LL, "64-bit file position was truncated");
    require(disc.close(fd) == 0, "Close failed");
    const auto async = disc.open("asset.bin", 1, true);
    constexpr auto async_busy = static_cast<std::int32_t>(0x80020329u);
    constexpr auto no_async = static_cast<std::int32_t>(0x8002032Au);
    require(async >= 3 && disc.read(async, 2, bytes) == async_busy, "Pending async open did not return PSP ASYNC_BUSY");
    require(disc.seek(async, 4, 0) == async_busy, "Pending async open allowed its file position to change");
    std::int64_t completion = -1;
    require(disc.completion(async, completion) == 0 && completion == async, "Async open result is wrong");
    const auto previous_completion = completion;
    require(disc.completion(async, completion) == no_async && completion == previous_completion,
            "Consumed async completion did not return PSP NOASYNC without changing the output");
    require(disc.read(async, 2, bytes, true) == 2 && std::string(bytes.begin(), bytes.end()) == "ab", "Async read bytes are wrong");
    require(disc.close(async) == async_busy, "Pending completion was silently discarded on close");
    require(disc.read(async, 2, bytes, true) == async_busy, "A second async read overwrote the first completion");
    require(disc.completion(async, completion) == 0 && completion == 2, "Async read completion count is wrong");
    require(disc.close(async) == 0, "Completed async handle did not close");
    require(disc.completion(-1, completion) < 0 && completion == 2, "Invalid fd changed a completion output");
    require(disc.open("missing.bin", 1) == static_cast<std::int32_t>(0x80010002u), "Missing file did not return PSP ENOENT");
    require(disc.open("asset.bin", 0x202) == static_cast<std::int32_t>(0x8001001Eu), "Read-only disc accepted mutation flags");
    bool traversal = false;
    try { disc.resolve("disc0:/../secret"); } catch (const Error &) { traversal = true; }
    require(traversal, "Disc path escaped its root");
    bool stream = false;
    try { disc.resolve("disc0:/PSP_GAME/USRDIR/asset.bin:hidden"); } catch (const Error &) { stream = true; }
    require(stream, "A PSP path selected a Windows alternate data stream");
}
void inactive_media_and_position() {
    Fixture fixture;
    badman::Disc disc(fixture.root);
    disc.activate("disc0:");
    const auto fd = disc.open("asset.bin", 1);
    std::vector<std::uint8_t> bytes;
    require(disc.read(fd, 2, bytes) == 2, "Initial read failed");
    disc.deactivate();
    constexpr auto io_error = static_cast<std::int32_t>(0x80010005u);
    require(disc.read(fd, 1, bytes) == io_error && disc.seek(fd, 0, 0) == io_error,
            "Inactive media allowed data access or changed the file position");
    disc.activate("disc0:");
    require(disc.seek(fd, 0, 1) == 2 && disc.read(fd, 1, bytes) == 1 && bytes[0] == 'c',
            "Failed inactive-media operation changed the descriptor position");
    // A completion already produced by the host remains collectable after an
    // unmount; it must not be replaced with a later media-state error.
    require(disc.read(fd, 1, bytes, true) == 1, "Async read submission failed");
    disc.deactivate();
    std::int64_t result = -1;
    require(disc.completion(fd, result) == 0 && result == 1, "Unmount lost an already-produced completion");
    require(disc.close(fd) == 0, "Unmount prevented resource cleanup");
    disc.activate("disc0:");
    const auto remaining = disc.open("asset.bin", 1);
    std::filesystem::remove(fixture.root / "PSP_GAME/PARAM.SFO");
    require(disc.status() == 1 && disc.read(remaining, 1, bytes) == io_error && disc.seek(remaining, 0, 0) == io_error,
            "A missing disc marker left the old active flag sufficient for I/O");
    require(disc.close(remaining) == 0, "Missing media prevented resource cleanup");
}
void firmware_font_mount_and_stat() {
    Fixture fixture;
    const auto font_root = fixture.root / "replacement-fonts";
    std::filesystem::create_directory(font_root);
    { std::ofstream out(font_root / "ltn0.pgf", std::ios::binary); out << "synthetic-font"; }
    badman::Disc disc(fixture.root);
    disc.mount_font_directory(font_root);
    badman::Disc::Stat info{123, true};
    require(disc.stat("flash0:/font/ltn0.pgf", info) == 0 && info.size == 14 && !info.directory,
            "Mounted font stat did not return its real file size");
    require(disc.stat("flash0:/font", info) == 0 && info.size == 0 && info.directory,
            "Mounted font root was not a directory");
    const auto directory_open = disc.open("flash0:/font", 1);
    require(directory_open < 0, "Read-only font mount opened a directory as a file");
    const auto font = disc.open("flash0:/font/ltn0.pgf", 1, true);
    require(font >= 3, "Font mount required UMD activation");
    constexpr auto async_busy = static_cast<std::int32_t>(0x80020329u);
    std::vector<std::uint8_t> bytes;
    require(disc.read(font, 1, bytes) == async_busy, "Mounted font lost the async completion contract");
    std::int64_t completion = -1;
    require(disc.completion(font, completion) == 0 && completion == font, "Mounted font async open result is wrong");
    require(disc.read(font, 9, bytes) == 9 && std::string(bytes.begin(), bytes.end()) == "synthetic",
            "Mounted font read returned wrong bytes");
    disc.activate("disc0:");
    disc.deactivate();
    require(disc.seek(font, -4, 2) == 10 && disc.read(font, 20, bytes, true) == 4 &&
            std::string(bytes.begin(), bytes.end()) == "font", "UMD deactivation blocked firmware font I/O");
    require(disc.completion(font, completion) == 0 && completion == 4 && disc.close(font) == 0,
            "Mounted font async completion/cleanup failed");
    info = {456, true};
    require(disc.stat("flash0:/font/missing.pgf", info) == static_cast<std::int32_t>(0x80010002u) &&
            info.size == 456 && info.directory, "Missing font stat overwrote its output or lost ENOENT");
    require(disc.stat("disc0:/PSP_GAME/USRDIR/asset.bin", info) == static_cast<std::int32_t>(0x80010005u) &&
            info.size == 456 && info.directory, "Disc stat ignored inactive media or changed its failed result");
    disc.activate("disc0:");
    require(disc.stat("disc0:/PSP_GAME/USRDIR/asset.bin", info) == 0 && info.size == 6 && !info.directory,
            "Disc stat did not query the actual backing file");
    for (const auto *path : {"flash0:/registry/secret", "flash0:/font/../secret", "flash0:/font/ltn0.pgf:secret"}) {
        bool rejected = false;
        try { disc.resolve(path); } catch (const Error &) { rejected = true; }
        require(rejected, "Font mount accepted an unmounted, traversal or alternate-stream path");
        info = {789, true};
        require(disc.stat(path, info) < 0 && info.size == 789 && info.directory,
                "Rejected font stat fabricated success or overwrote its result");
    }
    require(disc.open("flash0:/font/ltn0.pgf", 0x202) == static_cast<std::int32_t>(0x8001001Eu),
            "Firmware font mount accepted write flags");
    // The target remains inside this checked test-owned fixture, but outside
    // the font mount. Platforms permitting symlinks must enforce that boundary.
    std::error_code link_error;
    std::filesystem::create_directory_symlink(fixture.root / "PSP_GAME", font_root / "escape", link_error);
    if (!link_error) {
        bool rejected = false;
        try { disc.resolve("flash0:/font/escape/USRDIR/asset.bin"); } catch (const Error &) { rejected = true; }
        require(rejected, "A symlink escaped the mounted font root");
        info = {789, true};
        require(disc.stat("flash0:/font/escape/USRDIR/asset.bin", info) < 0 && info.size == 789,
                "A symlink escaped the font stat boundary");
    } else {
        std::cout << "Directory symlink creation unavailable: " << link_error.message() << '\n';
    }
}
void guest_io_abi() {
    Fixture fixture;
    Runtime runtime; runtime.set_game_root(fixture.root);
    std::ostringstream trace;
    badman::Host host(runtime, 0x089F0000, 0x089E0000, 0x0891F420, trace);
    host.install({}); host.bootstrap(0x08804000, "test");
    auto &ctx = runtime.cpu();
    const auto address = 0x08820000u;
    const std::string drive = "disc0:", name = "disc0:/PSP_GAME/USRDIR/asset.bin";
    for (std::size_t i = 0; i <= drive.size(); ++i) runtime.memory().store8(address + static_cast<std::uint32_t>(i), i == drive.size() ? 0 : drive[i]);
    ctx.gpr[4] = 1; ctx.gpr[5] = address; runtime.invoke_import("sceUmdUser", 0xC6183D47, ctx);
    for (std::size_t i = 0; i <= name.size(); ++i) runtime.memory().store8(address + static_cast<std::uint32_t>(i), i == name.size() ? 0 : name[i]);
    ctx.gpr[4] = address; ctx.gpr[5] = 1;
    runtime.invoke_import("IoFileMgrForUser", 0x89AA9906, ctx);
    const auto fd = ctx.gpr[2]; ctx.gpr[4] = fd; ctx.gpr[5] = address + 256;
    runtime.invoke_import("IoFileMgrForUser", 0xE23EEC33, ctx);
    require(runtime.memory().load32(address + 256) == fd && runtime.memory().load32(address + 260) == 0,
            "64-bit completion output does not follow the PSP ABI");
    runtime.memory().store32(address + 256, 0x11223344);
    runtime.memory().store32(address + 260, 0x55667788);
    runtime.invoke_import("IoFileMgrForUser", 0xE23EEC33, ctx);
    require(ctx.gpr[2] == 0x8002032Au && runtime.memory().load32(address + 256) == 0x11223344 &&
            runtime.memory().load32(address + 260) == 0x55667788,
            "sceIoWaitAsync fabricated a result or stopped instead of returning NOASYNC");
    ctx.gpr[4] = fd; ctx.gpr[6] = 0; ctx.gpr[7] = 1; ctx.gpr[8] = 0;
    runtime.invoke_import("IoFileMgrForUser", 0x27EB27B8, ctx);
    require(ctx.gpr[2] == 0 && ctx.gpr[3] == 1, "sceIoLseek ignored aligned argument/result register pairs");
    ctx.gpr[4] = 0xFFFFFFFFu; ctx.gpr[5] = address + 256;
    runtime.invoke_import("IoFileMgrForUser", 0xE23EEC33, ctx);
    require(ctx.gpr[2] == 0x80010009u && runtime.memory().load32(address + 256) == 0x11223344 &&
            runtime.memory().load32(address + 260) == 0x55667788,
            "Invalid async fd changed its result buffer or lost the API error");
}
}
int main() {
    try { stream_and_completion(); inactive_media_and_position(); firmware_font_mount_and_stat(); guest_io_abi(); std::cout << "Badman UMD, file, async completion and ABI tests passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
