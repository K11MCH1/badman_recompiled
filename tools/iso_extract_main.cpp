#include "psprecomp/common.hpp"
#include "psprecomp/iso9660.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// Extracts one subtree of a UMD ISO into a new directory. Used where no
// external archiver exists (Android); the caller verifies the image hash.
int main(int argc, char **argv) {
    try {
        if (argc < 3) {
            std::cerr << "Usage: psp_iso_extract <image.iso> <new output directory> [--subtree PATH] [--exclude PATH]...\n";
            return 2;
        }
        const std::filesystem::path output = argv[2];
        std::string subtree;
        std::vector<std::string> excluded;
        for (int index = 3; index < argc; ++index) {
            const std::string option = argv[index];
            if (index + 1 >= argc) throw psprecomp::Error("Missing value for " + option);
            if (option == "--subtree") subtree = argv[++index];
            else if (option == "--exclude") excluded.emplace_back(argv[++index]);
            else throw psprecomp::Error("Unknown option: " + option);
        }
        if (std::filesystem::exists(std::filesystem::symlink_status(output)))
            throw psprecomp::Error("Output directory must not exist yet: " + output.string());
        psprecomp::Iso9660Image image(argv[1]);
        const auto selected = [&](const std::string &path) {
            const auto under = [&](const std::string &root) {
                return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
            };
            if (!subtree.empty() && !under(subtree)) return false;
            for (const auto &root : excluded) if (under(root)) return false;
            return true;
        };
        std::filesystem::create_directory(output);
        std::size_t files = 0;
        std::uint64_t bytes = 0;
        for (const auto &entry : image.entries()) {
            if (!selected(entry.path)) continue;
            const auto target = output / std::filesystem::path(entry.path);
            if (entry.directory) { std::filesystem::create_directories(target); continue; }
            std::filesystem::create_directories(target.parent_path());
            image.extract(entry, target);
            ++files; bytes += entry.size;
        }
        std::cout << "Extracted " << files << " files, " << bytes << " bytes\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ISO extraction failed: " << error.what() << '\n';
        return 1;
    }
}
