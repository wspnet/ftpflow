// ftpflow -- virtual file system types shared by every backend.
// Part of core.lib: pure C++17, no Qt, no third-party dependencies.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ftpflow {

enum class BackendKind : std::uint8_t {
    Local,   // Win32 local filesystem
    Ftp,     // plain FTP / FTPS via libcurl
    Sftp,    // SFTP via libssh2
};

enum class EntryType : std::uint8_t {
    File,
    Directory,
    Symlink,
    Other,
};

// One directory entry. Kept small and value-semantic so a listing of
// 100k entries stays cache friendly (blueprint perf budget: 10w rows).
struct Entry {
    std::string name;
    std::uint64_t size = 0;
    std::int64_t mtimeUnix = 0;   // seconds since epoch
    EntryType type = EntryType::Other;
    std::uint32_t permissions = 0;
};

// What a backend can actually do. The UI disables operations that are
// not supported instead of failing after a click (blueprint section 03).
struct Capabilities {
    bool structuredListing = false;  // MLSD (FTP) / readdir attrs (SFTP)
    bool utf8Paths = false;
    bool resume = false;             // REST / SFTP offset+append
    bool chmod = false;
    bool symlinkFollow = false;
    bool execChannel = false;        // SSH exec: the compression fast path
    bool timestampSubSecond = false;
};

// Directory page, fetched with a cursor held by the backend (never shipped
// wholesale to the UI layer).
struct ListingPage {
    std::vector<Entry> entries;
    std::string cursor;     // opaque continuation token
    bool hasMore = false;
};

}  // namespace ftpflow
