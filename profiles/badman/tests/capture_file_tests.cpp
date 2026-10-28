#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace observation {
unsigned moves{}, sleeps{};
HANDLE *close_on_sleep{};
bool release_succeeded{};
BOOL WINAPI move(LPCWSTR source, LPCWSTR target, DWORD flags) {
    ++moves;
    return ::MoveFileExW(source, target, flags);
}
void WINAPI sleep(DWORD duration) {
    ++sleeps;
    // A real blocked first attempt has occurred. Release only the fixture's
    // owned reader at the actual backoff, without a scheduler-sensitive delay.
    if (close_on_sleep) {
        release_succeeded = ::CloseHandle(*close_on_sleep) != 0;
        if (release_succeeded) *close_on_sleep = INVALID_HANDLE_VALUE;
        close_on_sleep = nullptr;
    }
    ::Sleep(duration);
}
}
// Count real calls and synchronize fixture handle release. API return values
// are never forced; the production helper's retry algorithm is not duplicated.
#define MoveFileExW observation::move
#define Sleep observation::sleep
#include "badman_capture_file.hpp"
#undef MoveFileExW
#undef Sleep

namespace {
namespace fs = std::filesystem;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE input) : value(input) {
        require(value != INVALID_HANDLE_VALUE, "CreateFileW failed");
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void close() {
        if (value != INVALID_HANDLE_VALUE) {
            require(CloseHandle(value) != 0, "CloseHandle failed");
            value = INVALID_HANDLE_VALUE;
        }
    }
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
const std::string old_bytes = "OLD_COMPLETE|" + std::string(4096, 'o');
const std::string new_bytes = "NEW_COMPLETE|" + std::string(8192, 'n');
void write(const fs::path &path, const std::string &bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    require(bool(file), "Fixture write failed");
}
std::string read(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "Fixture read failed");
    return std::string(std::istreambuf_iterator<char>(file), {});
}
std::string read(HANDLE handle) {
    require(SetFilePointerEx(handle, {}, nullptr, FILE_BEGIN) != 0, "Held seek failed");
    std::string bytes(16384, '\0');
    DWORD count{};
    require(ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) != 0,
            "Held read failed");
    bytes.resize(count);
    return bytes;
}
struct OwnedRoot {
    fs::path path;
    OwnedRoot() {
        std::wstring module(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        require(count > 0 && count < module.size(), "Cannot locate capture test executable");
        module.resize(count);
        const auto binary_directory = fs::canonical(fs::path(module).parent_path());
        const auto fixtures = binary_directory / L"capture-fixtures";
        fs::create_directory(fixtures);
        require((GetFileAttributesW(fixtures.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
                "Fixture parent must not be a reparse point");
        const auto parent = fs::canonical(fixtures);
        require(parent.parent_path() == binary_directory && parent.filename() == L"capture-fixtures",
                "Fixture parent escaped the test binary directory");
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto name = L"badman-capture-test-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(stamp) + L"-" + std::to_wstring(attempt);
            auto candidate = parent / name;
            if (CreateDirectoryW(candidate.c_str(), nullptr)) {
                path = fs::canonical(candidate);
                require(path.parent_path() == parent && path.filename() == name,
                        "Owned fixture escaped its binary-tree parent");
                std::cout << "Capture file fixture: " << path.filename().string() << '\n';
                return;
            }
            require(GetLastError() == ERROR_ALREADY_EXISTS, "Cannot create owned fixture");
        }
        throw std::runtime_error("No unique capture fixture name available");
    }
    // Retain every unique fixture, including failures and readonly attributes,
    // under the binary tree for diagnostics. Tests never remove external paths.
};
struct Fixture {
    fs::path directory, pending, target;
    Fixture(const fs::path &root, const wchar_t *name)
        : directory(root / name), pending(directory / L"frame.pending"), target(directory / L"live.bmp") {
        require(fs::create_directory(directory), "Fixture already exists");
        write(pending, new_bytes); write(target, old_bytes);
    }
    void unchanged() const {
        require(read(pending) == new_bytes && read(target) == old_bytes,
                "Failed publication changed old/pending bytes");
    }
    void published() const {
        require(!fs::exists(pending) && read(target) == new_bytes,
                "Successful publication did not install complete new bytes");
    }
};
struct Result { DWORD error; unsigned moves, sleeps; double milliseconds; };
Result invoke(const Fixture &fixture) {
    observation::moves = observation::sleeps = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto error = badman::replace_capture_file(fixture.pending, fixture.target);
    return {error, observation::moves, observation::sleeps,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()};
}
DWORD blocked_native_move(const Fixture &fixture) {
    require(!::MoveFileExW(fixture.pending.c_str(), fixture.target.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "Source reader did not establish real blocked replacement");
    const auto error = GetLastError();
    require(error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION,
            "Unexpected native contention error");
    fixture.unchanged();
    return error;
}
void bounded_failure(const Result &result, DWORD expected) {
    require(result.error == expected, "Retry changed the observed native error");
    require(result.moves > 1 && result.sleeps > 0, "Contention was not retried");
    require(result.milliseconds >= 80, "Persistent contention returned before the retry deadline");
    // The 100-ms deadline bounds retry decisions, not kernel IO or scheduling.
    // CTest supplies the external watchdog instead of asserting an IO latency.
}
void run(const fs::path &root) {
    { Fixture f(root, L"normal"); const auto r = invoke(f);
      require(r.error == 0 && r.moves == 1 && r.sleeps == 0, "Normal publication failed"); f.published(); }
    { Fixture f(root, L"absent_target"); require(DeleteFileW(f.target.c_str()) != 0, "Owned delete failed");
      const auto r = invoke(f); require(r.error == 0 && r.moves == 1, "Absent target publication failed"); f.published(); }
    { Fixture f(root, L"released_source_reader");
      Handle reader(CreateFileW(f.pending.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr));
      blocked_native_move(f);
      observation::release_succeeded = false; observation::close_on_sleep = &reader.value;
      const auto r = invoke(f); observation::close_on_sleep = nullptr;
      require(observation::release_succeeded && r.error == 0 && r.moves > 1 && r.sleeps > 0,
              "Same pending file did not publish after releasing a real blocked reader"); f.published(); }
    { Fixture f(root, L"persistent_source_reader");
      Handle reader(CreateFileW(f.pending.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr));
      const auto observed = blocked_native_move(f);
      const auto old_time = fs::last_write_time(f.target), pending_time = fs::last_write_time(f.pending);
      bounded_failure(invoke(f), observed); f.unchanged();
      require(read(reader.value) == new_bytes, "Held source bytes changed");
      require(fs::last_write_time(f.target) == old_time && fs::last_write_time(f.pending) == pending_time,
              "Failed replacement changed timestamps");
      reader.close(); require(invoke(f).error == 0, "Source did not recover after close"); f.published(); }
    { Fixture f(root, L"target_share_delete_reader");
      Handle reader(CreateFileW(f.target.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr));
      const auto r = invoke(f);
      // Windows/filesystem versions may allow this replacement immediately or
      // retain the target name while its handle is open. Test either real result.
      if (r.error == 0) f.published();
      else {
          require(r.error == ERROR_ACCESS_DENIED || r.error == ERROR_SHARING_VIOLATION,
                  "Unexpected target-reader publication error");
          bounded_failure(r, r.error); f.unchanged();
      }
      require(read(reader.value) == old_bytes, "Held old target changed");
      reader.close();
      if (r.error != 0) { require(invoke(f).error == 0, "Target did not recover after close"); f.published(); }
    }
    { Fixture f(root, L"readonly_target");
      require(SetFileAttributesW(f.target.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "Set readonly failed");
      const auto attrs = GetFileAttributesW(f.target.c_str()); const auto r = invoke(f);
      require(r.error == ERROR_ACCESS_DENIED && r.moves == 1 && r.sleeps == 0,
              "Readonly publication failure was not immediate"); f.unchanged();
      require(GetFileAttributesW(f.target.c_str()) == attrs, "Readonly attributes changed"); }
    { Fixture f(root, L"directory_target"); require(DeleteFileW(f.target.c_str()) != 0, "Owned delete failed");
      require(fs::create_directory(f.target), "Create target directory failed"); write(f.target / L"sentinel", old_bytes);
      const auto r = invoke(f); require(r.error == ERROR_ACCESS_DENIED && r.moves == 1 && r.sleeps == 0,
                                       "Directory publication failure was not immediate");
      require(read(f.pending) == new_bytes && read(f.target / L"sentinel") == old_bytes,
              "Directory or sentinel changed"); }
    { Fixture f(root, L"missing_parent"); const auto original = f.target; f.target = f.directory / L"absent/live.bmp";
      const auto r = invoke(f); require(r.error == ERROR_PATH_NOT_FOUND && r.moves == 1 && r.sleeps == 0,
                                       "Missing-parent publication error was not immediate/exact");
      require(!fs::exists(f.target.parent_path()) && read(original) == old_bytes && read(f.pending) == new_bytes,
              "Missing parent, old file or pending source was modified"); }
}
}
int main() {
    try {
        OwnedRoot root; run(root.path);
        std::cout << "Capture file tests passed\n"; return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
