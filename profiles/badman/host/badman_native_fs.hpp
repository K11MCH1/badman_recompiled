#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace psprecomp { class GuestMemory; }
namespace badman {
// Native Windows compatibility policy: different regular single-link files
// under an existing mounted ms0 root, ASCII guest components, no replacement.
// Protected native/PSP save trees, directories and unsupported paths fail.
std::int32_t rename_native_file(const std::filesystem::path &root,
                               std::string_view old_name, std::string_view new_name) noexcept;
// Copies both bounded NUL-terminated guest strings before touching the host FS.
std::int32_t rename_native_file_guest(const psprecomp::GuestMemory &memory,
    const std::filesystem::path &root,std::uint32_t old_name,std::uint32_t new_name) noexcept;
}
