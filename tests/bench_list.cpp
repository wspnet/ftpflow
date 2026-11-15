// bench_list -- enumeration performance probe for the local backend.
// Blueprint budget: a 100k-entry directory must list its first page well
// under 300 ms and fully enumerate without pathological blowup.
//
// Usage:
//   bench_list.exe [dir] [--create N]   (default dir = %TEMP%\ftpflow_bench)
//
// --create N writes N empty files into the directory first (one-time setup,
// roughly one minute for 100k on NTFS); the directory is kept for re-runs.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "ftpflow/dir_cache.hpp"
#include "ftpflow/vfs_local.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double msSince(const Clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string defaultDir() {
    char buf[MAX_PATH];
    DWORD n = ::GetEnvironmentVariableA("TEMP", buf, MAX_PATH);
    std::string d = (n > 0 && n < MAX_PATH) ? buf : ".";
    return d + "\\ftpflow_bench";
}

int createFiles(const std::string& dir, int count) {
    ::CreateDirectoryA(dir.c_str(), nullptr);
    const auto t0 = Clock::now();
    int created = 0;
    char path[MAX_PATH];
    for (int i = 0; i < count; ++i) {
        std::snprintf(path, sizeof(path), "%s\\f%08d.bin", dir.c_str(), i);
        HANDLE h = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
            ++created;
        }
    }
    std::printf("created %d/%d files in %.0f ms\n", created, count, msSince(t0));
    return created == count ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = defaultDir();
    int create = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--create") == 0 && i + 1 < argc) {
            create = std::atoi(argv[++i]);
        } else {
            dir = argv[i];
        }
    }
    if (create > 0 && createFiles(dir, create) != 0) return 1;

    ftpflow::LocalFsBackend fs;
    std::string err;
    if (!fs.open(dir, &err)) {
        std::fprintf(stderr, "open failed: %s (dir=%s)\n", err.c_str(), dir.c_str());
        return 1;
    }

    // Cold run: page through everything (first page timing is the UI budget).
    // NOTE: the current backend re-enumerates the directory on every page
    // call; the bench reports both first-page and full-pagination costs so
    // the directory-cache work item (M1) has a baseline number.
    const auto t0 = Clock::now();
    ftpflow::ListingPage page = fs.list("", "", 500);
    const double firstPageMs = msSince(t0);
    std::size_t total = page.entries.size();
    std::size_t pages = 1;
    std::string cursor = page.cursor;
    while (page.hasMore) {
        page = fs.list("", cursor, 500);
        cursor = page.cursor;
        total += page.entries.size();
        ++pages;
    }
    const double fullMs = msSince(t0);

    // Warm re-run.
    const auto t1 = Clock::now();
    ftpflow::ListingPage warm = fs.list("", "", 500);
    const double warmFirstPageMs = msSince(t1);

    std::printf("dir            : %s\n", dir.c_str());
    std::printf("entries        : %zu (%zu pages x 500)\n", total, pages);
    std::printf("first page     : %.1f ms cold, %.1f ms warm  (budget 300 ms)\n",
                firstPageMs, warmFirstPageMs);
    std::printf("full enumerate : %.1f ms\n", fullMs);

    // --- DirCache: the fix for the repeated-enumeration paging cost. ---
    ftpflow::DirCache cache;
    bool fromCache = true;
    const auto t2 = Clock::now();
    ftpflow::ListingPage cp = cache.list(fs, dir, {}, 500, &fromCache);
    const double cacheFirstMs = msSince(t2);         // miss: enumerate once
    const bool firstWasCached = fromCache;
    std::size_t cachedTotal = cp.entries.size();
    const auto t3 = Clock::now();
    while (cp.hasMore) {                              // every page: memory slice
        cp = cache.list(fs, dir, cp.cursor, 500, &fromCache);
        cachedTotal += cp.entries.size();
    }
    const double cachePagedMs = msSince(t3);
    auto cs = cache.stats();
    std::printf("cache          : first %.1f ms (cached=%d), paged walk %zu entries in %.1f ms (%d hits)\n",
                cacheFirstMs, (int)firstWasCached, cachedTotal, cachePagedMs,
                (int)cs.hits);

    std::printf("verdict        : %s\n",
                warmFirstPageMs < 300.0 && cachePagedMs < 50.0
                    ? "PASS" : "OVER BUDGET");
    return (warmFirstPageMs < 300.0 && cachePagedMs < 50.0) ? 0 : 1;
}
