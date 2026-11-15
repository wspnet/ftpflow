// test_dir_cache -- DirCache paging + FsWatcher invalidation.
//
// NOTE: never put side-effecting calls inside CHECK() -- release builds
// define NDEBUG and the expression is never evaluated. All opens/watches
// happen as plain statements first.
//
// Asserts:
//   1. miss/hit statistics and cache-served pages
//   2. paged (cursor) walk == fresh full enumeration, no gaps/overlaps
//   3. invalidate() -> next list() is a miss and reflects new contents
//   4. FsWatcher: create/modify/delete in a watched dir invalidates within
//      a generous timeout (ReadDirectoryChangesW latency varies)
//   5. cache eviction respects the entry cap
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "ftpflow/dir_cache.hpp"
#include "ftpflow/fs_watcher.hpp"
#include "ftpflow/vfs_local.hpp"


static int g_checks = 0;
static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                 \
        }                                                               \
    } while (0)

namespace {

std::string tempDir() {
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    std::string d = std::string(buf) + "ftpflow_dircache_test";
    CreateDirectoryA(d.c_str(), nullptr);
    // Start empty.
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::string(fd.cFileName) != "." && std::string(fd.cFileName) != "..") {
                DeleteFileA((d + "\\" + fd.cFileName).c_str());
                RemoveDirectoryA((d + "\\" + fd.cFileName).c_str());
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return d;
}

std::string join(const std::string& a, const std::string& b) {
    return a + "\\" + b;
}

int createFile(const std::string& path, const char* data = nullptr,
               DWORD size = 0) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (data) WriteFile(h, data, size, &size, nullptr);
    CloseHandle(h);
    return 1;
}

}  // namespace

