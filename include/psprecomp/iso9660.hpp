#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace psprecomp {

// Read-only ISO 9660 reader for UMD images: primary volume descriptor, plain
// single-extent files and directories. Multi-extent and interleaved files,
// unsafe names and extents outside the image are rejected, never guessed.
class Iso9660Image {
public:
    struct Entry {
        std::string path;       // '/'-separated, version suffix removed.
        bool directory{};
        std::uint32_t extent{}; // Logical block of the data.
        std::uint32_t size{};
    };
    explicit Iso9660Image(const std::filesystem::path &image);
    [[nodiscard]] const std::vector<Entry> &entries() const noexcept { return entries_; }
    [[nodiscard]] std::uint64_t image_size() const noexcept { return image_size_; }
    // Streams one file's bytes to a new destination; an existing file is an error.
    void extract(const Entry &entry, const std::filesystem::path &destination);
private:
    void read(std::uint64_t offset, void *data, std::size_t size);
    void scan(const std::string &prefix, std::uint32_t extent, std::uint32_t size, unsigned depth);
    std::ifstream file_;
    std::uint64_t image_size_{};
    std::vector<Entry> entries_;
};

} // namespace psprecomp
