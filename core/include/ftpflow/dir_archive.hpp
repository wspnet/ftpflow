// ftpflow -- directory archive: pack a whole folder tree into one
// container and extract it back, for "transfer a folder as a single blob"
// workflows.
// Part of core.lib: pure C++17 + optional zstd (FTPFLOW_HAVE_ZSTD), no Qt.
//
// Container format: a standard POSIX ustar tar stream (so it interoperates
// with system `tar`), optionally compressed with zstd as a single frame.
// The ".ftz1" extension is NOT used here (that is the single-file FTZ1
// container); a directory archive uses ".tar.zst" or, when the remote has
// no zstd executable, plain ".tar". Both the pack and extract sides are
// implemented locally (no external tools), so a download can always be
// unpacked on the client regardless of what the remote end has installed.
#pragma once

#include <cstdint>
#include <string>

namespace ftpflow {

enum class DirArchiveStatus : std::uint8_t {
    Ok = 0,
    EngineUnavailable,   // built without zstd
    IoError,             // source missing / unreadable / dest unwritable
    BadArchive,          // corrupt tar / zstd stream
    UnsafePath,          // tar entry escapes the destination (../ or abs)
};

inline const char* dirArchiveStatusName(DirArchiveStatus s) {
    switch (s) {
        case DirArchiveStatus::Ok: return "ok";
        case DirArchiveStatus::EngineUnavailable: return "engine-unavailable";
        case DirArchiveStatus::IoError: return "io-error";
        case DirArchiveStatus::BadArchive: return "bad-archive";
        case DirArchiveStatus::UnsafePath: return "unsafe-path";
    }
    return "?";
}

// Packs the local directory `srcDir` (recursively) into a tar at `dstPath`.
// File names are stored as UTF-8. With compress=true (the default) the tar
// is zstd-compressed into `dstPath`; with compress=false a plain ustar is
// written. Plain tar works even in builds without zstd, which is the
// fallback for remotes that have no `zstd` executable for `tar --zstd`.
DirArchiveStatus archiveDirectory(const std::string& srcDir,
                                  const std::string& dstPath,
                                  int zstdLevel = 3,
                                  bool compress = true);

// Extracts `srcPath` (a zstd-compressed tar) into `dstDir`, recreating the
// directory tree. Refuses entries that would escape dstDir (path traversal)
// and returns UnsafePath rather than writing outside.
DirArchiveStatus extractArchive(const std::string& srcPath,
                                const std::string& dstDir);

// Extracts a plain (uncompressed) ustar into `dstDir`. Same path-safety
// rules as extractArchive(); used when the remote could only run plain tar.
DirArchiveStatus extractArchiveRaw(const std::string& srcPath,
                                   const std::string& dstDir);

}  // namespace ftpflow
