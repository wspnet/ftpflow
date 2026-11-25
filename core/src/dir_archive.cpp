// ftpflow -- directory archive implementation (ustar tar + optional zstd).
// Part of core.lib: pure C++17 + zstd (FTPFLOW_HAVE_ZSTD), no Qt.
#include "ftpflow/dir_archive.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "ftpflow/text_codec.hpp"
#include "ftpflow/tree_walk.hpp"

#if FTPFLOW_HAVE_ZSTD
#include <zstd.h>
#endif

namespace ftpflow {

namespace {

constexpr std::size_t kTarBlock = 512;

// ---- ustar tar header ------------------------------------------------------

// Writes one 512-byte tar header. `name` is the UTF-8 path relative to the
// archive root (forward slashes). `size` applies to regular files.
void writeTarHeader(std::FILE* f, const char* name, char typeflag,
                    std::uint64_t size, std::uint64_t mtime) {
    std::uint8_t h[kTarBlock] = {};
    // name[0..99]
    std::size_t nlen = std::strlen(name);
    if (nlen > 100) nlen = 100;
    std::memcpy(h, name, nlen);
    // mode[100..107] = "0000644" / "0000755"
    const char* mode = (typeflag == '5') ? "0000755" : "0000644";
    std::memcpy(h + 100, mode, 8);
    // uid[108..115], gid[116..123]
    std::memcpy(h + 108, "0000000", 8);
    std::memcpy(h + 116, "0000000", 8);
    // size[124..135] octal
    char sz[16];
    std::snprintf(sz, sizeof(sz), "%011llo", (unsigned long long)size);
    std::memcpy(h + 124, sz, 11);
    // mtime[136..147] octal
    char mt[16];
    std::snprintf(mt, sizeof(mt), "%011llo", (unsigned long long)mtime);
    std::memcpy(h + 136, mt, 11);
    // typeflag[156]
    h[156] = static_cast<std::uint8_t>(typeflag);
    // magic[257..262] = "ustar", version[263..264] = "00"
    std::memcpy(h + 257, "ustar", 5);
    std::memcpy(h + 263, "00", 2);
    // checksum[148..155]: spaces first, then sum of the block as bytes
    // interpreted as space-padded ASCII (classic tar checksum).
    std::memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (std::size_t i = 0; i < kTarBlock; ++i) sum += h[i];
    char ck[16];
    std::snprintf(ck, sizeof(ck), "%06o", sum);
    ck[6] = '\0';
    std::memcpy(h + 148, ck, 6);
    h[154] = ' ';
    h[155] = ' ';
    std::fwrite(h, 1, kTarBlock, f);
}

// Pads the stream out to the next 512-byte boundary with NULs.
void padTar(std::FILE* f, std::uint64_t written) {
    const std::uint64_t rem = written % kTarBlock;
    if (rem == 0) return;
    std::uint8_t zeros[kTarBlock] = {};
    std::fwrite(zeros, 1, kTarBlock - rem, f);
}

// ---- path safety (extract side) -------------------------------------------

// Rejects absolute paths, drive letters, and any ".." component so an
// archive cannot write outside the destination directory ("zip slip").
// BOTH separators split components: Windows treats '\' as a separator, and
// the old '/'-only split let `..\..\evil` sail through. Trailing spaces/tabs
// are stripped before the ".." test because Win32 path normalization strips
// them too (`.. ` resolves to the parent just like `..`).
bool safeRelPath(const std::string& name) {
    if (name.empty()) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (name.size() >= 2 && name[1] == ':') return false;   // "C:..."
    std::size_t i = 0;
    while (i < name.size()) {
        std::size_t j = name.find_first_of("/\\", i);
        std::string comp = name.substr(i, j == std::string::npos
                                             ? std::string::npos : j - i);
        while (!comp.empty() && (comp.back() == ' ' || comp.back() == '\t'))
            comp.pop_back();
        if (comp == "..") return false;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return true;
}

// Writes a plain ustar stream of `srcDir` into `tarPath`. This is shared by
// the compressed and uncompressed pack paths (a remote without `zstd` can
// still extract a plain tar with any POSIX tar).
DirArchiveStatus writeTarStream(const std::string& srcDir,
                                const std::string& tarPath) {
    std::vector<TreeEntry> entries;
    std::string walkErr;
    if (!walkLocalTree(srcDir, entries, &walkErr))
        return DirArchiveStatus::IoError;

    std::FILE* tar = openLocalFile(tarPath, "wb");
    if (!tar) return DirArchiveStatus::IoError;

    const std::uint64_t mtime = 0;   // deterministic archive
    for (const TreeEntry& e : entries) {
        if (!safeRelPath(e.relPath)) {
            std::fclose(tar);
            std::remove(tarPath.c_str());
            return DirArchiveStatus::UnsafePath;
        }
        if (e.isDir) {
            // Directory entry: name must end with '/' for clean extraction.
            std::string dn = e.relPath + "/";
            writeTarHeader(tar, dn.c_str(), '5', 0, mtime);
        } else {
            writeTarHeader(tar, e.relPath.c_str(), '0', e.size, mtime);
            const std::string full = joinTreePath(srcDir, e.relPath);
            std::FILE* f = openLocalFile(full, "rb");
            if (!f) {
                std::fclose(tar);
                std::remove(tarPath.c_str());
                return DirArchiveStatus::IoError;
            }
            std::vector<std::uint8_t> buf(256 * 1024);
            std::size_t n;
            while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
                std::fwrite(buf.data(), 1, n, tar);
            std::fclose(f);
            padTar(tar, e.size);
        }
    }
    // Two zero blocks terminate the tar.
    std::uint8_t zeros[kTarBlock * 2] = {};
    std::fwrite(zeros, 1, sizeof(zeros), tar);
    std::fclose(tar);
    return DirArchiveStatus::Ok;
}

// Reads a plain ustar stream from `tarPath` into `dstDir`.
DirArchiveStatus extractTarStream(const std::string& tarPath,
                                  const std::string& dstDir) {
    std::FILE* tar = openLocalFile(tarPath, "rb");
    if (!tar) return DirArchiveStatus::IoError;

    DirArchiveStatus result = DirArchiveStatus::Ok;
    std::vector<std::uint8_t> block(kTarBlock);
    for (;;) {
        const std::size_t got = std::fread(block.data(), 1, kTarBlock, tar);
        if (got < kTarBlock) {
            result = DirArchiveStatus::BadArchive;
            break;
        }
        // All-zero block => end of archive (possibly one of two).
        bool allZero = true;
        for (std::size_t i = 0; i < kTarBlock; ++i)
            if (block[i] != 0) { allZero = false; break; }
        if (allZero) break;

        // name[0..99]
        std::string name(reinterpret_cast<char*>(block.data()), 100);
        name = name.substr(0, name.find('\0'));
        // size[124..135] octal
        std::uint64_t size = 0;
        for (int i = 0; i < 12; ++i) {
            char c = static_cast<char>(block[124 + i]);
            if (c == 0 || c == ' ') continue;
            if (c < '0' || c > '7') { result = DirArchiveStatus::BadArchive; break; }
            size = (size << 3) | static_cast<std::uint64_t>(c - '0');
        }
        if (result != DirArchiveStatus::Ok) break;
        const char typeflag = static_cast<char>(block[156]);

        if (!safeRelPath(name)) {
            result = DirArchiveStatus::UnsafePath;
            break;
        }
        // Strip trailing '/' for directories.
        while (!name.empty() && name.back() == '/') name.pop_back();

        const std::string full = joinTreePath(dstDir, name);
        if (typeflag == '5') {
            // Directory: create (recursively, best effort).
            // Use Win32 CreateDirectoryW via text_codec's wide helper path.
            const std::size_t last = full.find_last_of("/\\");
            for (std::size_t i = 1; i <= last; ++i) {
                if (full[i] != '/' && full[i] != '\\') continue;
                const std::string seg = full.substr(0, i);
                if (seg.size() < 2 || seg.back() == ':') continue;
                createDirUtf8(seg);
            }
            createDirUtf8(full);
        } else if (typeflag == '0' || typeflag == '\0') {
            // Regular file: ensure parent dir, then write content.
            const std::size_t last = full.find_last_of("/\\");
            for (std::size_t i = 1; i <= last; ++i) {
                if (full[i] != '/' && full[i] != '\\') continue;
                const std::string seg = full.substr(0, i);
                if (seg.size() < 2 || seg.back() == ':') continue;
                createDirUtf8(seg);
            }
            std::FILE* out = openLocalFile(full, "wb");
            if (!out) { result = DirArchiveStatus::IoError; break; }
            std::uint64_t remaining = size;
            std::vector<std::uint8_t> buf(256 * 1024);
            while (remaining > 0) {
                const std::size_t want = remaining < buf.size()
                    ? static_cast<std::size_t>(remaining) : buf.size();
                const std::size_t r = std::fread(buf.data(), 1, want, tar);
                if (r != want) { std::fclose(out); result = DirArchiveStatus::BadArchive; break; }
                std::fwrite(buf.data(), 1, r, out);
                remaining -= r;
            }
            std::fclose(out);
            if (result != DirArchiveStatus::Ok) break;
            // Skip padding to the next block boundary.
            const std::uint64_t rem = size % kTarBlock;
            if (rem != 0)
                _fseeki64(tar, static_cast<__int64>(kTarBlock - rem), SEEK_CUR);
        } else {
            // Skip unknown entry types (symlinks '2', etc.) by skipping
            // their data. 64-bit seek: entries may exceed 2 GB.
            const std::uint64_t skip = size;
            const std::uint64_t rem = skip % kTarBlock;
            if (_fseeki64(tar, static_cast<__int64>(skip + (rem ? kTarBlock - rem : 0)),
                          SEEK_CUR) != 0) {
                result = DirArchiveStatus::BadArchive;
                break;
            }
        }
    }
    std::fclose(tar);
    return result;
}

}  // namespace

#if FTPFLOW_HAVE_ZSTD

// ---- zstd streaming helpers -----------------------------------------------

namespace {

// Compresses `srcPath` -> `dstPath` using a streaming zstd context. Returns
// true on success.
bool zstdCompressFile(const std::string& srcPath, const std::string& dstPath,
                      int level) {
    std::FILE* src = openLocalFile(srcPath, "rb");
    if (!src) return false;
    std::FILE* dst = openLocalFile(dstPath, "wb");
    if (!dst) { std::fclose(src); return false; }

    ZSTD_CStream* cs = ZSTD_createCStream();
    if (!cs) { std::fclose(src); std::fclose(dst); return false; }
    bool ok = ZSTD_isError(ZSTD_initCStream(cs, level)) == 0;

    std::vector<std::uint8_t> inBuf(256 * 1024);
    std::vector<std::uint8_t> outBuf(ZSTD_CStreamOutSize());
    ZSTD_inBuffer in{};
    ZSTD_outBuffer out{};
    bool eof = false;
    while (ok && !eof) {
        in.src = inBuf.data();
        in.size = std::fread(inBuf.data(), 1, inBuf.size(), src);
        in.pos = 0;
        eof = in.size < inBuf.size();   // short read => last chunk
        if (std::ferror(src)) { ok = false; break; }
        const ZSTD_EndDirective mode = eof ? ZSTD_e_end : ZSTD_e_continue;
        std::size_t remaining;
        do {
            out.dst = outBuf.data();
            out.size = outBuf.size();
            out.pos = 0;
            remaining = ZSTD_compressStream2(cs, &out, &in, mode);
            if (ZSTD_isError(remaining)) { ok = false; break; }
            if (out.pos > 0)
                std::fwrite(outBuf.data(), 1, out.pos, dst);
        } while (ok && remaining != 0);
    }
    ZSTD_freeCStream(cs);
    std::fclose(src);
    std::fclose(dst);
    return ok;
}

// Decompresses `srcPath` -> `dstPath` (streaming). Returns true on success.
bool zstdDecompressFile(const std::string& srcPath, const std::string& dstPath) {
    std::FILE* src = openLocalFile(srcPath, "rb");
    if (!src) return false;
    std::FILE* dst = openLocalFile(dstPath, "wb");
    if (!dst) { std::fclose(src); return false; }

    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) { std::fclose(src); std::fclose(dst); return false; }
    bool ok = ZSTD_isError(ZSTD_initDStream(ds)) == 0;