int main() {
    const std::string dir = tempDir();
    std::printf("test dir: %s\n", dir.c_str());

    // --- fixture: 500 files ---
    for (int i = 0; i < 500; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "f%04d.txt", i);
        createFile(join(dir, name));
    }

    ftpflow::LocalFsBackend fs;
    std::string err;
    const bool opened = fs.open(dir, &err);
    CHECK(opened);

    // Cache key = full directory path (same convention the GUI model uses;
    // FsWatcher callbacks report full paths and must match cache keys).
    ftpflow::DirCache cache;
    bool fromCache = true;
    ftpflow::ListingPage p1 = cache.list(fs, dir, {}, 100, &fromCache);
    CHECK(!fromCache);          // first call enumerates
    CHECK(p1.entries.size() == 100);
    CHECK(p1.hasMore);

    ftpflow::ListingPage p2 = cache.list(fs, dir, p1.cursor, 100, &fromCache);
    CHECK(fromCache);           // second call served from memory
    CHECK(p2.entries.size() == 100);
    CHECK(p2.entries.front().name > p1.entries.back().name);  // sorted walk

    // Walk everything through the cache, compare with direct backend.
    std::vector<std::string> viaCache, viaBackend;
    ftpflow::ListingPage page = cache.list(fs, dir, {}, 500000);
    for (const auto& e : page.entries) viaCache.push_back(e.name);
    ftpflow::ListingPage direct = fs.list(dir, {}, 500000);
    for (const auto& e : direct.entries) viaBackend.push_back(e.name);
    std::printf("[dbg] cache full walk: %zu, direct backend: %zu\n",
                viaCache.size(), viaBackend.size());
    CHECK(viaCache.size() == 500);
    CHECK(viaBackend.size() == 500);
    CHECK(viaCache == viaBackend);   // exact same sorted set

    // Page-by-page concatenation also equals the whole listing.
    std::vector<std::string> paged;
    page = cache.list(fs, dir, {}, 100);
    while (true) {
        for (const auto& e : page.entries) paged.push_back(e.name);
        if (!page.hasMore) break;
        page = cache.list(fs, dir, page.cursor, 100);
    }
    CHECK(paged == viaBackend);
    std::printf("[ok] paged walk == full enumeration (500 files)\n");

    // --- 3: manual invalidate forces a miss + refresh ---
    cache.invalidate(dir);
    createFile(join(dir, "zz_new.txt"));
    page = cache.list(fs, dir, {}, 500000, &fromCache);
    CHECK(!fromCache);          // stale -> re-enumerated
    CHECK(viaCache.size() + 1 == page.entries.size());
    CHECK(page.entries.back().name == "zz_new.txt");
    DeleteFileA(join(dir, "zz_new.txt").c_str());
    cache.invalidate(dir);
    std::printf("[ok] invalidate -> miss -> refreshed listing\n");

    auto stats = cache.stats();
    std::printf("[stats] hits=%llu misses=%llu invalidations=%llu dirs=%zu entries=%zu\n",
                (unsigned long long)stats.hits, (unsigned long long)stats.misses,
                (unsigned long long)stats.invalidations, stats.dirsCached,
                stats.entriesCached);
    CHECK(stats.entriesCached == 0);     // invalidate() released the stale
                                         // snapshot immediately by design
    CHECK(stats.dirsCached == 1);        // node slot retained for reuse

    // --- 4: FsWatcher invalidates on external change ---
    ftpflow::DirCache cache2;
    bool seenChange = false;
    ftpflow::FsWatcher watcher([&](const std::string& changed) {
        cache2.invalidate(changed);
        seenChange = true;
    });
    const bool watching = watcher.watch(dir);
    CHECK(watching);
    CHECK(watcher.watchedCount() == 1);
    CHECK(watcher.watch(dir));   // idempotent
    CHECK(watcher.watchedCount() == 1);

    // Prime the cache.
    cache2.list(fs, dir, {}, 500000, &fromCache);
    CHECK(!fromCache);

    // External create: wait until the watcher invalidates and the listing
    // refreshes (timeout 10s; ReadDirectoryChangesW is usually <100 ms).
    createFile(join(dir, "watched_new.bin"));

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(10);
    bool reflected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        page = cache2.list(fs, dir, {}, 500000);
        for (const auto& e : page.entries) {
            if (e.name == "watched_new.bin") { reflected = true; break; }
        }
        if (reflected) break;
        Sleep(50);
    }
    CHECK(reflected);
    CHECK(seenChange);
    std::printf("[ok] watcher fired on create; listing refreshed\n");

    // External modify: size must be reflected.
    const std::string victim = join(dir, "f0000.txt");
    createFile(victim, "1234567890", 10);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::uint64_t newSize = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        page = cache2.list(fs, dir, {}, 500000);
        for (const auto& e : page.entries) {
            if (e.name == "f0000.txt") { newSize = e.size; break; }
        }
        if (newSize == 10) break;
        Sleep(50);
    }
    CHECK(newSize == 10);
    std::printf("[ok] watcher picked up size change\n");

    // External delete.
    DeleteFileA(join(dir, "watched_new.bin").c_str());
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool gone = false;
    while (std::chrono::steady_clock::now() < deadline) {
        page = cache2.list(fs, dir, {}, 500000);
        gone = true;
        for (const auto& e : page.entries) {
            if (e.name == "watched_new.bin") { gone = false; break; }
        }
        if (gone) break;
        Sleep(50);
    }
    CHECK(gone);
    std::printf("[ok] watcher picked up delete\n");

    watcher.unwatch(dir);
    CHECK(watcher.watchedCount() == 0);

    // --- 5: eviction by size cap ---
    const std::string dir2 = dir + "_b";
    CreateDirectoryA(dir2.c_str(), nullptr);
    for (int i = 0; i < 600; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "g%04d.txt", i);
        createFile(join(dir2, name));
    }
    ftpflow::LocalFsBackend fs2;
    const bool opened2 = fs2.open(dir2, &err);
    CHECK(opened2);

    ftpflow::DirCache evicting(800);
    evicting.list(fs, dir, {}, 500000);      // 500 entries in
    CHECK(evicting.stats().entriesCached == 500);
    evicting.list(fs2, dir2, {}, 500000);    // 600 more -> must evict dir 1
    auto es = evicting.stats();
    CHECK(es.entriesCached == 600);          // 600 <= cap, old node evicted
    CHECK(es.evictions == 1);
    std::printf("[ok] eviction: %zu entries cached, %llu evictions\n",
                es.entriesCached, (unsigned long long)es.evictions);

    // Cleanup (everything we created).
    for (const std::string& name : viaBackend)
        DeleteFileA(join(dir, name).c_str());
    DeleteFileA(victim.c_str());
    for (int i = 0; i < 600; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "g%04d.txt", i);
        DeleteFileA(join(dir2, name).c_str());
    }
    RemoveDirectoryA(dir2.c_str());
    RemoveDirectoryA(dir.c_str());

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL DIR-CACHE TESTS PASSED\n");
    return g_failed == 0 ? 0 : 1;
}
