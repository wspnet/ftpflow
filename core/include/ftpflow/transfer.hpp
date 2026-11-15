// ftpflow -- transfer pipeline public types.
// Part of core.lib: pure C++17, no Qt.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "ftpflow/vfs.hpp"

namespace ftpflow {

enum class TransferDirection : std::uint8_t { Upload, Download };

enum class TransferMode : std::uint8_t {
    Store,      // raw byte pipeline (FTP always; SSH fallback)
    Compress,   // SSH exec single-stream zstd pipeline (the fast path)
};

// Live progress for one transfer task. Pushed at throttle intervals
// (blueprint: UI re-render budget 16 ms), never per-chunk.
struct TransferProgress {
    std::uint64_t bytesMoved = 0;
    std::uint64_t bytesTotal = 0;
    std::uint64_t bytesOnWire = 0;        // after compression, if any
    double speedBytesPerSec = 0.0;
    double compressionRatio = 1.0;        // 1.0 = stored uncompressed
};

using ProgressCallback = std::function<void(const TransferProgress&)>;

// Shared mid-transfer control block. The queue (or any caller) flips these
// from another thread; every backend polls between chunks:
//   pause  -> the transfer parks (no data moves, connection stays alive)
//   cancel -> aborts as soon as practical; the result reports Canceled
struct TransferControl {
    volatile bool pause = false;
    volatile bool cancel = false;
};

}  // namespace ftpflow
