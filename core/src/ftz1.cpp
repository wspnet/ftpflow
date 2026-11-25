// ftpflow -- FTZ1 container v1 implementation.
// Part of core.lib: pure C++17 + zstd (FTPFLOW_HAVE_ZSTD).
#include "ftpflow/ftz1.hpp"

#include <cstdio>
#include <cstring>

#include "ftpflow/sha256.hpp"
#include "ftpflow/text_codec.hpp"

#if FTPFLOW_HAVE_ZSTD
#include <zstd.h>
#endif

namespace ftpflow {

#if FTPFLOW_HAVE_ZSTD

namespace {

constexpr char kFtzMagic[4] = {'F', 'T', 'Z', '1'};
constexpr std::uint32_t kHeaderSize = 32;
constexpr std::uint32_t kVersion = 1;

// ---- minimal CBOR (subset: uint, bstr, array, map) ------------------------

struct CborWriter {
    std::vector<std::uint8_t> buf;

    void typeByte(std::uint8_t major, std::uint64_t val) {
        if (val < 24) {
            buf.push_back(static_cast<std::uint8_t>((major << 5) | val));
        } else if (val <= 0xFF) {
            buf.push_back(static_cast<std::uint8_t>((major << 5) | 24));
            buf.push_back(static_cast<std::uint8_t>(val));
        } else if (val <= 0xFFFF) {
            buf.push_back(static_cast<std::uint8_t>((major << 5) | 25));
            buf.push_back(static_cast<std::uint8_t>(val >> 8));
            buf.push_back(static_cast<std::uint8_t>(val));
        } else if (val <= 0xFFFFFFFF) {
            buf.push_back(static_cast<std::uint8_t>((major << 5) | 26));
            for (int i = 3; i >= 0; --i)
                buf.push_back(static_cast<std::uint8_t>(val >> (8 * i)));
        } else {
            buf.push_back(static_cast<std::uint8_t>((major << 5) | 27));
            for (int i = 7; i >= 0; --i)
                buf.push_back(static_cast<std::uint8_t>(val >> (8 * i)));
        }
    }
    void uint(std::uint64_t v) { typeByte(0, v); }
    void bstr(const void* p, std::size_t n) {
        typeByte(2, n);
        const auto* b = static_cast<const std::uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    void array(std::size_t n) { typeByte(4, n); }
    void map(std::size_t n) { typeByte(5, n); }
};

struct CborReader {
    const std::uint8_t* p;
    const std::uint8_t* end;
    bool bad = false;

