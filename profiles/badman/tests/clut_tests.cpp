#include "badman_clut.hpp"
#include "psprecomp/common.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using psprecomp::GuestMemory;
using Registers = std::array<std::uint32_t, 256>;
constexpr std::uint32_t source = 0x08830000;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char *message) {
    bool failed = false;
    try { operation(); } catch (const psprecomp::Error &) { failed = true; }
    require(failed, message);
}
Registers registers(std::uint32_t address = source) {
    Registers regs{};
    regs[0xB0] = 0xB0000000 | (address & 0xFFFFFF);
    regs[0xB1] = 0xB1000000 | ((address >> 8) & 0xFF0000);
    regs[0xC5] = 0xC500FF03;
    return regs;
}
void latch_partial_and_validity() {
    GuestMemory memory; badman::Clut palette;
    auto regs = registers();
    rejects([&] { palette.lookup(0, regs[0xC5]); }, "Uninitialized palette entry fabricated a color");
    for (std::uint32_t i = 0; i < 256; ++i) memory.store32(source + i * 4, 0xFF000000 | i * 0x010101);
    palette.load(memory, regs, 0xC4000020);
    require(palette.lookup(0, regs[0xC5]) == 0xFF000000 && palette.lookup(255, regs[0xC5]) == 0xFFFFFFFF,
            "Full1024-byte palette load omitted boundary entries");
    memory.store32(source, 0x12345678);
    require(palette.lookup(0, regs[0xC5]) == 0xFF000000, "Palette read guest bytes after the CLUT load command");
    for (std::uint32_t i = 0; i < 8; ++i) memory.store32(source + i * 4, 0x12345678 + i);
    palette.load(memory, regs, 1);
    require(palette.lookup(0, regs[0xC5]) == 0x12345678 && palette.lookup(7, regs[0xC5]) == 0x1234567F &&
            palette.lookup(8, regs[0xC5]) == 0xFF080808 && palette.lookup(255, regs[0xC5]) == 0xFFFFFFFF,
            "Partial CLUT load did not update exactly32 prefix bytes/preserve the remaining entries");
    badman::Clut partial;
    partial.load(memory, regs, 1);
    require(partial.lookup(7, regs[0xC5]) == 0x1234567F, "Last initialized partial entry failed");
    rejects([&] { partial.lookup(8, regs[0xC5]); }, "Partial load initialized an entry it did not read");
    regs = registers(0x07000000);
    palette.load(memory, regs, 0);
    require(palette.lookup(0, 0xFF03) == 0x12345678, "Zero-count load read invalid memory or erased palette state");
}
void lookup_transform_and_raw_formats() {
    GuestMemory memory; badman::Clut palette;
    auto regs = registers();
    for (std::uint32_t i = 0; i < 256; ++i) memory.store32(source + i * 4, 0xAB000000 | i);
    palette.load(memory, regs, 32);
    require(palette.lookup(0xBA, 3 | (1 << 2) | (3 << 8) | (1 << 16)) == 0xAB000011,
            "CLUT shift/mask/start lookup order is wrong");
    require(palette.lookup(16, 0xFF03 | (1 << 16)) == 0xAB000010,
            "CLUT start was added instead of ORed into the index");
    require(palette.lookup(1, 0xFF03 | (17 << 16)) == 0xAB000011,
            "8888 CLUT index did not wrap to256 entries");
    require(palette.lookup(0x80000000, 3 | (31 << 2) | (1 << 8)) == 0xAB000001,
            "CLUT maximum shift overflowed the host index");
    memory.store16(source, 0x07E0); memory.store16(source + 2, 0x801F);
    memory.store16(source + 4, 0xF321); memory.store16(source + 1022, 0xDCBA);
    palette.load(memory, regs, 32);
    require(palette.lookup(0, 0xFF00) == 0xFF00FF00, "5650 palette expansion is incorrect");
    require(palette.lookup(1, 0xFF01) == 0xFF0000FF && palette.lookup(0, 0xFF01) == 0x0008FF00,
            "5551 palette alpha/color expansion is incorrect");
    require(palette.lookup(2, 0xFF02) == 0xFF332211, "4444 palette nibble expansion is incorrect");
    require(palette.lookup(255, 0x1FFF02) == 0xDDCCBBAA, "16-bit CLUT did not expose its512th raw entry");
    require(palette.lookup(0, 0xFF03) == 0x801F07E0,
            "Changing palette format did not reinterpret the same latched raw bytes");
    rejects([&] { palette.lookup(0, 0xFF83); }, "Reserved CLUT format flag accepted");
    rejects([&] { palette.lookup(0, 0x20FF03); }, "Out-of-range CLUT start flag accepted");
}
void source_validation_is_atomic_and_aligned() {
    GuestMemory memory; badman::Clut palette;
    auto regs = registers(); memory.store32(source, 0xDEADC0DE); palette.load(memory, regs, 1);
    const auto unchanged = [&] { require(palette.lookup(0, 0xFF03) == 0xDEADC0DE, "Rejected load corrupted latched bytes"); };
    regs = registers(0x09FFFFF0);
    rejects([&] { palette.load(memory, regs, 1); }, "Load with partially valid source footprint accepted"); unchanged();
    regs = registers();
    rejects([&] { palette.load(memory, regs, 33); }, "Unimplemented oversized CLUT load fabricated completion"); unchanged();
    regs[0xB1] |= 1;
    rejects([&] { palette.load(memory, regs, 1); }, "Reserved CLUT source flag accepted"); unchanged();
    memory.store32(source, 0xF0123456); regs = registers(source + 15); palette.load(memory, regs, 1);
    require(palette.lookup(0, 0xFF03) == 0xF0123456, "CLUT source low four bits were not ignored");
    memory.store32(0x04001000, 0x87654321); regs = registers(0x04001000); palette.load(memory, regs, 1);
    require(palette.lookup(0, 0xFF03) == 0x87654321, "Valid VRAM CLUT source was not latched");
}
// Packed inputs and expected colors are literal fixtures, not a second decoder.
constexpr std::array<std::uint32_t, 4> packed_prefix{0x000007E0, 0x0000801F, 0x0000F321, 0xA1B2C3D4};
constexpr std::array<std::uint32_t, 4> decoded_prefix{0xFF00FF00, 0xFF0000FF, 0xFF332211, 0xA1B2C3D4};
void seed_switch_fixture(GuestMemory &memory) {
    const std::array<std::uint8_t, 1024> zeros{};
    memory.copy_in(source, zeros);
    // At byte64: 5650 green, 5551 green+blue, 4444 nibbles, or one raw 8888 color.
    memory.store32(source + 64, 0x801F07E0);
    memory.store32(source + 1020, 0xFFFFFFFF);
}
void require_literal_tails(const badman::Clut &palette) {
    constexpr std::array<std::uint32_t, 4> expected{0xFF00FF00, 0x0008FF00, 0x0077EE00, 0x801F07E0};
    for (std::uint32_t format = 0; format < 4; ++format) {
        const auto index = format == 3 ? 16u : 32u;
        require(palette.lookup(index, 0xFF00u | format) == expected[format],
                "Format switch/partial load lost or reinterpreted an older raw tail");
        const auto last_word = format == 3 ? 0xFF03u : 0x1FFF00u | format;
        require(palette.lookup(255, last_word) == 0xFFFFFFFF,
                "Short load erased the final previously latched physical entry");
    }
}
void actual_load_formats_switches_and_retained_tails() {
    GuestMemory memory;
    for (std::uint32_t first = 0; first < 4; ++first) {
        for (std::uint32_t second = 0; second < 4; ++second) {
            seed_switch_fixture(memory);
            auto regs = registers(); regs[0xC5] = 0xC500FF00u | first;
            memory.store32(source, packed_prefix[first]);
            badman::Clut palette; palette.load(memory, regs, 32);
            require(palette.lookup(0, 0xFF00u | first) == decoded_prefix[first],
                    "Actual load-time format did not decode its literal color");
            memory.store32(source, packed_prefix[second]);
            regs[0xC5] = 0xC500FF00u | second; palette.load(memory, regs, 1);
            require(palette.lookup(0, 0xFF00u | second) == decoded_prefix[second],
                    "Short format-switch load retained a stale prefix color");
            require_literal_tails(palette);
            // Switch back after a short load: the tail may need the original raw path.
            memory.store32(source, packed_prefix[first]);
            regs[0xC5] = 0xC500FF00u | first; palette.load(memory, regs, 1);
            require(palette.lookup(0, 0xFF00u | first) == decoded_prefix[first],
                    "Switch-back load returned a stale decoded prefix");
            require_literal_tails(palette);
            memory.store32(source + 64, 0);
            require_literal_tails(palette); // No reload: guest writes cannot change the latch.
        }
    }
}
template<class F> void rejects_with(F operation, const char *expected) {
    try { operation(); }
    catch (const psprecomp::Error &error) {
        require(std::string(error.what()) == expected, "CLUT failure timing or error priority changed");
        return;
    }
    throw std::runtime_error("Expected CLUT failure did not occur");
}
void malformed_load_format_is_lazy_and_failed_loads_preserve_state() {
    GuestMemory memory;
    for (std::uint32_t format = 0; format < 4; ++format) {
        seed_switch_fixture(memory);
        memory.store32(source, packed_prefix[format]);
        auto regs = registers(); regs[0xC5] = 0xC500FF80u | format;
        badman::Clut palette; palette.load(memory, regs, 32); // Reserved C5 bit must not fail LOAD.
        require(palette.lookup(0, 0xFF00u | format) == decoded_prefix[format],
                "Malformed C5 prevented a successful palette load");
        rejects_with([&] { palette.lookup(0, regs[0xC5]); },
                     "GE CLUT: unsupported format/shift/mask/start flags");
        regs[0xC5] = 0xC520FF00u | format; palette.load(memory, regs, 1);
        rejects_with([&] { palette.lookup(0, regs[0xC5]); },
                     "GE CLUT: unsupported format/shift/mask/start flags");
        const auto unchanged = [&] {
            require(palette.lookup(0, 0xFF00u | format) == decoded_prefix[format],
                    "Zero/rejected load changed the previous decoded/raw prefix");
            require_literal_tails(palette);
        };
        regs = registers(0x09FFFFF0); regs[0xB1] |= 1;
        regs[0xC5] = 0xC5FFFF80u | ((format + 1) & 3u);
        palette.load(memory, regs, 0); unchanged(); // Zero bypasses source, flags and C5 validation.
        rejects_with([&] { palette.load(memory, regs, 33); },
                     "GE CLUT: loads larger than 32 blocks are not implemented"); unchanged();
        rejects_with([&] { palette.load(memory, regs, 1); },
                     "GE CLUT: unsupported source-address flags"); unchanged();
        regs = registers(0x09FFFFF0); regs[0xC5] = 0xC500FF80u | ((format + 1) & 3u);
        rejects_with([&] { palette.load(memory, regs, 1); },
                     "GE CLUT: complete load source lies outside guest memory"); unchanged();
        badman::Clut empty;
        // Lookup format errors still precede unloaded-entry checks.
        rejects_with([&] { empty.lookup(0, 0xFF80u | format); },
                     "GE CLUT: unsupported format/shift/mask/start flags");
        auto partial_regs = registers(); partial_regs[0xC5] = 0xC500FF00u | format;
        empty.load(memory, partial_regs, 1);
        rejects_with([&] { empty.lookup(format == 3 ? 8u : 16u, 0xFF00u | format); },
                     "GE CLUT: palette entry has not been loaded");
    }
}
void copied_palette_owns_its_format_prefix_and_raw_tail() {
    GuestMemory memory;
    for (std::uint32_t format = 0; format < 4; ++format) {
        seed_switch_fixture(memory); memory.store32(source, packed_prefix[format]);
        auto regs = registers(); regs[0xC5] = 0xC500FF00u | format;
        badman::Clut original; original.load(memory, regs, 32);
        auto copied = original;
        badman::Clut assigned; assigned = original;
        memory.store32(source, 0xFFFFFFFF);
        original.load(memory, regs, 1);
        require(original.lookup(0, 0xFF00u | format) == 0xFFFFFFFF,
                "Original palette did not accept its independent short update");
        require(copied.lookup(0, 0xFF00u | format) == decoded_prefix[format] &&
                assigned.lookup(0, 0xFF00u | format) == decoded_prefix[format],
                "Copied palette shared mutable decoded/raw prefix storage");
        require_literal_tails(original); require_literal_tails(copied); require_literal_tails(assigned);
        const auto other = (format + 1) & 3u;
        memory.store32(source, packed_prefix[other]); regs[0xC5] = 0xC500FF00u | other;
        copied.load(memory, regs, 1);
        require(copied.lookup(0, 0xFF00u | other) == decoded_prefix[other] &&
                assigned.lookup(0, 0xFF00u | format) == decoded_prefix[format],
                "Switching one copied palette changed another instance's format/prefix");
        require_literal_tails(copied); require_literal_tails(assigned);
    }
}
}
int main() {
    try {
        latch_partial_and_validity(); lookup_transform_and_raw_formats(); source_validation_is_atomic_and_aligned();
        actual_load_formats_switches_and_retained_tails();
        malformed_load_format_is_lazy_and_failed_loads_preserve_state();
        copied_palette_owns_its_format_prefix_and_raw_tail();
        std::cout << "CLUT tests passed\n"; return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
