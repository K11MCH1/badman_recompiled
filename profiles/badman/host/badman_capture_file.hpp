#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <chrono>
#include <filesystem>

namespace badman {
// The caller has finished and closed its own pending capture. Only an actual
// successful replacement returns ERROR_SUCCESS. Failed attempts never remove
// the old frame or repair its permissions/attributes.
inline DWORD replace_capture_file(const std::filesystem::path &pending,
                                  const std::filesystem::path &destination) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    for (;;) {
        if (MoveFileExW(pending.c_str(), destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return ERROR_SUCCESS;
        const auto error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION)
            return error;
        if (error == ERROR_ACCESS_DENIED) {
            const auto attributes = GetFileAttributesW(destination.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)))
                return error;
        }
        if (std::chrono::steady_clock::now() >= deadline) return error;
        // Windows replacement can report ACCESS_DENIED even for an ordinary
        // share-delete reader. Retry the same complete pending file briefly;
        // a persistent ACL/reader failure still returns its exact error.
        Sleep(1);
    }
}
}
#endif
