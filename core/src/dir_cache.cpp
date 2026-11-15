// ftpflow -- directory listing cache implementation.
#include "ftpflow/dir_cache.hpp"

#include <algorithm>

namespace ftpflow {

DirCache::DirCache(std::size_t maxEntries) : maxEntries_(maxEntries) {}

void DirCache::evictLocked(std::size_t incoming) {
    if (curEntries_ + incoming <= maxEntries_) return;
    // Drop least-recently-used nodes until the incoming snapshot fits.
    // Stale nodes (null snapshot) hold no entries but still cost a map
    // slot, so they go first when they are also the least recently used.
    std::vector<std::uint64_t> stamps;
    stamps.reserve(cache_.size());
    for (const auto& kv : cache_) stamps.push_back(kv.second.lastUsed);
    std::sort(stamps.begin(), stamps.end());
    std::size_t need = curEntries_ + incoming - maxEntries_;
    for (std::uint64_t victim : stamps) {
        if (need == 0) break;
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.lastUsed == victim) {
                const std::size_t sz =
                    it->second.entries ? it->second.entries->size() : 0;
                need = need > sz ? need - sz : 0;
                curEntries_ -= sz;
                cache_.erase(it);
                ++stats_.evictions;
                break;
            }
        }
    }
}

ListingPage DirCache::list(LocalFsBackend& fs, const std::string& dir,
                           const std::string& startAfter,
                           std::uint32_t maxEntries, bool* fromCache) {
    ListingPage page;
    if (maxEntries == 0) maxEntries = 256;
    if (fromCache) *fromCache = false;

    // 1) Grab a snapshot under the lock (a shared_ptr bump, not a copy of
    //    a potentially 100k-entry listing).
    std::shared_ptr<const std::vector<Entry>> snap;
    {
        std::lock_guard<std::mutex> g(mu_);
        ++tick_;
        auto it = cache_.find(dir);
        if (it != cache_.end() && it->second.entries) {
            snap = it->second.entries;
            it->second.lastUsed = tick_;
            ++stats_.hits;
            if (fromCache) *fromCache = true;
        } else {
            ++stats_.misses;
        }
    }

    // 2) Miss (or stale): enumerate outside the lock, then publish.
    if (!snap) {
        // The backend sorts internally; ask for "everything" in one page.
        ListingPage full = fs.list(dir, {}, 0xFFFFFFFFu);
        snap = std::make_shared<const std::vector<Entry>>(
            std::move(full.entries));

        std::lock_guard<std::mutex> g(mu_);
        auto it = cache_.find(dir);
        if (snap->size() <= maxEntries_) {
            // Bookkeeping for the node being replaced (stale nodes keep
            // their slot but released their snapshot on invalidate()).
            if (it != cache_.end() && it->second.entries)
                curEntries_ -= it->second.entries->size();
            evictLocked(snap->size());
            Node n;
            n.entries = snap;
            n.lastUsed = tick_;
            if (it != cache_.end())
                it->second = std::move(n);
            else
                cache_.emplace(dir, std::move(n));
            curEntries_ += snap->size();
        } else {
            // Too big for the budget: serve uncached and drop any node.
            if (it != cache_.end() && it->second.entries) {
                curEntries_ -= it->second.entries->size();
                it->second.entries.reset();
            }
        }
    }

    // 3) Slice the requested page (same cursor semantics as the backend:
    //    upper_bound over the sorted names, last name becomes the cursor).
    const std::vector<Entry>& listing = *snap;
    std::size_t i = 0;
    if (!startAfter.empty()) {
        auto pos = std::upper_bound(listing.begin(), listing.end(),
                                    startAfter,
            [](const std::string& key, const Entry& e) { return key < e.name; });
        i = static_cast<std::size_t>(pos - listing.begin());
    }
    for (; i < listing.size() && page.entries.size() < maxEntries; ++i) {
        page.entries.push_back(listing[i]);
    }
    page.hasMore = i < listing.size();
    if (page.hasMore && !page.entries.empty()) {
        page.cursor = page.entries.back().name;
    }

    {
        std::lock_guard<std::mutex> g(mu_);
        stats_.dirsCached = cache_.size();
        stats_.entriesCached = curEntries_;
    }
    return page;
}

void DirCache::invalidate(const std::string& dir) {
    std::lock_guard<std::mutex> g(mu_);
    if (dir.empty()) {
        for (auto& kv : cache_) {
            if (kv.second.entries) {
                curEntries_ -= kv.second.entries->size();
                kv.second.entries.reset();   // release stale memory now
                ++stats_.invalidations;
            }
        }
        return;
    }
    // Exact dir plus every cached subdirectory under it (a rename/delete of
    // `dir` also makes all cached descendants stale).
    std::string prefix = dir;
    while (!prefix.empty() &&
           (prefix.back() == '\\' || prefix.back() == '/'))
        prefix.pop_back();
    for (auto it = cache_.lower_bound(prefix); it != cache_.end(); ++it) {
        const std::string& key = it->first;
        if (key != prefix &&
            (key.size() <= prefix.size() ||
             key.compare(0, prefix.size(), prefix) != 0 ||
             (key[prefix.size()] != '\\' && key[prefix.size()] != '/')))
            break;
        if (it->second.entries) {
            curEntries_ -= it->second.entries->size();
            it->second.entries.reset();
            ++stats_.invalidations;
        }
    }
}

void DirCache::clear() {
    std::lock_guard<std::mutex> g(mu_);
    cache_.clear();
    curEntries_ = 0;
    stats_.dirsCached = 0;
    stats_.entriesCached = 0;
}

DirCache::Stats DirCache::stats() const {
    std::lock_guard<std::mutex> g(mu_);
    Stats s = stats_;
    s.dirsCached = cache_.size();
    s.entriesCached = curEntries_;
    return s;
}

}  // namespace ftpflow
