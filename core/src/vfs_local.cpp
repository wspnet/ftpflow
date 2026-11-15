// ftpflow -- local filesystem backend (Win32 native).
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// Listing uses FindFirstFileExW with FIND_FIRST_EX_LARGE_FETCH -- the
// Windows-native bulk enumeration path (blueprint section 06 / M0 step 3).
// Names cross the API boundary as UTF-8.
#include "ftpflow/vfs_local.hpp"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace ftpflow {

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string joinPath(const std::string& base, const std::wstring& leaf) {
    std::string b = base;
    while (!b.empty() && (b.back() == '\\' || b.back() == '/')) b.pop_back();
    return b + "\\" + wideToUtf8(leaf);
}

// FILETIME (100ns since 1601) -> unix seconds.
std::int64_t filetimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochOffset = 116444736000000000ULL;
    if (u.QuadPart < kUnixEpochOffset) return 0;
    return (std::int64_t)((u.QuadPart - kUnixEpochOffset) / 10000000ULL);
}

}  // namespace

Capabilities LocalFsBackend::capabilities() const {
    Capabilities c;
    c.structuredListing = true;
    c.utf8Paths = true;
    c.resume = true;             // local file offsets
    c.chmod = false;             // POSIX chmod is meaningless on Windows ACLs
    c.symlinkFollow = true;
    c.execChannel = false;       // local "remote agent" = child process, M1
    c.timestampSubSecond = true; // FILETIME granularity: 100 ns
    return c;
}

bool LocalFsBackend::open(std::string root, std::string* error) {
    root_ = std::move(root);
    DWORD attrs = GetFileAttributesW(utf8ToWide(root_).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error) *error = "not a directory";
        return false;
    }
    return true;
}

ListingPage LocalFsBackend::list(const std::string& dir,
                                 const std::string& startAfter,
                                 std::uint32_t maxEntries) {
    ListingPage page;
    if (maxEntries == 0) maxEntries = 256;

    const std::string base = dir.empty() ? root_ : dir;
    const std::wstring pattern = utf8ToWide(base) + L"\\*";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        return page;  // empty listing; error surfaced via open()/errno upstream
    }

    std::vector<Entry> all;
    do {
        const std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        Entry e;
        e.name = wideToUtf8(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            e.type = EntryType::Directory;
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            e.type = EntryType::Symlink;
        } else {
            e.type = EntryType::File;
        }
        e.size = ((std::uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.mtimeUnix = filetimeToUnix(fd.ftLastWriteTime);
        e.permissions = 0;
        all.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });

    std::size_t i = 0;
    if (!startAfter.empty()) {
        // upper_bound: the cursor row itself was already delivered.
        auto it = std::upper_bound(all.begin(), all.end(), startAfter,
            [](const std::string& key, const Entry& e) { return key < e.name; });
        i = static_cast<std::size_t>(it - all.begin());
    }
    for (; i < all.size() && page.entries.size() < maxEntries; ++i) {
        page.entries.push_back(std::move(all[i]));
    }
    page.hasMore = i < all.size();
    if (page.hasMore && !page.entries.empty()) {
        page.cursor = page.entries.back().name;
    }
    return page;
}

}  // namespace ftpflow
