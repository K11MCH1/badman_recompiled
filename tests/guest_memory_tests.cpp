#include "psprecomp/guest_memory.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void watch(const char *address) {
#ifdef _WIN32
    require(_putenv_s("PSPRECOMP_WATCH_WRITE", address ? address : "") == 0, "Cannot set watch environment");
    require(_putenv_s("PSPRECOMP_WATCH_WRITE_SIZE", "2") == 0, "Cannot set watch width");
#else
    require((address ? setenv("PSPRECOMP_WATCH_WRITE", address, 1) : unsetenv("PSPRECOMP_WATCH_WRITE")) == 0,
            "Cannot set watch environment");
    require(setenv("PSPRECOMP_WATCH_WRITE_SIZE", "2", 1) == 0, "Cannot set watch width");
#endif
}
struct Capture {
    std::ostringstream text;
    std::streambuf *original = std::cerr.rdbuf(text.rdbuf());
    ~Capture() { std::cerr.rdbuf(original); }
};
template<class F> void error_at(F operation, std::uint32_t address) {
    try { operation(); }
    catch (const psprecomp::Error &error) {
        require(error.what() == std::string("Guest memory access outside PSP RAM/EDRAM at ") + psprecomp::hex32(address),
                "Memory failure reported a different address");
        return;
    }
    throw std::runtime_error("Invalid memory access succeeded");
}
std::size_t occurrences(const std::string &text, std::string_view needle) {
    std::size_t count = 0, at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) { ++count; at += needle.size(); }
    return count;
}
void unaligned(psprecomp::GuestMemory &memory, std::uint32_t base, std::uint32_t alias) {
    const std::array<std::uint8_t, 8> seed{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
    memory.copy_in(base, seed);
    require(memory.load16(alias + 1) == 0x3322 && memory.load32(alias + 1) == 0x55443322,
            "Unaligned alias read changed little-endian values");
    memory.store16(alias + 1, 0xBEEF);
    memory.store32(alias + 3, 0xA1B2C3D4);
    const std::array<std::uint8_t, 8> expected{0x11,0xEF,0xBE,0xD4,0xC3,0xB2,0xA1,0x88};
    std::array<std::uint8_t, 8> actual{};
    memory.copy_out(base, actual);
    require(actual == expected && memory.load32(base + 3) == 0xA1B2C3D4,
            "Unaligned alias writes damaged bytes or guards");
}
void atomic_end(psprecomp::GuestMemory &memory, std::uint32_t end) {
    const std::array<std::uint8_t, 8> seed{1,2,3,4,5,6,7,8};
    memory.copy_in(end - 8, seed);
    error_at([&] { memory.store32(end - 2, 0xFFFFFFFF); }, end);
    error_at([&] { memory.store16(end - 1, 0xFFFF); }, end);
    error_at([&] { (void)memory.load32(end - 2); }, end);
    std::array<std::uint8_t, 8> after{};
    memory.copy_out(end - 8, after);
    require(after == seed, "Failed whole-word preflight wrote a valid prefix");
}
void boundaries() {
    watch(nullptr);
    for (const auto size : {32u * 1024u * 1024u, 64u * 1024u * 1024u}) {
        psprecomp::GuestMemory memory(size);
        unaligned(memory, 0x08000000, 0x88000000);
        unaligned(memory, 0x04000000, 0x44600000);
        constexpr auto physical_end = psprecomp::GuestMemory::kVramPhysicalBase + psprecomp::GuestMemory::kVramSize;
        require(memory.contains(physical_end - 2, 4) && !memory.raw_pointer(physical_end - 2, 4),
                "A physical mirror wrap became a contiguous host span");
        memory.store32(physical_end - 2, 0xA1B2C3D4);
        require(memory.load32(physical_end - 2) == 0xA1B2C3D4 &&
                memory.load16(0x04000000) == 0xA1B2 && memory.load16(physical_end - 2) == 0xC3D4,
                "Word access failed across physical VRAM mirror");
        memory.store16(0x443FFFFF, 0x7E6D);
        require(memory.load16(0x041FFFFF) == 0x7E6D, "Halfword access failed across mirrored alias");
        atomic_end(memory, 0x04800000);
        atomic_end(memory, 0x08000000 + size);
        error_at([&] { (void)memory.load32(0x07FFFFFE); }, 0x07FFFFFE);
    }
}
void watch_enabled() {
    watch("0x08000001");
    psprecomp::GuestMemory memory;
    Capture output;
    memory.store32(0x88000000, 0xA1B2C3D4);
    memory.store32(0x88000000, 0xA1B2C3D4); // Unchanged writes are still observable.
    memory.store16(0x88000002, 0x1122); // Partial overlap through an alias.
    memory.store32(0x08000008, 0x12345678); // Outside the watch.
    const auto text = output.text.str();
    require(occurrences(text, "[watch-write]") == 3 && occurrences(text, "op=store32") == 2 &&
            occurrences(text, "op=store16") == 1 && text.find("old=0xa1b2c3d4") != std::string::npos &&
            text.find("old=0xa1b2") != std::string::npos,
            "Word watch lost old values, aliases, unchanged writes or overlap filtering");
}
void late_invalid() {
    watch(nullptr);
    psprecomp::GuestMemory memory;
    Capture output;
    error_at([&] { memory.store32(0x09FFFFFE, 1); }, 0x0A000000);
    require(memory.load16(0x09FFFFFE) == 0 && output.text.str().empty(),
            "Failed store changed memory or emitted a watch event");
    watch("0x08000000"); // Deliberately after construction and failed preflight.
    memory.store32(0x08000000, 0x12345678);
    require(occurrences(output.text.str(), "[watch-write]") == 1,
            "Invalid store initialized the lazy watcher early, or constructor flag hid it");
}
void latch(bool enabled) {
    watch(enabled ? "0x08000000" : nullptr);
    psprecomp::GuestMemory memory;
    Capture output;
    memory.store16(0x08000000, 1);
    watch(enabled ? nullptr : "0x08000000");
    memory.store32(0x08000000, 2);
    require(occurrences(output.text.str(), "[watch-write]") == (enabled ? 2u : 0u),
            "Successful-store watch initialization stopped being process-latched");
}
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "Select one fresh-process memory scenario");
        const std::string_view mode = argv[1];
        if (mode == "boundaries") boundaries();
        else if (mode == "watch") watch_enabled();
        else if (mode == "late-invalid") late_invalid();
        else if (mode == "latch-enabled") latch(true);
        else if (mode == "latch-disabled") latch(false);
        else throw std::runtime_error("Unknown memory scenario");
        std::cout << "Memory regression passed: " << mode << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Memory regression failed: " << error.what() << '\n';
        return 1;
    }
}
