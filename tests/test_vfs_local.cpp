// ftpflow tests -- local VFS backend smoke test.
#include <cstdio>
#include <filesystem>
#include <string>

#include "ftpflow/vfs_local.hpp"

using namespace ftpflow;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK2(cond)                                                    \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                 \
        }                                                               \
    } while (0)

int main() {
    namespace fs = std::filesystem;

    // Build a tiny sandbox directory with known contents.
    fs::path tmp = fs::temp_directory_path() / "ftpflow_test_vfs";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    fs::path a = tmp / "a.txt", b = tmp / "b.log", sub = tmp / "sub";
    fs::create_directory(sub, ec);
    { FILE* f = std::fopen(a.string().c_str(), "wb"); std::fputs("hello", f); std::fclose(f); }
    { FILE* f = std::fopen(b.string().c_str(), "wb"); std::fputs("x", f); std::fclose(f); }

    LocalFsBackend backend;
    std::string err;
    if (!backend.open(tmp.u8string(), &err)) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }

    // Open on a bad root must fail cleanly.
    LocalFsBackend bad;
    CHECK2(bad.open("Z:/definitely/not/here", nullptr) == false);

    auto caps = backend.capabilities();
    CHECK2(caps.structuredListing && caps.utf8Paths && caps.resume);
    CHECK2(caps.execChannel == false);

    ListingPage page = backend.list({});
    CHECK2(page.entries.size() == 3);  // a.txt, b.log, sub
    CHECK2(page.hasMore == false);
    bool sawSub = false, sawA = false;
    for (const auto& e : page.entries) {
        if (e.name == "sub") { sawSub = true; CHECK2(e.type == EntryType::Directory); }
        if (e.name == "a.txt") { sawA = true; CHECK2(e.type == EntryType::File); CHECK2(e.size == 5); }
    }
    CHECK2(sawSub && sawA);

    // Pagination: page size 2 -> first page 2 entries + cursor, second page 1.
    ListingPage p1 = backend.list({}, {}, 2);
    CHECK2(p1.entries.size() == 2);
    CHECK2(p1.hasMore);
    ListingPage p2 = backend.list({}, p1.cursor, 2);
    CHECK2(p2.entries.size() == 1);
    CHECK2(!p2.hasMore);

    fs::remove_all(tmp, ec);
    std::printf("vfs_local: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
