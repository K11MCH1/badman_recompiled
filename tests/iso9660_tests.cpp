#include "psprecomp/common.hpp"
#include "psprecomp/iso9660.hpp"
#include "psprecomp/sha256.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
constexpr std::size_t sector = 2048;

void put32(std::vector<std::uint8_t> &d, std::size_t at, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) { d[at + i] = static_cast<std::uint8_t>(v >> (8u * i)); d[at + 7 - i] = static_cast<std::uint8_t>(v >> (8u * i)); }
}
// Appends one ISO 9660 directory record (both-endian extent/size fields).
void record(std::vector<std::uint8_t> &d, std::size_t &at, const std::string &name, std::uint32_t extent,
            std::uint32_t size, bool directory) {
    const auto length = static_cast<std::uint8_t>(33 + name.size() + (name.size() % 2 == 0 ? 1 : 0));
    d[at] = length; put32(d, at + 2, extent); put32(d, at + 10, size);
    d[at + 25] = directory ? 2u : 0u; d[at + 28] = 1u; d[at + 32] = static_cast<std::uint8_t>(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) d[at + 33 + i] = static_cast<std::uint8_t>(name[i]);
    at += length;
}
std::vector<std::uint8_t> image(const std::string &file_name = "A.TXT;1", std::uint32_t file_extent = 20) {
    std::vector<std::uint8_t> d(22 * sector);
    auto *pvd = d.data() + 16 * sector;
    pvd[0] = 1; std::copy_n("CD001", 5, pvd + 1); pvd[6] = 1; pvd[128] = 0x00; pvd[129] = 0x08;
    std::size_t root = 16 * sector + 156; record(d, root, std::string(1, '\0'), 18, sector, true);
    d[17 * sector] = 255; std::copy_n("CD001", 5, d.data() + 17 * sector + 1);
    std::size_t at = 18 * sector;
    record(d, at, std::string(1, '\0'), 18, sector, true); record(d, at, std::string(1, '\1'), 18, sector, true);
    record(d, at, "DIR", 19, sector, true); record(d, at, file_name, file_extent, 5, false);
    at = 19 * sector;
    record(d, at, std::string(1, '\0'), 19, sector, true); record(d, at, std::string(1, '\1'), 18, sector, true);
    record(d, at, "B.BIN;1", 21, 3, false);
    std::copy_n("hello", 5, d.data() + 20 * sector); std::copy_n("xyz", 3, d.data() + 21 * sector);
    return d;
}
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("psprecomp-iso-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directory(root); }
    ~Fixture() { std::error_code e; if (root.filename().string().starts_with("psprecomp-iso-test-")) fs::remove_all(root, e); }
    fs::path write(const std::string &name, const std::vector<std::uint8_t> &data) const {
        const auto path = root / name; std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size())); return path;
    }
};
std::string read(const fs::path &path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
template<class F> void rejects(F action, const char *message) {
    bool failed = false; try { action(); } catch (const psprecomp::Error &) { failed = true; } require(failed, message);
}

void reads_nested_tree_and_extracts() {
    Fixture f; psprecomp::Iso9660Image iso(f.write("good.iso", image()));
    const auto &entries = iso.entries();
    require(entries.size() == 3 && entries[0].path == "DIR" && entries[0].directory && entries[1].path == "A.TXT" &&
            entries[2].path == "DIR/B.BIN" && entries[2].size == 3, "ISO tree was not read with version suffixes removed");
    iso.extract(entries[1], f.root / "a.txt"); iso.extract(entries[2], f.root / "b.bin");
    require(read(f.root / "a.txt") == "hello" && read(f.root / "b.bin") == "xyz", "Extracted bytes are wrong");
    rejects([&] { iso.extract(entries[1], f.root / "a.txt"); }, "Extraction overwrote an existing file");
}
void rejects_unsafe_images() {
    Fixture f;
    for (const auto *name : {"..;1", "A/B;1", "C:D;1", "BAD;X"})
        rejects([&] { psprecomp::Iso9660Image iso(f.write("bad.iso", image(name))); }, "Unsafe ISO name was accepted");
    rejects([&] { psprecomp::Iso9660Image iso(f.write("outside.iso", image("A.TXT;1", 900))); }, "Out-of-image extent was accepted");
    auto truncated = image(); truncated.resize(16 * sector + 100);
    rejects([&] { psprecomp::Iso9660Image iso(f.write("short.iso", truncated)); }, "Truncated ISO was accepted");
    auto multi = image(); multi[18 * sector + 34 + 34 + 36 + 25] |= 0x80u;
    rejects([&] { psprecomp::Iso9660Image iso(f.write("multi.iso", multi)); }, "Multi-extent ISO file was accepted");
}
void sha256_vectors_and_streaming() {
    require(psprecomp::sha256_bytes({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty vector");
    const std::string abc = "abc";
    require(psprecomp::sha256_bytes({reinterpret_cast<const std::uint8_t *>(abc.data()), abc.size()}) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc vector");
    const std::vector<std::uint8_t> million(1000000, 'a');
    psprecomp::Sha256 hash;
    for (std::size_t at = 0; at < million.size(); at += 7)
        hash.update({million.data() + at, std::min<std::size_t>(7, million.size() - at)});
    require(hash.hexdigest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 streamed million-a vector");
    Fixture f; const auto path = f.write("million.bin", million);
    require(psprecomp::sha256_file(path) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256_file vector");
}
}

int main() {
    try {
        reads_nested_tree_and_extracts();
        rejects_unsafe_images();
        sha256_vectors_and_streaming();
        std::cout << "ISO 9660 reader and streaming SHA-256 tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