    std::vector<std::uint8_t> inBuf(256 * 1024);
    std::vector<std::uint8_t> outBuf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer in{};
    ZSTD_outBuffer out{};
    while (ok) {
        in.src = inBuf.data();
        in.size = std::fread(inBuf.data(), 1, inBuf.size(), src);
        in.pos = 0;
        if (std::ferror(src)) { ok = false; break; }
        const bool eof = in.size < inBuf.size();
        while (in.pos < in.size || (eof && in.pos == in.size)) {
            out.dst = outBuf.data();
            out.size = outBuf.size();
            out.pos = 0;
            const std::size_t r = ZSTD_decompressStream(ds, &out, &in);
            if (ZSTD_isError(r)) { ok = false; break; }
            if (out.pos > 0)
                std::fwrite(outBuf.data(), 1, out.pos, dst);
            if (eof && in.pos == in.size && r == 0) break;  // done
            if (in.pos == in.size && r > 0 && !eof) break;  // need more input
        }
        if (!ok) break;
        if (eof) break;
    }
    ZSTD_freeDStream(ds);
    std::fclose(src);
    std::fclose(dst);
    return ok;
}

}  // namespace

#endif  // FTPFLOW_HAVE_ZSTD

DirArchiveStatus archiveDirectory(const std::string& srcDir,
                                  const std::string& dstPath, int zstdLevel,
                                  bool compress) {
#if !FTPFLOW_HAVE_ZSTD
    if (compress) return DirArchiveStatus::EngineUnavailable;
#else
    (void)zstdLevel;
#endif
    // Raw tar writes straight to dstPath; compressed mode writes an
    // intermediate .tar.tmp and zstd-compresses it into dstPath.
    const std::string tarTmp = compress ? dstPath + ".tar.tmp" : dstPath;
    const DirArchiveStatus s = writeTarStream(srcDir, tarTmp);
    if (s != DirArchiveStatus::Ok || !compress) return s;

#if FTPFLOW_HAVE_ZSTD
    const bool ok = zstdCompressFile(tarTmp, dstPath, zstdLevel);
    std::remove(tarTmp.c_str());
    if (!ok) std::remove(dstPath.c_str());
    return ok ? DirArchiveStatus::Ok : DirArchiveStatus::IoError;
#else
    return DirArchiveStatus::EngineUnavailable;
#endif
}

DirArchiveStatus extractArchive(const std::string& srcPath,
                                const std::string& dstDir) {
#if FTPFLOW_HAVE_ZSTD
    // Stage 1: decompress to a temp tar.
    const std::string tarTmp = srcPath + ".extract.tmp";
    if (!zstdDecompressFile(srcPath, tarTmp))
        return DirArchiveStatus::BadArchive;
    const DirArchiveStatus s = extractTarStream(tarTmp, dstDir);
    std::remove(tarTmp.c_str());
    return s;
#else
    (void)srcPath;
    (void)dstDir;
    return DirArchiveStatus::EngineUnavailable;
#endif
}

DirArchiveStatus extractArchiveRaw(const std::string& srcPath,
                                   const std::string& dstDir) {
    return extractTarStream(srcPath, dstDir);
}

}  // namespace ftpflow
