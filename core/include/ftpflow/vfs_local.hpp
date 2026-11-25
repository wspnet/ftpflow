// ftpflow -- local filesystem backend interface.
// Part of core.lib: pure C++17, no Qt.
#pragma once

#include <cstdint>
#include <string>

#include "ftpflow/vfs.hpp"

namespace ftpflow {

// Directory listing over the local Win32 filesystem, page-by-page.
class LocalFsBackend {
public:
    Capabilities capabilities() const;

    // Binds the backend to a root directory. Returns false with an error
    // message on failure; all later paths are relative to the root.
    bool open(std::string root, std::string* error = nullptr);

    // Lists one directory, at most maxEntries entries, continuing after
    // startAfter (an entry name). The returned cursor feeds the next call.
    ListingPage list(const std::string& dir,
                     const std::string& startAfter = {},
                     std::uint32_t maxEntries = 0);

    // Filesystem mutations. All paths are absolute UTF-8. Return an empty
    // string on success, or a human-readable error on failure.
    // `remove` deletes a file or an entire directory tree (recursive).
    std::string remove(const std::string& path);
    std::string rename(const std::string& from, const std::string& to);
    std::string mkdir(const std::string& path);

private:
    std::string root_;
};

}  // namespace ftpflow
