// ftpflow tests -- FTZ1 container: roundtrip, corruption, resume semantics.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ftpflow/ftz1.hpp"
#include "ftpflow/sha256.hpp"

using namespace ftpflow;

static int g_checks = 0;
static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                 \
        }                                                               \
    } while (0)

static std::string g_tmp;

static std::string writeTemp(const char* name, const std::vector<std::uint8_t>& data) {
    std::string p = g_tmp + "\\" + name;
    std::FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot create %s\n", p.c_str()); std::exit(2); }
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return p;
}

static std::vector<std::uint8_t> makeMixed(std::size_t n) {
    // Half text-ish, half pseudo-random: exercises both zstd and raw frames.
    std::vector<std::uint8_t> v(n);
    const char* pat = "ftpflow-ftz1-roundtrip-payload ";
    const std::size_t patLen = std::strlen(pat);
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (std::size_t i = 0; i < n; ++i) {
        if (i < n / 2) v[i] = static_cast<std::uint8_t>(pat[i % patLen]);
        else {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            v[i] = static_cast<std::uint8_t>(s >> 32);
        }
    }
    return v;
}

static void testRoundtrip() {
    // ~17.3 MB mixed payload -> 3 frames at 8 MiB.
    std::vector<std::uint8_t> data = makeMixed(17 * 1024 * 1024 + 333 * 1000);
    std::string src = writeTemp("ftz_src.bin", data);
    std::string dst = g_tmp + "\\ftz_out.ftz";
    std::remove(dst.c_str());

    FtzInfo info;
    FtzStatus st = ftzWrite(src, dst, 8, 1, &info);
    CHECK(st == FtzStatus::Ok);
    CHECK(info.frameCount == 3);  // 8 + 8 + ~1.3 MiB
    CHECK(info.originalSize == data.size());
    auto truth = Sha256::digest(data.data(), data.size());
    CHECK(std::memcmp(truth.data(), info.originalSha256, 32) == 0);

    // Compressible half compresses, random half must not expand: expect at
    // least some saving overall.
    std::FILE* f = std::fopen(dst.c_str(), "rb");
    std::fseek(f, 0, SEEK_END);
    const long containerSize = std::ftell(f);
    std::fclose(f);
    CHECK(containerSize > 0 && static_cast<std::size_t>(containerSize) < data.size());

    // Frames 0 and 1 contain mixed data (text + random): the random tail of
    // each 8 MiB frame dominates, so those frames may be stored raw. Frame 2
    // is pure random -> must be raw.
    CHECK(info.frames[2].raw);
    for (const auto& fe : info.frames) {
        CHECK(fe.storedSize <= fe.plainSize);  // never expands
    }

    // All frames verify.
    for (std::uint32_t i = 0; i < info.frameCount; ++i) {
        CHECK(ftzVerifyFrame(dst, info, i, /*decompress=*/true) == FtzStatus::Ok);
    }

    // Full extract -> byte-identical.
    std::string back = g_tmp + "\\ftz_back.bin";
    std::remove(back.c_str());
    std::vector<std::uint32_t> bad;
    CHECK(ftzExtract(dst, back, &bad) == FtzStatus::Ok);
    CHECK(bad.empty());
    std::FILE* g = std::fopen(back.c_str(), "rb");
    std::vector<std::uint8_t> got(data.size());
    CHECK(std::fread(got.data(), 1, got.size(), g) == got.size());
    std::fclose(g);
    CHECK(got == data);
}

