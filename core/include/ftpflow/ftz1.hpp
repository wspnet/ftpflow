// ftpflow -- FTZ1 container v1: transparent resumable compressed transfer.
// Part of core.lib: pure C++17, no Qt.
//
// File layout:
//   [Header  32 bytes]  see kFtzMagic below
//   [Frames  ...]       independent zstd frames (default 8 MiB each);
//                       a frame whose zstd output would expand the input is
//                       stored raw instead -- the container never grows.
//   [Manifest]          minimal CBOR: original size + whole-file SHA-256 +
//                       per-frame table (type/offset/csize/usize/sha256)
//
// The resume primitive is frame-level verification: verifyFrame() checks a
// frame's SHA-256 (and, optionally, its decompressed size), so a resumed
// transfer can skip intact frames and re-request only the damaged ones.
// Requires FTPFLOW_HAVE_ZSTD; without it every entry point returns
// FtzStatus::EngineUnavailable so callers degrade to plain transfers.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace ftpflow {

enum class FtzStatus : std::uint8_t {
    Ok = 0,
    EngineUnavailable,   // built without zstd
    NotFtz1,             // magic mismatch
    BadVersion,
    BadHeader,
    BadManifest,         // truncated / malformed CBOR
    IoError,
    CorruptFrame,        // hash mismatch or size mismatch
    ShortFile,           // frame range extends past EOF
};

inline const char* ftzStatusName(FtzStatus s) {
    switch (s) {
        case FtzStatus::Ok: return "ok";
        case FtzStatus::EngineUnavailable: return "engine-unavailable";
        case FtzStatus::NotFtz1: return "not-ftz1";
        case FtzStatus::BadVersion: return "bad-version";
        case FtzStatus::BadHeader: return "bad-header";
        case FtzStatus::BadManifest: return "bad-manifest";
        case FtzStatus::IoError: return "io-error";
        case FtzStatus::CorruptFrame: return "corrupt-frame";
        case FtzStatus::ShortFile: return "short-file";
    }
    return "?";
}

struct FtzFrameEntry {
    std::uint64_t offset = 0;           // from container start
    std::uint64_t storedSize = 0;       // bytes in the container
    std::uint64_t plainSize = 0;        // bytes of original data
    bool raw = false;                   // true = stored, false = zstd
    std::uint8_t sha256[32] = {};       // of the stored payload
};

struct FtzInfo {
    std::uint64_t originalSize = 0;
    std::uint8_t originalSha256[32] = {};
    std::uint32_t frameCount = 0;
    std::uint32_t manifestOffset = 0;
    std::vector<FtzFrameEntry> frames;
};

// One-shot container writer (prototype: whole file in memory-mapped style,
// streamed frame by frame). frameMiB is clamped to [4, 16] per the
// blueprint's independent-frame sizing.
FtzStatus ftzWrite(const std::string& srcPath, const std::string& dstPath,
                   int frameMiB = 8, int zstdLevel = 1,
                   FtzInfo* infoOut = nullptr);

// Parse header + manifest only. Cheap; the resume path calls this first.
FtzStatus ftzOpen(const std::string& path, FtzInfo& info);

// Frame-level verification: reads the stored payload, checks SHA-256.
// With decompress=true it also decompresses and checks plainSize.
FtzStatus ftzVerifyFrame(const std::string& path, const FtzInfo& info,
                         std::uint32_t frameIndex, bool decompress = false);

// Extract the whole container, verifying frames as they are read.
// Fills badFrames (when non-null) and continues past corrupt frames when
// requested (best-effort recovery of what survived).
FtzStatus ftzExtract(const std::string& ftzPath, const std::string& dstPath,
                     std::vector<std::uint32_t>* badFrames = nullptr);

// Stream out a single frame's plain payload into `out` (append).
// Used by the resume/retry transfer path.
FtzStatus ftzReadFrame(const std::string& path, const FtzInfo& info,
                       std::uint32_t frameIndex, std::vector<std::uint8_t>& out);

}  // namespace ftpflow
