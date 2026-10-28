#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace psprecomp {

// Streaming SHA-256; hexdigest() finalizes once.
class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    [[nodiscard]] std::string hexdigest();
private:
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> block_{};
    std::uint64_t length_{};
    std::size_t used_{};
    bool finished_{};
};
[[nodiscard]] std::string sha256_bytes(std::span<const std::uint8_t> data);
[[nodiscard]] std::string sha256_file(const std::filesystem::path &path);

} // namespace psprecomp