static void testCorruptionDetection() {
    std::vector<std::uint8_t> data = makeMixed(9 * 1024 * 1024);
    std::string src = writeTemp("ftz_c.bin", data);
    std::string dst = g_tmp + "\\ftz_c.ftz";
    std::remove(dst.c_str());

    FtzInfo info;
    CHECK(ftzWrite(src, dst, 4, 1, &info) == FtzStatus::Ok);
    CHECK(info.frameCount == 3);

    // Corrupt one byte inside frame 1's stored payload.
    const FtzFrameEntry& f1 = info.frames[1];
    std::FILE* h = std::fopen(dst.c_str(), "r+b");
    CHECK(h != nullptr);
    std::fseek(h, static_cast<long>(f1.offset + f1.storedSize / 2), SEEK_SET);
    std::uint8_t orig = 0;
    CHECK(std::fread(&orig, 1, 1, h) == 1);
    std::fseek(h, static_cast<long>(f1.offset + f1.storedSize / 2), SEEK_SET);
    const std::uint8_t flipped = static_cast<std::uint8_t>(orig ^ 0xFF);
    CHECK(std::fwrite(&flipped, 1, 1, h) == 1);
    std::fclose(h);

    // Frame 0/2 still good; frame 1 detected as corrupt -> resume would
    // re-request exactly this frame.
    CHECK(ftzVerifyFrame(dst, info, 0, true) == FtzStatus::Ok);
    CHECK(ftzVerifyFrame(dst, info, 1, true) == FtzStatus::CorruptFrame);
    CHECK(ftzVerifyFrame(dst, info, 2, true) == FtzStatus::Ok);

    // Extract reports the bad frame and fails the whole-file hash.
    std::string back = g_tmp + "\\ftz_c_back.bin";
    std::remove(back.c_str());
    std::vector<std::uint32_t> bad;
    CHECK(ftzExtract(dst, back, &bad) == FtzStatus::CorruptFrame);
    CHECK(bad.size() == 1 && bad[0] == 1);
}

static void testHeaderRejection() {
    // Not an FTZ1 file at all.
    std::string junk = writeTemp("junk.bin", {'n', 'o', 't', 'a', 'c', 'o', 'n'});
    FtzInfo info;
    CHECK(ftzOpen(junk, info) == FtzStatus::NotFtz1);

    // Truncated: header present but manifest range past EOF.
    std::vector<std::uint8_t> data = makeMixed(5 * 1024 * 1024);
    std::string src = writeTemp("ftz_t.bin", data);
    std::string full = g_tmp + "\\ftz_t.ftz";
    std::remove(full.c_str());
    CHECK(ftzWrite(src, full, 4, 1, nullptr) == FtzStatus::Ok);

    FtzInfo fi;
    CHECK(ftzOpen(full, fi) == FtzStatus::Ok);

    // Simulate a truncated download: cut the manifest away.
    std::FILE* f = std::fopen(full.c_str(), "rb");
    std::vector<std::uint8_t> blob(fi.manifestOffset);
    CHECK(std::fread(blob.data(), 1, blob.size(), f) == blob.size());
    std::fclose(f);
    std::string trunc = g_tmp + "\\ftz_t_trunc.ftz";
    writeTemp("ftz_t_trunc.ftz", blob);

    // Frames still verifiable from the (still-intact) in-memory manifest --
    // this is exactly the resume semantic: keep the manifest from the first
    // attempt, verify what landed, request the rest.
    for (std::uint32_t i = 0; i < fi.frameCount; ++i) {
        CHECK(ftzVerifyFrame(trunc, fi, i, false) == FtzStatus::Ok);
    }
    // A manifest read from the truncated file itself is a short read.
    FtzInfo info2;
    CHECK(ftzOpen(trunc, info2) == FtzStatus::ShortFile);
}

static void testTinyInput() {
    // Single small file -> one frame, still a valid container.
    std::vector<std::uint8_t> data(100);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<std::uint8_t>(i);
    std::string src = writeTemp("ftz_s.bin", data);
    std::string dst = g_tmp + "\\ftz_s.ftz";
    std::remove(dst.c_str());
    FtzInfo info;
    CHECK(ftzWrite(src, dst, 8, 1, &info) == FtzStatus::Ok);
    CHECK(info.frameCount == 1);
    CHECK(info.frames[0].plainSize == 100);

    std::vector<std::uint8_t> out;
    CHECK(ftzReadFrame(dst, info, 0, out) == FtzStatus::Ok);
    CHECK(out == data);

    std::string back = g_tmp + "\\ftz_s_back.bin";
    std::remove(back.c_str());
    CHECK(ftzExtract(dst, back, nullptr) == FtzStatus::Ok);
}

int main() {
    const char* tmpEnv = std::getenv("TEMP");
    g_tmp = tmpEnv ? tmpEnv : ".";
    testRoundtrip();
    testCorruptionDetection();
    testHeaderRejection();
    testTinyInput();
    std::printf("ftz1: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
