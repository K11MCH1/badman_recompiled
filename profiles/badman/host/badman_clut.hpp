#pragma once
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <bitset>
#include <cstdint>

namespace badman {
// The palette contains latched raw bytes, independent of subsequent guest
// writes and texture-cache invalidation. Only the bounded 1-KiB load is exposed.
class Clut {
public:
    void load(const psprecomp::GuestMemory &memory,
              const std::array<std::uint32_t, 256> &registers, std::uint32_t command_data);
    std::uint32_t lookup(std::uint32_t index, std::uint32_t format_word) const;
    // As lookup, but false instead of an exception for an unsupported format
    // or an entry that has not been loaded. Hashing and decoding whole
    // palettes for scene recording expect such entries; an exception per
    // entry cost the Android game thread tens of milliseconds a frame.
    bool try_lookup(std::uint32_t index, std::uint32_t format_word, std::uint32_t &color) const noexcept;
    static void validate_format(std::uint32_t format_word);
    // Counts completed loads, so a palette hash can be reused until the next one.
    std::uint64_t generation() const noexcept { return generation_; }
private:
    std::uint64_t generation_{};
    static std::uint32_t decode_color(std::uint32_t value, std::uint32_t format) noexcept;
    static std::uint32_t entry_index(std::uint32_t index, std::uint32_t format_word) noexcept;
    std::uint32_t lookup_raw(std::uint32_t entry, std::uint32_t format) const;
    std::array<std::uint8_t, 1024> bytes_{};
    std::bitset<1024> valid_{};
    // Each actual load updates a whole-entry prefix. Only the selected format
    // and this cached prefix may bypass the original latched-byte fallback.
    // Owned values are initialized once; lookup never mutates the cache.
    std::array<std::uint32_t, 512> decoded_{};
    std::uint16_t cached_entries_{};
    std::uint8_t cached_format_{0xFFu};
};
}
