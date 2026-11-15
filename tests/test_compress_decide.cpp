// ftpflow tests -- compressibility decision funnel, stages 1-3.
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "ftpflow/compress_decide.hpp"

using namespace ftpflow;

static int g_checks = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                 \
        }                                                               \
    } while (0)
static int g_failed = 0;

static Decision ext(const char* name) { return decideByExtension(name); }

static void testStage1() {
    CHECK(ext("readme.txt").verdict == Verdict::Compress);
    CHECK(ext("server.LOG").verdict == Verdict::Compress);    // case-insensitive
    CHECK(ext("main.CPP").verdict == Verdict::Compress);
    CHECK(ext("style.css").verdict == Verdict::Compress);
    CHECK(ext("archive.tar").verdict == Verdict::Compress);
    CHECK(ext("dump.pcap").verdict == Verdict::Compress);

    CHECK(ext("photo.jpg").verdict == Verdict::Store);
    CHECK(ext("doc.pdf").verdict == Verdict::Store);
    CHECK(ext("report.docx").verdict == Verdict::Store);
    CHECK(ext("app.apk").verdict == Verdict::Store);
    CHECK(ext("data.sqlite").verdict == Verdict::Store);
    CHECK(ext("v1.zst").verdict == Verdict::Store);

    CHECK(ext("noext").verdict == Verdict::Undecided);
    CHECK(ext(".gitignore").verdict == Verdict::Undecided);
}

static Decision magic(const std::vector<std::uint8_t>& head) {
    return decideByMagic(head.data(), head.size());
}

static void testStage2() {
    // ZIP (which is also docx/jar/apk inside).
    std::vector<std::uint8_t> zip = {0x50, 0x4B, 0x03, 0x04, 0x00, 0x00};
    CHECK(magic(zip).verdict == Verdict::Store);
    // gzip.
    std::vector<std::uint8_t> gz = {0x1F, 0x8B, 0x08, 0x00};
    CHECK(magic(gz).verdict == Verdict::Store);
    // zstd frame.
    std::vector<std::uint8_t> zstd = {0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x00};
    CHECK(magic(zstd).verdict == Verdict::Store);
    // PNG.
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(magic(png).verdict == Verdict::Store);
    // JPEG.
    std::vector<std::uint8_t> jpg = {0xFF, 0xD8, 0xFF, 0xE0};
    CHECK(magic(jpg).verdict == Verdict::Store);
    // PDF.
    std::vector<std::uint8_t> pdf = {'%', 'P', 'D', 'F', '-', '1', '.', '7'};
    CHECK(magic(pdf).verdict == Verdict::Store);
    // UTF-8 BOM text.
    std::vector<std::uint8_t> bom = {0xEF, 0xBB, 0xBF, '#', ' ', 't', 'o'};
    CHECK(magic(bom).verdict == Verdict::Compress);

    // A renamed zip wearing a .txt name must still be caught by magic.
    FileProbe p;
    p.fileName = "innocent.txt";
    p.head = zip;
    Decision d = decideCompressibility(p);
    CHECK(d.verdict == Verdict::Store);
    CHECK(d.stage == 2);

    // Random-looking head with unknown extension: undecided at stage 2.
    FileProbe p2;
    p2.fileName = "blob.dat";
    std::vector<std::uint8_t> rnd = {0x01, 0x27, 0xF3, 0x90, 0xAA, 0x55};
    p2.head = rnd;
    CHECK(decideByMagic(rnd.data(), rnd.size()).verdict == Verdict::Undecided);
}

static void testStage3() {
    // Text-like sample: low entropy.
    std::string text(65536, '\0');
    for (std::size_t i = 0; i < text.size(); ++i) text[i] = 'a' + (i % 3);
    auto t = reinterpret_cast<const std::uint8_t*>(text.data());
    CHECK(decideByEntropy(t, text.size()).verdict == Verdict::Compress);
    CHECK(shannonEntropyBits(t, text.size()) < 2.0);

    // Pseudo-random: high entropy -> Store. xoshiro-ish LCG, good enough
    // to push entropy over 7.2 bits/byte.
    std::vector<std::uint8_t> rnd(65536);
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (auto& b : rnd) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        b = static_cast<std::uint8_t>(s >> 32);
    }
    CHECK(shannonEntropyBits(rnd.data(), rnd.size()) > 7.5);
    CHECK(decideByEntropy(rnd.data(), rnd.size()).verdict == Verdict::Store);

    // Too small to judge.
    std::vector<std::uint8_t> tiny(64, 0);
    CHECK(decideByEntropy(tiny.data(), tiny.size()).verdict == Verdict::Undecided);
}

static void testFunnel() {
    // .txt + text bytes -> Compress at stage 1 (short-circuit).
    FileProbe p;
    p.fileName = "notes.txt";
    p.head = {'h', 'e', 'l', 'l', 'o'};
    Decision d = decideCompressibility(p);
    CHECK(d.verdict == Verdict::Compress);
    CHECK(d.stage == 1);

    // .zip -> Store at stage 1.
    FileProbe p2;
    p2.fileName = "backup.zip";
    d = decideCompressibility(p2);
    CHECK(d.verdict == Verdict::Store);
    CHECK(d.stage == 1);

    // Unknown extension + no bytes at all -> safe Store default.
    FileProbe p3;
    p3.fileName = "mystery.xyz";
    d = decideCompressibility(p3);
    CHECK(d.verdict == Verdict::Store);
}

int main() {
    testStage1();
    testStage2();
    testStage3();
    testFunnel();
    std::printf("compress_decide: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
