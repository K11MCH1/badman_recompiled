#include "psprecomp/iso9660.hpp"
#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <set>

namespace psprecomp {
namespace {
constexpr std::uint32_t sector = 2048u;
constexpr unsigned max_depth = 16u;
constexpr std::size_t max_entries = 65536u;

std::uint32_t le32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) | static_cast<std::uint32_t>(data[1]) << 8u |
           static_cast<std::uint32_t>(data[2]) << 16u | static_cast<std::uint32_t>(data[3]) << 24u;
}
// d-characters plus '.', ';' and the lowercase/punctuation UMD mastering
// tools emit. Separators, traversal, drive/stream colons and controls are not.
std::string checked_name(const std::uint8_t *data, std::size_t length) {
    std::string name(reinterpret_cast<const char *>(data), length);
    if (const auto version = name.find(';'); version != std::string::npos) {
        if (name.find_first_not_of("0123456789", version + 1) != std::string::npos || version + 1 == name.size())
            throw Error("ISO 9660 file name has an invalid version suffix");
        name.resize(version);
    }
    if (!name.empty() && name.back() == '.') name.pop_back();
    if (name.empty() || name == "." || name == ".." || name.size() > 255)
        throw Error("ISO 9660 image contains an empty or reserved name");
    for (const char character : name) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20u || value >= 0x7Fu || character == '/' || character == '\\' || character == ':' ||
            character == '*' || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
            throw Error("ISO 9660 image contains an unsafe file name");
    }
    return name;
}
}

Iso9660Image::Iso9660Image(const std::filesystem::path &image) : file_(image, std::ios::binary) {
    if (!file_) throw Error("Cannot open ISO image: " + image.string());
    file_.seekg(0, std::ios::end);
    image_size_ = static_cast<std::uint64_t>(file_.tellg());
    if (image_size_ < 17u * sector || image_size_ % sector) throw Error("ISO image size is not a whole number of 2048-byte sectors");
    std::array<std::uint8_t, sector> volume{};
    read(16u * sector, volume.data(), volume.size());
    if (volume[0] != 1u || std::string(reinterpret_cast<const char *>(volume.data() + 1), 5) != "CD001" || volume[6] != 1u)
        throw Error("ISO image has no primary volume descriptor");
    if ((volume[128] | volume[129] << 8u) != sector) throw Error("Only 2048-byte ISO 9660 logical blocks are supported");
    const auto *root = volume.data() + 156;
    if (root[0] != 34u || !(root[25] & 2u)) throw Error("Invalid ISO 9660 root directory record");
    scan("", le32(root + 2), le32(root + 10), 0u);
}

void Iso9660Image::read(std::uint64_t offset, void *data, std::size_t size) {
    if (offset > image_size_ || size > image_size_ - offset) throw Error("ISO 9660 extent lies outside the image");
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    if (static_cast<std::size_t>(file_.gcount()) != size) throw Error("Short read from ISO image");
}

void Iso9660Image::scan(const std::string &prefix, std::uint32_t extent, std::uint32_t size, unsigned depth) {
    if (depth > max_depth) throw Error("ISO 9660 directory tree is too deep");
    if (!size || size % sector) throw Error("ISO 9660 directory size is not whole sectors");
    std::vector<std::uint8_t> data(size);
    read(static_cast<std::uint64_t>(extent) * sector, data.data(), data.size());
    std::set<std::string> names;
    std::vector<Entry> directories;
    for (std::size_t offset = 0; offset < data.size();) {
        const auto length = data[offset];
        if (length == 0u) { offset = (offset / sector + 1u) * sector; continue; } // Records never span sectors.
        if (length < 34u || offset % sector + length > sector) throw Error("Invalid ISO 9660 directory record");
        const auto *record = data.data() + offset;
        const auto name_length = record[32];
        if (33u + name_length > length) throw Error("ISO 9660 directory record name overruns its record");
        offset += length;
        if (name_length == 1u && (record[33] == 0u || record[33] == 1u)) continue; // Self and parent.
        const auto flags = record[25];
        if (flags & 0x80u) throw Error("Multi-extent ISO 9660 files are not supported");
        if (record[26] || record[27]) throw Error("Interleaved ISO 9660 files are not supported");
        Entry entry;
        entry.path = prefix + checked_name(record + 33, name_length);
        entry.directory = (flags & 2u) != 0u;
        entry.extent = le32(record + 2);
        entry.size = le32(record + 10);
        std::string folded = entry.path;
        std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
        if (!names.insert(folded).second) throw Error("ISO 9660 directory has duplicate names: " + entry.path);
        if (static_cast<std::uint64_t>(entry.extent) * sector + entry.size > image_size_)
            throw Error("ISO 9660 entry lies outside the image: " + entry.path);
        if (entries_.size() >= max_entries) throw Error("ISO 9660 image has too many entries");
        entries_.push_back(entry);
        if (entry.directory) directories.push_back(entry);
    }
    for (const auto &directory : directories) scan(directory.path + "/", directory.extent, directory.size, depth + 1u);
}

void Iso9660Image::extract(const Entry &entry, const std::filesystem::path &destination) {
    if (entry.directory) throw Error("Cannot extract an ISO 9660 directory as a file");
    if (std::filesystem::exists(std::filesystem::symlink_status(destination)))
        throw Error("Refusing to overwrite an existing extracted file: " + destination.string());
    std::ofstream out(destination, std::ios::binary);
    if (!out) throw Error("Cannot create extracted file: " + destination.string());
    std::vector<char> buffer(1u << 20u);
    std::uint64_t offset = static_cast<std::uint64_t>(entry.extent) * sector;
    for (std::uint32_t remaining = entry.size; remaining;) {
        const auto count = std::min<std::uint32_t>(remaining, static_cast<std::uint32_t>(buffer.size()));
        read(offset, buffer.data(), count);
        out.write(buffer.data(), count);
        offset += count; remaining -= count;
    }
    out.close();
    if (!out) throw Error("Cannot write extracted file: " + destination.string());
}

} // namespace psprecomp
