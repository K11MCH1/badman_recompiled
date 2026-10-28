#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace badman {
class Disc {
public:
    explicit Disc(std::filesystem::path root);
    void mount_font_directory(std::filesystem::path root);
    bool present() const;
    void activate(const std::string &drive);
    void deactivate() { active_ = false; }
    std::uint32_t status() const { return present() ? (active_ ? 0x32u : 0x12u) : 1u; }
    std::int32_t open(const std::string &name, std::uint32_t flags, bool async = false);
    std::int64_t read(std::int32_t fd, std::uint32_t count, std::vector<std::uint8_t> &bytes, bool async = false);
    std::int64_t seek(std::int32_t fd, std::int64_t offset, std::uint32_t whence);
    std::int32_t close(std::int32_t fd);
    std::int32_t completion(std::int32_t fd, std::int64_t &result);
    struct Stat { std::uint64_t size{}; bool directory{}; };
    std::int32_t stat(const std::string &name, Stat &result) const;
    std::filesystem::path resolve(const std::string &name) const;
    std::uint64_t files_opened() const { return opened_; }
    std::uint64_t bytes_read() const { return read_; }
private:
    struct File {
        std::ifstream stream;
        std::uint64_t size{}, position{};
        bool pending{};
        bool disc_backed{true};
        std::int64_t result{};
    };
    std::filesystem::path root_;
    std::filesystem::path font_root_;
    // The game polls the drive state many times per frame. On Windows a
    // change notification under the data root says when PARAM.SFO must be
    // looked up again; until then the last answer stands.
    mutable std::shared_ptr<void> presence_watch_;
    mutable bool presence_cached_{};
    struct Path { std::filesystem::path host; bool disc_backed; };
    Path resolve_path(const std::string &name) const;
    std::map<std::int32_t, File> files_;
    std::int32_t next_fd_{3};
    bool active_{};
    std::uint64_t opened_{}, read_{};
};
}
