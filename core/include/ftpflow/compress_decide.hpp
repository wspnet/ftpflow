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

}  // namespace ftpflow
