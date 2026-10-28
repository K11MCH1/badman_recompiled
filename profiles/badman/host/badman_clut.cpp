#include "badman_clut.hpp"
#include "psprecomp/common.hpp"

namespace badman {
void Clut::load(const psprecomp::GuestMemory &memory,
                const std::array<std::uint32_t, 256> &registers, std::uint32_t command_data) {
    const auto blocks = command_data & 0x00FFFFFFu;
    if (blocks > 32)
        throw psprecomp::Error("GE CLUT: loads larger than 32 blocks are not implemented");
    if (!blocks) return; // Hardware zero-count load preserves the previous palette.
    const auto high = registers[0xB1] & 0x00FFFFFFu;
    if (high & ~0x00FF0000u)
        throw psprecomp::Error("GE CLUT: unsupported source-address flags");
    // Hardware ignores the low four address bits. Each load block is32 bytes,
    // independent of the current palette's interpretation as16/32-bit entries.
    const auto address = (registers[0xB0] & 0x00FFFFF0u) | ((high & 0x00FF0000u) << 8);
    const auto length = blocks * 32;
    if (!memory.contains(address, length))
        throw psprecomp::Error("GE CLUT: complete load source lies outside guest memory");
    auto next = bytes_;
    for (std::uint32_t i = 0; i < length; ++i) next[i] = memory.load8(address + i);
    // Selecting low two C5 bits must not validate reserved format/transform
    // flags here: original load errors depend only on count/source/footprint.
    const auto format = registers[0xC5] & 3u;
    const auto size = format == 3u ? 4u : 2u;
    const auto entries = length / size;
    // Deliberately not value-initialized or copied from the old table. Only
    // [0, entries) is assigned, read and committed below; unused cells stay local.
    std::array<std::uint32_t, 512> staged_colors;
    for (std::uint32_t entry = 0; entry < entries; ++entry) {
        const auto offset = entry * size;
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < size; ++i)
            value |= static_cast<std::uint32_t>(next[offset + i]) << (i * 8u);
        staged_colors[entry] = decode_color(value, format);
    }
    const auto new_count = static_cast<std::uint16_t>(entries);
    const auto retained_count = format == cached_format_ && cached_entries_ > new_count
        ? cached_entries_ : new_count;
    // Source reads and nonthrowing prefix decode are complete before commit.
    // Same-format short loads retain cached tails; other raw tails remain
    // available through lookup_raw after any format switch.
    bytes_ = next;
    for (std::uint32_t i = 0; i < length; ++i) valid_.set(i);
    for (std::uint32_t entry = 0; entry < entries; ++entry) decoded_[entry] = staged_colors[entry];
    cached_entries_ = retained_count;
    cached_format_ = static_cast<std::uint8_t>(format);
    ++generation_;
}
void Clut::validate_format(std::uint32_t format_word) {
    if ((format_word & 0x00FFFFFFu) & ~0x001FFF7Fu)
        throw psprecomp::Error("GE CLUT: unsupported format/shift/mask/start flags");
}
std::uint32_t Clut::entry_index(std::uint32_t index, std::uint32_t format_word) noexcept {
    const auto format = format_word & 3u;
    const auto shift = (format_word >> 2) & 31u;
    const auto mask = (format_word >> 8) & 255u;
    const auto start = ((format_word >> 16) & 31u) << 4;
    return (((index >> shift) & mask) | start) & (format == 3 ? 255u : 511u);
}
std::uint32_t Clut::lookup(std::uint32_t index, std::uint32_t format_word) const {
    validate_format(format_word);
    const auto format = format_word & 3u;
    const auto entry = entry_index(index, format_word);
    if (format == cached_format_ && entry < cached_entries_) return decoded_[entry];
    return lookup_raw(entry, format);
}
bool Clut::try_lookup(std::uint32_t index, std::uint32_t format_word, std::uint32_t &color) const noexcept {
    if ((format_word & 0x00FFFFFFu) & ~0x001FFF7Fu) return false;
    const auto format = format_word & 3u;
    const auto entry = entry_index(index, format_word);
    if (format == cached_format_ && entry < cached_entries_) { color = decoded_[entry]; return true; }
    const auto size = format == 3 ? 4u : 2u;
    const auto offset = entry * size;
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        if (!valid_[offset + i]) return false;
        value |= static_cast<std::uint32_t>(bytes_[offset + i]) << (i * 8);
    }
    color = decode_color(value, format);
    return true;
}
std::uint32_t Clut::lookup_raw(std::uint32_t entry, std::uint32_t format) const {
    const auto size = format == 3 ? 4u : 2u;
    const auto offset = entry * size;
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        if (!valid_[offset + i]) throw psprecomp::Error("GE CLUT: palette entry has not been loaded");
        value |= static_cast<std::uint32_t>(bytes_[offset + i]) << (i * 8);
    }
    return decode_color(value, format);
}
std::uint32_t Clut::decode_color(std::uint32_t value, std::uint32_t format) noexcept {
    if (format == 3) return value;
    const auto expand5 = [](std::uint32_t v) { return (v << 3) | (v >> 2); };
    if (format == 0) {
        const auto green = (value >> 5) & 63u;
        return 0xFF000000u | expand5(value & 31u) | (((green << 2) | (green >> 4)) << 8) |
               (expand5((value >> 11) & 31u) << 16);
    }
    if (format == 1)
        return ((value & 0x8000u) ? 0xFF000000u : 0u) | expand5(value & 31u) |
               (expand5((value >> 5) & 31u) << 8) | (expand5((value >> 10) & 31u) << 16);
    return ((value & 15u) * 17u) | ((((value >> 4) & 15u) * 17u) << 8) |
           ((((value >> 8) & 15u) * 17u) << 16) | ((((value >> 12) & 15u) * 17u) << 24);
}
}
