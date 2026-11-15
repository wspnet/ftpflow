// ftpflow -- smart compressibility decision funnel (five stages).
// Part of core.lib: pure C++17, no Qt.
//
// Stage 1  extension prior        -- fast table lookup
// Stage 2  magic bytes            -- first 32 bytes (defeats renamed files)
// Stage 3  sampled entropy        -- head/middle/tail 3x64 KiB
// Stage 4  trial compression      -- zstd -1 probe (needs FTPFLOW_HAVE_ZSTD)
// Stage 5  bandwidth cost model   -- (1-r)/B > 1/C + r/D
//
// Any stage may short-circuit to Store. Stages 1-3 are implemented here
// today; 4-5 land with the zstd dependency in M0 Spike step 2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ftpflow {

enum class Verdict : std::uint8_t {
    Compress,
    Store,
    Undecided,   // needs a later stage (sampled entropy / trial run)
};

struct Decision {
    Verdict verdict = Verdict::Undecided;
    int stage = 0;          // 1..5, 0 = never evaluated
    std::string reason;     // human-readable, surfaces in UI tooltips
};

// Bytes sampled from the file. Any field may be empty; the funnel uses
// whatever is present. head[0..31] must be the first 32 bytes for stage 2.
struct FileProbe {
    std::string fileName;
    std::uint64_t fileSize = 0;
    std::vector<std::uint8_t> head;    // first 32 bytes (stage 2)
    std::vector<std::uint8_t> sample;  // concatenated head/mid/tail (stage 3)
};

// Stage 1: known compressible / incompressible extensions. Returns
// Undecided for extensions absent from both tables.
Decision decideByExtension(const std::string& fileName);

// Stage 2: container/codec magic bytes. Defeats mislabeled extensions
// (.zip renamed to .txt) and knows that .docx/.jar are ZIP inside.
Decision decideByMagic(const std::uint8_t* head, std::size_t headLen);

// Stage 3: Shannon entropy over the sample. High entropy (> 7.2 bits/byte
// for 64 KiB windows) means the payload is effectively random.
Decision decideByEntropy(const std::uint8_t* data, std::size_t len);

// Run stages 1-3 in order. Short-circuits on the first definite verdict.
Decision decideCompressibility(const FileProbe& probe);

// Shannon entropy in bits per byte, [0, 8].
double shannonEntropyBits(const std::uint8_t* data, std::size_t len);

// ---------------------------------------------------------------------------
// Stage 4: trial compression.
//
// Compresses a sample (bounded, e.g. 256 KiB) with the fastest available
// engine and reports the observed ratio plus the measured throughput.
// Engines, in order of preference: zstd -1 (when FTPFLOW_HAVE_ZSTD), then
// the Windows-native ntdll RtlCompressBuffer XPRESS_HUFF (zero dependency,
// always present on Win8+). The trial both estimates the ratio and measures
// local compression speed for the stage-5 cost model.
// ---------------------------------------------------------------------------

struct TrialResult {
    bool ok = false;               // engine ran at all
    bool engineIsZstd = false;     // false = win32 xpress-huff fallback
    std::uint64_t sampleBytes = 0;
    std::uint64_t compressedBytes = 0;
    double ratio = 1.0;            // compressed/original; >=1 means no gain
    double measuredCompressBps = 0.0;  // bytes/second observed during trial
};

TrialResult trialCompress(const std::uint8_t* data, std::size_t len);

// ---------------------------------------------------------------------------
// Stage 5: bandwidth cost model.
//
// Compress only when the transfer time saved beats the CPU time spent:
//
//     timeSaved = (1 - r) * S / B
//     timeCost  = S / C + r * S / D      (sender compress + remote decompress)
//
//     compress  <=>  timeSaved > safetyMargin * timeCost
//               AND  gain% >= minGainPercent
//               AND  saved bytes >= minSavingBytes (small files go through
//                    the aggregation path instead, never single-file).
// ---------------------------------------------------------------------------

struct CostModel {
    double bandwidthBps = 20.0 * 1024 * 1024;  // 20 MiB/s unknown-link default
    double compressBps = 0.0;      // 0 = auto-measure via the stage-4 trial
    double decompressBps = 0.0;    // 0 = assume 4x compression speed
    double safetyMargin = 1.25;    // cost must be beaten by this factor
    double minGainPercent = 10.0;
    std::uint64_t minSavingBytes = 64 * 1024;
};

// Pure decision math -- fully deterministic, safe to unit-test.
Decision decideByCostModel(std::uint64_t fileSizeBytes, double ratio,
                           double compressBps, const CostModel& cm);

// Full funnel: stages 1-3 as a candidate filter, then stages 4-5 confirm
// that compression actually pays off for THIS link and THIS file. When the
// trial engine is unavailable the stage 1-3 verdict stands.
Decision decideFullFunnel(const FileProbe& probe, const CostModel& cm);

}  // namespace ftpflow
