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

private:
    std::string root_;
};

}  // namespace ftpflow
