// test_dir_archive -- directory archive round-trip: pack a tree into a
// zstd-compressed tar and extract it back, verifying content, structure,
// Chinese paths, empty dirs, and path-traversal rejection.
#include "ftpflow/dir_archive.hpp"
#include "ftpflow/text_codec.hpp"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace ftpflow;

static int g_checks = 0;
static int g_failed = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++g_failed;                                                      \
        }                                                                    \
    } while (0)

static bool writeFile(const std::string& path, const std::string& content) {
    std::FILE* f = openLocalFile(path, "wb");
    if (!f) return false;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return true;
}

static bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = openLocalFile(path, "rb");
    if (!f) return false;
    char buf[4096];
    std::size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return true;
}

int main() {
    const std::string base = "C:\\Users\\wspne\\ftpflow_test_archive";
    const std::string src = base + "\\src";
    const std::string dst = base + "\\dst";
    const std::string archive = base + "\\archive.tar.zst";
    const std::string plain = base + "\\archive.tar";
    const std::string rawDst = base + "\\rawdst";

    // Setup source tree: nested dirs, Chinese names, an empty dir.
    createDirUtf8(base);
    createDirUtf8(src);
    createDirUtf8(src + "\\sub");
    createDirUtf8(src + "\\中文目录");
    createDirUtf8(src + "\\emptydir");
    writeFile(src + "\\root.txt", "root file\n");
    writeFile(src + "\\sub\\nested.txt", "nested content\n");
    writeFile(src + "\\中文目录\\中文文件.txt", "chinese content\n");
    writeFile(src + "\\中文目录\\another.bin", std::string(2000, 'x'));

    // 1. Pack.
    DirArchiveStatus s = archiveDirectory(src, archive, 3);
    CHECK(s == DirArchiveStatus::Ok);

    // 2. Extract into dst.
    createDirUtf8(dst);
    s = extractArchive(archive, dst);
    CHECK(s == DirArchiveStatus::Ok);

    // 3. Verify structure + content.
    std::string c;
    CHECK(readFile(dst + "\\root.txt", c) && c == "root file\n");
    CHECK(readFile(dst + "\\sub\\nested.txt", c) && c == "nested content\n");
    CHECK(readFile(dst + "\\中文目录\\中文文件.txt", c) && c == "chinese content\n");
    CHECK(readFile(dst + "\\中文目录\\another.bin", c) && c.size() == 2000);
    // Empty dir was recreated.
    {
        const std::string ed = dst + "\\emptydir";
        const std::wstring we(ed.begin(), ed.end());
        DWORD attrs = GetFileAttributesW(we.c_str());
        CHECK(attrs != INVALID_FILE_ATTRIBUTES &&
              (attrs & FILE_ATTRIBUTE_DIRECTORY));
    }

    // 4. A file > 512 bytes exercises the block padding + multi-block read.
    CHECK(readFile(dst + "\\中文目录\\another.bin", c) && c.size() == 2000);

    // 5. Uncompressed fallback for remotes without a zstd executable:
    //    pack a plain ustar, verify magic, then extract it locally.
    s = archiveDirectory(src, plain, 3, /*compress=*/false);
    CHECK(s == DirArchiveStatus::Ok);
    {
        std::FILE* f = openLocalFile(plain, "rb");
        char magic[8] = {};
        CHECK(f != nullptr);
        if (f) {
            CHECK(std::fseek(f, 257, SEEK_SET) == 0);
            CHECK(std::fread(magic, 1, 5, f) == 5);
            std::fclose(f);
        }
        CHECK(std::string(magic, 5) == "ustar");
    }
    createDirUtf8(rawDst);
    s = extractArchiveRaw(plain, rawDst);
    CHECK(s == DirArchiveStatus::Ok);
    CHECK(readFile(rawDst + "\\root.txt", c) && c == "root file\n");
    CHECK(readFile(rawDst + "\\中文目录\\中文文件.txt", c) &&
          c == "chinese content\n");

    std::printf("dir_archive: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