    std::uint8_t major() {
        if (p >= end) { bad = true; return 0; }
        return static_cast<std::uint8_t>(*p >> 5);
    }
    std::uint64_t value() {
        if (p >= end) { bad = true; return 0; }
        std::uint8_t b = *p++;
        std::uint8_t info = b & 0x1F;
        if (info < 24) return info;
        int n = 0;
        if (info == 24) n = 1;
        else if (info == 25) n = 2;
        else if (info == 26) n = 4;
        else if (info == 27) n = 8;
        else { bad = true; return 0; }
        if (end - p < n) { bad = true; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | *p++;
        return v;
    }
    std::uint64_t uint() {
        if (major() != 0) { bad = true; return 0; }
        return value();
    }
    void bstr(std::uint8_t* dst, std::size_t want) {
        if (major() != 2) { bad = true; return; }
        std::uint64_t n = value();
        if (bad || n != want || static_cast<std::uint64_t>(end - p) < n) {
            bad = true; return;
        }
        std::memcpy(dst, p, n);
        p += n;
    }
    std::uint64_t array() {
        if (major() != 4) { bad = true; return 0; }
        return value();
    }
    std::uint64_t map() {
        if (major() != 5) { bad = true; return 0; }
        return value();
    }
};

std::vector<std::uint8_t> encodeManifest(const FtzInfo& info) {
    CborWriter w;
    w.map(3);
    w.uint(1); w.uint(info.originalSize);
    w.uint(2); w.bstr(info.originalSha256, 32);
    w.uint(3);
    w.array(info.frames.size());
    for (const FtzFrameEntry& f : info.frames) {
        w.map(5);
        w.uint(0); w.uint(f.raw ? 1 : 0);
        w.uint(1); w.uint(f.offset);
        w.uint(2); w.uint(f.storedSize);
        w.uint(3); w.uint(f.plainSize);
        w.uint(4); w.bstr(f.sha256, 32);
    }
    return w.buf;
}

bool decodeManifest(const std::uint8_t* p, std::size_t n, FtzInfo& info) {
    CborReader r{p, p + n, false};
    std::uint64_t pairs = r.map();
    if (r.bad || pairs != 3) return false;
    if (r.uint() != 1) return false;
    info.originalSize = r.uint();
    if (r.uint() != 2) return false;
    r.bstr(info.originalSha256, 32);
    if (r.bad) return false;
    if (r.uint() != 3) return false;
    std::uint64_t count = r.array();
    if (r.bad || count > 4096) return false;
    info.frameCount = static_cast<std::uint32_t>(count);
    info.frames.resize(static_cast<std::size_t>(count));
    for (FtzFrameEntry& f : info.frames) {
        std::uint64_t fields = r.map();
        if (r.bad || fields != 5) return false;
        if (r.uint() != 0) return false;
        f.raw = r.uint() != 0;
        if (r.uint() != 1) return false;
        f.offset = r.uint();
        if (r.uint() != 2) return false;
        f.storedSize = r.uint();
        if (r.uint() != 3) return false;
        f.plainSize = r.uint();
        if (r.uint() != 4) return false;
        r.bstr(f.sha256, 32);
        if (r.bad) return false;
    }
    return true;
}

// ---- raw file helpers ------------------------------------------------------

bool readExact(std::FILE* f, void* dst, std::size_t n) {
    return std::fread(dst, 1, n, f) == n;
}

bool writeAll(std::FILE* f, const void* src, std::size_t n) {
    return std::fwrite(src, 1, n, f) == n;
}

bool readAt(const std::string& path, std::uint64_t off, void* dst,
            std::size_t n) {
    std::FILE* f = openLocalFile(path, "rb");
    if (!f) return false;
    bool ok = _fseeki64(f, static_cast<__int64>(off), SEEK_SET) == 0 &&
              readExact(f, dst, n);
    std::fclose(f);
    return ok;
}

}  // namespace

// ---- writer ----------------------------------------------------------------

FtzStatus ftzWrite(const std::string& srcPath, const std::string& dstPath,
                   int frameMiB, int zstdLevel, FtzInfo* infoOut) {
    if (frameMiB < 4) frameMiB = 4;
    if (frameMiB > 16) frameMiB = 16;
    const std::uint64_t frameBytes =
        static_cast<std::uint64_t>(frameMiB) * 1024 * 1024;

    std::FILE* src = openLocalFile(srcPath, "rb");
    if (!src) return FtzStatus::IoError;
    // 64-bit positions: 32-bit ftell wraps at 2 GB and would corrupt both
    // the frame walk and the resume offsets.
    _fseeki64(src, 0, SEEK_END);
    const __int64 len = _ftelli64(src);
    _fseeki64(src, 0, SEEK_SET);
    if (len < 0) { std::fclose(src); return FtzStatus::IoError; }
    const std::uint64_t total = static_cast<std::uint64_t>(len);

    std::FILE* dst = openLocalFile(dstPath, "wb");
    if (!dst) { std::fclose(src); return FtzStatus::IoError; }

    FtzInfo info;
    info.originalSize = total;
    Sha256 wholeFile;

    // Placeholder header; patched after the manifest position is known.
    std::uint8_t header[kHeaderSize] = {};
    std::memcpy(header, kFtzMagic, 4);
    header[4] = static_cast<std::uint8_t>(kVersion);
    header[5] = 0;  // flags
    header[6] = header[7] = 0;
    if (!writeAll(dst, header, sizeof(header))) {
        std::fclose(src); std::fclose(dst); return FtzStatus::IoError;
    }

    std::vector<std::uint8_t> inBuf(static_cast<std::size_t>(frameBytes));
    std::vector<std::uint8_t> zBuf(ZSTD_compressBound(
        static_cast<std::size_t>(frameBytes)));

    std::uint64_t done = 0;
    while (done < total) {
        const std::uint64_t left = total - done;
        const std::size_t chunk = static_cast<std::size_t>(
            left < frameBytes ? left : frameBytes);
        if (!readExact(src, inBuf.data(), chunk)) {
            std::fclose(src); std::fclose(dst); return FtzStatus::IoError;
        }
        wholeFile.update(inBuf.data(), chunk);

        // Compress the frame; fall back to raw when it would expand.
        const std::size_t produced = ZSTD_compress(
            zBuf.data(), zBuf.size(), inBuf.data(), chunk, zstdLevel);
        const bool useRaw = ZSTD_isError(produced) || produced >= chunk;

        FtzFrameEntry entry;
        entry.offset = static_cast<std::uint64_t>(_ftelli64(dst));
        entry.plainSize = chunk;
        entry.raw = useRaw;
        const void* payload = nullptr;
        std::size_t payloadSize = 0;
        if (useRaw) {
            payload = inBuf.data();
            payloadSize = chunk;
        } else {
            payload = zBuf.data();
            payloadSize = produced;
        }
        entry.storedSize = payloadSize;
        {
            auto d = Sha256::digest(payload, payloadSize);
            std::memcpy(entry.sha256, d.data(), 32);
        }

        if (!writeAll(dst, payload, payloadSize)) {
            std::fclose(src); std::fclose(dst); return FtzStatus::IoError;
        }
        info.frames.push_back(entry);
        done += chunk;
    }
    std::fclose(src);

    auto digest = wholeFile.final();
    std::memcpy(info.originalSha256, digest.data(), 32);
    info.frameCount = static_cast<std::uint32_t>(info.frames.size());

    // Manifest at the tail, then patch the header. The on-disk manifest
    // offset is u32 (format v1), so a container whose tail lands past 4 GiB
    // cannot be represented -- fail instead of silently truncating it.
    std::vector<std::uint8_t> manifest = encodeManifest(info);
    const __int64 manifestPos = _ftelli64(dst);
    if (manifestPos < 0 || manifestPos > 0xFFFFFFFFll) {
        std::fclose(dst); return FtzStatus::IoError;
    }
    info.manifestOffset = static_cast<std::uint32_t>(manifestPos);
    if (!writeAll(dst, manifest.data(), manifest.size())) {
        std::fclose(dst); return FtzStatus::IoError;
    }

    std::memcpy(header, kFtzMagic, 4);
    header[4] = static_cast<std::uint8_t>(kVersion);
    // u32 frameCount @8, u32 manifestLen @12, u32 manifestOffset @16,
    // u64 originalSize @24.
    auto put32 = [&](std::uint32_t off, std::uint32_t v) {
        header[off] = static_cast<std::uint8_t>(v);
        header[off + 1] = static_cast<std::uint8_t>(v >> 8);
        header[off + 2] = static_cast<std::uint8_t>(v >> 16);
        header[off + 3] = static_cast<std::uint8_t>(v >> 24);
    };
    put32(8, info.frameCount);
    put32(12, static_cast<std::uint32_t>(manifest.size()));
    put32(16, info.manifestOffset);
    for (int i = 0; i < 8; ++i)
        header[24 + i] = static_cast<std::uint8_t>(total >> (8 * i));
    if (_fseeki64(dst, 0, SEEK_SET) != 0 ||
        !writeAll(dst, header, sizeof(header))) {
        std::fclose(dst); return FtzStatus::IoError;
    }
    std::fclose(dst);

    if (infoOut) *infoOut = info;
    return FtzStatus::Ok;
}

// ---- reader ----------------------------------------------------------------

FtzStatus ftzOpen(const std::string& path, FtzInfo& info) {
    // Magic first: a short or non-container file reports NotFtz1, not a
    // generic I/O error -- the UI wants "this isn't a container".
    std::uint8_t magic[4];
    if (!readAt(path, 0, magic, sizeof(magic)))
        return FtzStatus::IoError;
    if (std::memcmp(magic, kFtzMagic, 4) != 0)
        return FtzStatus::NotFtz1;

    std::uint8_t header[kHeaderSize];
    if (!readAt(path, 0, header, sizeof(header)))
        return FtzStatus::IoError;
    if (header[4] != kVersion)
        return FtzStatus::BadVersion;

    auto get32 = [&](std::uint32_t off) -> std::uint32_t {
        return static_cast<std::uint32_t>(header[off]) |
               (static_cast<std::uint32_t>(header[off + 1]) << 8) |
               (static_cast<std::uint32_t>(header[off + 2]) << 16) |
               (static_cast<std::uint32_t>(header[off + 3]) << 24);
    };
    info.frameCount = get32(8);
    const std::uint32_t manifestLen = get32(12);
    info.manifestOffset = get32(16);
    info.originalSize = 0;
    for (int i = 7; i >= 0; --i)
        info.originalSize = (info.originalSize << 8) | header[24 + i];

    if (info.frameCount > 4096 || manifestLen == 0 || manifestLen > (16 << 20))
        return FtzStatus::BadHeader;

    std::vector<std::uint8_t> manifest(manifestLen);
    if (!readAt(path, info.manifestOffset, manifest.data(), manifestLen))
        return FtzStatus::ShortFile;
    if (!decodeManifest(manifest.data(), manifest.size(), info))
        return FtzStatus::BadManifest;
    return FtzStatus::Ok;
}

FtzStatus ftzVerifyFrame(const std::string& path, const FtzInfo& info,
                         std::uint32_t frameIndex, bool decompress) {
    if (frameIndex >= info.frames.size()) return FtzStatus::BadHeader;
    const FtzFrameEntry& f = info.frames[frameIndex];

    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(f.storedSize));
    if (f.storedSize == 0 || f.storedSize > (512ULL << 20))
        return FtzStatus::BadManifest;
    if (!readAt(path, f.offset, payload.data(),
                static_cast<std::size_t>(f.storedSize)))
        return FtzStatus::ShortFile;

