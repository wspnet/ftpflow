// ftpflow -- transfer pipeline public types.
// Part of core.lib: pure C++17, no Qt.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "ftpflow/vfs.hpp"

namespace ftpflow {

enum class TransferDirection : std::uint8_t { Upload, Download };

enum class TransferMode : std::uint8_t {
    Store,      // raw byte pipeline (FTP always; SSH fallback)
    Compress,   // SSH exec single-stream zstd pipeline (the fast path)
    Ftz1,       // ship an FTZ1 container (.ftz1): frame-verifiable resume
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

// Global transfer-rate limiter (FileZilla "速度限制"). Sleeps between chunks
// so effective throughput converges to `bytesPerSecLimit` (0 = unlimited).
// The mutable state lives here (not in TransferControl) so a const
// TransferControl* — what every backend receives — can still throttle.
// `bytesPerSecLimit` is atomic because the UI thread updates it while the
// transfer thread reads it; the window fields are per-job (one thread).
struct RateLimiter {
    std::atomic<std::uint64_t> bytesPerSecLimit{0};   // 0 = no limit
    std::uint64_t windowStartUs = 0;      // monotonic µs the window opened
    std::uint64_t windowBytes = 0;        // bytes credited this window
};

// Shared mid-transfer control block. The queue (or any caller) flips these
// from another thread; every backend polls between chunks:
//   pause  -> the transfer parks (no data moves, connection stays alive)
//   cancel -> aborts as soon as practical; the result reports Canceled
//   rate   -> per-job throttle state (see throttle() below)
// The flags are std::atomic (NOT volatile): they are written by the UI /
// queue thread and polled by the transfer thread, which requires proper
// atomic synchronization, not just volatile.
struct TransferControl {
    std::atomic<bool> pause{false};
    std::atomic<bool> cancel{false};
    mutable RateLimiter rate;
};

// Block for as long as needed so that `bytes` more bytes fit within the
// configured rate since the window opened. No-op when the limit is 0. `ctl`
// may be null (treated as unlimited). Safe to call from any single transfer
// thread (one job = one thread, so no cross-thread contention on `rate`).
void throttle(const TransferControl* ctl, std::uint64_t bytes);

}  // namespace ftpflow
