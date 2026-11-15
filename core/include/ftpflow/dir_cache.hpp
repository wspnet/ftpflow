// ftpflow -- directory listing cache with lazy invalidation.
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// The local backend re-enumerates a directory on every paged list() call;
// for a 100k-entry directory that is ~8s of repeated FindFirstFile+sort
// work just to page through the UI (measured in M0). DirCache stores the
// full sorted listing once and serves every page after that as a binary
// search + memcpy. Stale entries are re-fetched lazily on the next list()
// after invalidate() (driven by FsWatcher / ReadDirectoryChangesW).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "ftpflow/vfs.hpp"
#include "ftpflow/vfs_local.hpp"

namespace ftpflow {

class DirCache {
public:
    struct Stats {
        std::uint64_t hits = 0;          // pages served from memory
        std::uint64_t misses = 0;        // pages that triggered enumeration
        std::uint64_t invalidations = 0; // invalidate() calls on live nodes
        std::uint64_t evictions = 0;     // nodes dropped by the size cap
        std::size_t dirsCached = 0;
        std::size_t entriesCached = 0;
    };

    // `maxEntries` caps total cached entries across all directories (the
    // blueprint memory budget); a single directory larger than the cap is
    // not cached at all (still correct, just slower).
    explicit DirCache(std::size_t maxEntries = 400'000);

    // Paged listing with the same cursor contract as LocalFsBackend::list.
    // `fromCache` (optional) reports whether the page came from memory.
    // Hits take a shared_ptr snapshot under the lock and slice outside it,
    // so a 100k-entry directory pages without copying the whole listing.
    // Thread-safe.
    ListingPage list(LocalFsBackend& fs, const std::string& dir,
                     const std::string& startAfter = {},
                     std::uint32_t maxEntries = 0,
                     bool* fromCache = nullptr);

    // Marks `dir` (and every cached subdirectory under it) stale; the next
    // list() re-enumerates. Invalidated nodes release their snapshot
    // immediately (stale data is worthless, the memory is not). Cheap,
    // thread-safe.
    void invalidate(const std::string& dir);

    void clear();
    Stats stats() const;

private:
    struct Node {
        std::shared_ptr<const std::vector<Entry>> entries;  // null = stale
        std::uint64_t lastUsed = 0;
    };

    void evictLocked(std::size_t incoming);

    mutable std::mutex mu_;
    std::map<std::string, Node> cache_;  // ordered map -> prefix invalidation
    std::size_t maxEntries_;
    std::size_t curEntries_ = 0;
    std::uint64_t tick_ = 0;
    Stats stats_;
};

}  // namespace ftpflow