    std::uint8_t sha[32];
    {
        auto d = Sha256::digest(payload.data(), payload.size());
        std::memcpy(sha, d.data(), 32);
    }
    if (std::memcmp(sha, f.sha256, 32) != 0)
        return FtzStatus::CorruptFrame;

    if (decompress && !f.raw) {
        std::vector<std::uint8_t> plain(
            static_cast<std::size_t>(f.plainSize) + 1);
        const std::size_t got = ZSTD_decompress(
            plain.data(), plain.size(), payload.data(), payload.size());
        if (ZSTD_isError(got) || got != f.plainSize)
            return FtzStatus::CorruptFrame;
    }
    return FtzStatus::Ok;
}

FtzStatus ftzReadFrame(const std::string& path, const FtzInfo& info,
                       std::uint32_t frameIndex,
                       std::vector<std::uint8_t>& out) {
    if (frameIndex >= info.frames.size()) return FtzStatus::BadHeader;
    const FtzFrameEntry& f = info.frames[frameIndex];
    if (f.storedSize == 0 || f.storedSize > (512ULL << 20))
        return FtzStatus::BadManifest;

    std::vector<std::uint8_t> payload(
        static_cast<std::size_t>(f.storedSize));
    if (!readAt(path, f.offset, payload.data(),
                static_cast<std::size_t>(f.storedSize)))
        return FtzStatus::ShortFile;

    std::uint8_t sha[32];
    {
        auto d = Sha256::digest(payload.data(), payload.size());
        std::memcpy(sha, d.data(), 32);
    }
    if (std::memcmp(sha, f.sha256, 32) != 0)
        return FtzStatus::CorruptFrame;

    if (f.raw) {
        out.insert(out.end(), payload.begin(), payload.end());
    } else {
        const std::size_t before = out.size();
        out.resize(before + static_cast<std::size_t>(f.plainSize));
        const std::size_t got = ZSTD_decompress(
            out.data() + before, static_cast<std::size_t>(f.plainSize),
            payload.data(), payload.size());
        if (ZSTD_isError(got) || got != f.plainSize)
            return FtzStatus::CorruptFrame;
    }
    return FtzStatus::Ok;
}

FtzStatus ftzExtract(const std::string& ftzPath, const std::string& dstPath,
                     std::vector<std::uint32_t>* badFrames) {
    FtzInfo info;
    FtzStatus st = ftzOpen(ftzPath, info);
    if (st != FtzStatus::Ok) return st;

    std::FILE* dst = openLocalFile(dstPath, "wb");
    if (!dst) return FtzStatus::IoError;

    Sha256 wholeFile;
    std::vector<std::uint8_t> plain;
    for (std::uint32_t i = 0; i < info.frameCount; ++i) {
        plain.clear();
        FtzStatus fs = ftzReadFrame(ftzPath, info, i, plain);
        if (fs != FtzStatus::Ok) {
            if (badFrames) badFrames->push_back(i);
            continue;
        }
        wholeFile.update(plain.data(), plain.size());
        if (!writeAll(dst, plain.data(), plain.size())) {
            std::fclose(dst); return FtzStatus::IoError;
        }
    }
    std::fclose(dst);

    auto digest = wholeFile.final();
    if (std::memcmp(digest.data(), info.originalSha256, 32) != 0)
        return FtzStatus::CorruptFrame;
    return FtzStatus::Ok;
}

#else  // !FTPFLOW_HAVE_ZSTD

FtzStatus ftzWrite(const std::string&, const std::string&, int, int, FtzInfo*) {
    return FtzStatus::EngineUnavailable;
}
FtzStatus ftzOpen(const std::string&, FtzInfo&) {
    return FtzStatus::EngineUnavailable;
}
FtzStatus ftzVerifyFrame(const std::string&, const FtzInfo&, std::uint32_t, bool) {
    return FtzStatus::EngineUnavailable;
}
FtzStatus ftzReadFrame(const std::string&, const FtzInfo&, std::uint32_t,
                       std::vector<std::uint8_t>&) {
    return FtzStatus::EngineUnavailable;
}
FtzStatus ftzExtract(const std::string&, const std::string&,
                     std::vector<std::uint32_t>*) {
    return FtzStatus::EngineUnavailable;
}

#endif

}  // namespace ftpflow
