#include "badman_disc.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <limits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace badman {
namespace {
constexpr std::int32_t missing = static_cast<std::int32_t>(0x80010002u);
constexpr std::int32_t bad_fd = static_cast<std::int32_t>(0x80010009u);
constexpr std::int32_t async_busy = static_cast<std::int32_t>(0x80020329u);
constexpr std::int32_t no_async = static_cast<std::int32_t>(0x8002032Au);
constexpr std::int32_t invalid = static_cast<std::int32_t>(0x80010016u);
constexpr std::int32_t read_only = static_cast<std::int32_t>(0x8001001Eu);
constexpr std::int32_t io_error = static_cast<std::int32_t>(0x80010005u);
}
Disc::Disc(std::filesystem::path root) : root_(root.empty() ? root : std::filesystem::weakly_canonical(root)) {}
void Disc::mount_font_directory(std::filesystem::path root) {
    if (!root.empty()) {
        root = std::filesystem::canonical(root);
        if (!std::filesystem::is_directory(root)) throw psprecomp::Error("Firmware font mount must be a directory");
    }
    font_root_ = std::move(root);
}
bool Disc::present() const {
    if (root_.empty()) return false;
    const auto check = [&] { return std::filesystem::is_regular_file(root_ / "PSP_GAME/PARAM.SFO"); };
#if defined(_WIN32)
    if (!presence_watch_) {
        const auto handle = FindFirstChangeNotificationW(root_.c_str(), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
        if (handle == INVALID_HANDLE_VALUE) return check();
        presence_watch_.reset(handle, [](void *watch) { FindCloseChangeNotification(watch); });
        return presence_cached_ = check();
    }
    if (WaitForSingleObject(presence_watch_.get(), 0) != WAIT_TIMEOUT) {
        // Re-arm first, so a change during the lookup below is seen next time.
        if (!FindNextChangeNotification(presence_watch_.get())) presence_watch_.reset();
        presence_cached_ = check();
    }
    return presence_cached_;
#else
    return check();
#endif
}
void Disc::activate(const std::string &drive) {
    if (drive != "disc0:" || !present()) throw psprecomp::Error("UMD activation needs an extracted PSP_GAME root");
    active_ = true;
}
std::filesystem::path Disc::resolve(const std::string &name) const {
    return resolve_path(name).host;
}
Disc::Path Disc::resolve_path(const std::string &name) const {
    std::string relative = name;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const auto colon = relative.find(':');
    const bool device = colon != std::string::npos;
    bool disc_backed = true;
    if (device) {
        const auto name = relative.substr(0, colon);
        if (name != "disc0" && name != "flash0") throw psprecomp::Error("Unsupported file device");
        disc_backed = name == "disc0";
        relative.erase(0, colon + 1);
    }
    // A second colon is not a PSP disc-path separator and could select an NTFS
    // alternate data stream when these files are hosted on Windows.
    if (relative.find(':') != std::string::npos) throw psprecomp::Error("Invalid colon in disc path");
    if (!disc_backed) {
        if (relative != "/font" && !relative.starts_with("/font/"))
            throw psprecomp::Error("Only flash0:/font is mounted");
        relative.erase(0, 5);
    }
    const auto &root = disc_backed ? root_ : font_root_;
    if (root.empty()) throw psprecomp::Error("File device root is not configured");
    const bool absolute = !relative.empty() && relative.front() == '/';
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    std::filesystem::path clean;
    for (const auto &part : std::filesystem::path(relative)) {
        if (part == "..") throw psprecomp::Error("Disc path contains parent traversal");
        if (part != ".") clean /= part;
    }
    if (disc_backed && !device && !absolute && !relative.starts_with("PSP_GAME/")) clean = "PSP_GAME/USRDIR" / clean;
    const auto result = std::filesystem::weakly_canonical(root / clean);
    const auto inside = result.lexically_relative(root);
    if (inside.empty() || *inside.begin() == "..") throw psprecomp::Error("Disc path escapes the extracted root");
    return {result, disc_backed};
}
std::int32_t Disc::open(const std::string &name, std::uint32_t flags, bool async) {
    const auto path = resolve_path(name);
    if (path.disc_backed && (!active_ || !present())) return io_error;
    // Read access plus the optional PSP_O_NBLOCK bit; mutation flags are rejected.
    if ((flags & ~0x0005u) != 0 || (flags & 3u) != 1u) return read_only;
    if (!std::filesystem::is_regular_file(path.host)) return missing;
    File file;
    file.disc_backed = path.disc_backed;
    file.stream.open(path.host, std::ios::binary);
    if (!file.stream) return io_error;
    file.size = std::filesystem::file_size(path.host);
    const auto fd = next_fd_++;
    file.pending = async; file.result = fd;
    files_.emplace(fd, std::move(file)); ++opened_;
    return fd;
}
std::int64_t Disc::read(std::int32_t fd, std::uint32_t count, std::vector<std::uint8_t> &bytes, bool async) {
    const auto found = files_.find(fd);
    if (found == files_.end()) return bad_fd;
    auto &file = found->second;
    if (file.disc_backed && (!active_ || !present())) return io_error;
    if (file.pending) return async_busy;
    const auto available = file.position < file.size ? file.size - file.position : 0;
    const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(count, available));
    bytes.resize(length);
    if (length) {
        file.stream.clear(); file.stream.seekg(static_cast<std::streamoff>(file.position));
        file.stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(length));
        if (file.stream.gcount() != static_cast<std::streamsize>(length)) return io_error;
    }
    file.position += length; read_ += length;
    file.pending = async; file.result = static_cast<std::int64_t>(length);
    return static_cast<std::int64_t>(length);
}
std::int64_t Disc::seek(std::int32_t fd, std::int64_t offset, std::uint32_t whence) {
    const auto found = files_.find(fd);
    if (found == files_.end()) return bad_fd;
    auto &file = found->second;
    if (file.disc_backed && (!active_ || !present())) return io_error;
    if (file.pending) return async_busy;
    if (whence > 2) return invalid;
    const auto raw_base = whence == 0 ? 0 : whence == 1 ? file.position : file.size;
    if (raw_base > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return invalid;
    const auto base = static_cast<std::int64_t>(raw_base);
    if (offset < -base || (offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset)) return invalid;
    file.position = static_cast<std::uint64_t>(base + offset);
    return static_cast<std::int64_t>(file.position);
}
std::int32_t Disc::close(std::int32_t fd) {
    const auto found = files_.find(fd);
    if (found == files_.end()) return bad_fd;
    if (found->second.pending) return async_busy;
    files_.erase(found); return 0;
}
std::int32_t Disc::completion(std::int32_t fd, std::int64_t &result) {
    const auto found = files_.find(fd);
    if (found == files_.end()) return bad_fd;
    auto &file = found->second;
    if (!file.pending) return no_async;
    file.pending = false;
    result = file.result;
    return 0;
}
std::int32_t Disc::stat(const std::string &name, Stat &result) const {
    try {
        const auto path = resolve_path(name);
        if (path.disc_backed && (!active_ || !present())) return io_error;
        std::error_code error;
        const auto status = std::filesystem::status(path.host, error);
        if (error) return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory ? missing : io_error;
        if (!std::filesystem::exists(status)) return missing;
        Stat value;
        value.directory = std::filesystem::is_directory(status);
        if (!value.directory) {
            if (!std::filesystem::is_regular_file(status)) return invalid;
            value.size = std::filesystem::file_size(path.host, error);
            if (error) return io_error;
        }
        result = value;
        return 0;
    } catch (const psprecomp::Error &) {
        return invalid;
    } catch (const std::filesystem::filesystem_error &) {
        return io_error;
    }
}
}
