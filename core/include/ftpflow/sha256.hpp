// ftpflow -- standalone SHA-256 (FIPS 180-4).
// Part of core.lib: pure C++17, no Qt, no external crypto dependency.
//
// Used for FTZ1 block checksums and host-key blob hashing. Deliberately
// not constant-time: it guards integrity, not secrets.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace ftpflow {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const void* data, std::size_t len);
    // Finalizes and returns the 32-byte digest; the object is reset.
    std::vector<std::uint8_t> final();

    // One-shot helpers.
    static std::vector<std::uint8_t> digest(const void* data, std::size_t len);
    static std::string hex(const void* data, std::size_t len);

private:
    void processBlock(const std::uint8_t* block);

    std::uint32_t h_[8];
    std::uint8_t buffer_[64];
    std::size_t bufferLen_ = 0;
    std::uint64_t totalBytes_ = 0;
};

}  // namespace ftpflow
