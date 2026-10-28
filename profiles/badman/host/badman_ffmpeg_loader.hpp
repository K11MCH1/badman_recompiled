#pragma once
// Runtime loading of the separately distributed, pinned LGPL FFmpeg shared
// libraries: versioned DLLs on Windows, unversioned .so files on Android. The
// adapters call FFmpeg's public API only; no FFmpeg code is linked into them.
#include "psprecomp/common.hpp"
#include <filesystem>
#include <string>
#include <vector>
#if defined(BADMAN_FFMPEG_HEADERS) && (defined(_WIN32) || defined(__ANDROID__))
#define BADMAN_FFMPEG_RUNTIME 1
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace badman {
class FfmpegLibraries {
public:
#if defined(_WIN32)
    using Module = HMODULE;
#else
    using Module = void *;
#endif
    explicit FfmpegLibraries(const char *kind) : kind_(kind) {}
    ~FfmpegLibraries() { close_all(); }
    FfmpegLibraries(const FfmpegLibraries &) = delete;
    FfmpegLibraries &operator=(const FfmpegLibraries &) = delete;
    // base "avcodec", major 62: avcodec-62.dll on Windows, libavcodec.so on Android.
    Module open(const std::filesystem::path &directory, const char *base, int major) {
#if defined(_WIN32)
        const auto path = std::filesystem::absolute(directory / (std::string(base) + "-" + std::to_string(major) + ".dll"));
        const auto module = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) throw psprecomp::Error("Cannot load " + kind_ + " codec DLL: " + path.string());
#else
        (void)major; // Android builds use unversioned sonames; headers pin the major.
        const auto path = std::filesystem::absolute(directory / ("lib" + std::string(base) + ".so"));
        const auto module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module) throw psprecomp::Error("Cannot load " + kind_ + " codec library: " + path.string() + ": " + dlerror());
#endif
        modules_.push_back(module); return module;
    }
    template<class Function> Function find(Module module, const char *name) const {
#if defined(_WIN32)
        const auto address = GetProcAddress(module, name);
#else
        const auto address = dlsym(module, name);
#endif
        if (!address) throw psprecomp::Error("Missing " + kind_ + " codec public API: " + name);
        return reinterpret_cast<Function>(address);
    }
    void close_all() noexcept {
        for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
#if defined(_WIN32)
            FreeLibrary(*it);
#else
            dlclose(*it);
#endif
        }
        modules_.clear();
    }
private:
    std::string kind_;
    std::vector<Module> modules_;
};
}
#endif
