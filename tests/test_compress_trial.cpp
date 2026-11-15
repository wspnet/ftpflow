// ftpflow tests -- stage 4 trial compression + stage 5 cost model.
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "ftpflow/compress_decide.hpp"

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

static std::vector<std::uint8_t> makeText(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    const char* words[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "logs "};
    std::size_t w = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::uint8_t>(words[w][i % 5]);
        if (i % 5 == 4) w = (w + 1) % 6;
    }
    return v;
}

static std::vector<std::uint8_t> makeRandom(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (auto& b : v) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        b = static_cast<std::uint8_t>(s >> 32);
    }
    return v;
}

static void testTrial() {
    // Repetitive text must compress well.
    std::vector<std::uint8_t> text = makeText(64 * 1024);
    TrialResult t = trialCompress(text.data(), text.size());
    CHECK(t.ok);
    CHECK(t.sampleBytes == text.size());
    CHECK(t.ratio < 0.5);
    CHECK(t.measuredCompressBps > 0);

    // Random data: no gain, ratio pinned at 1.0.
    std::vector<std::uint8_t> rnd = makeRandom(64 * 1024);
    TrialResult t2 = trialCompress(rnd.data(), rnd.size());
    CHECK(t2.ok);
    CHECK(t2.ratio >= 0.98);

    // Empty / null input: engine reports nothing to do.
    TrialResult t3 = trialCompress(nullptr, 0);
    CHECK(!t3.ok);
}

static void testCostModel() {
    const double MiB = 1024.0 * 1024.0;

    // Deterministic pure-math checks. 100 MiB file, ratio 0.3.
    const std::uint64_t S = 100ULL * 1024 * 1024;
    const double C = 250.0 * MiB;

    // Slow-ish link (20 MiB/s): saving 70 MiB is worth the CPU.
    CostModel slow;
    slow.bandwidthBps = 20.0 * MiB;
    CHECK(decideByCostModel(S, 0.3, C, slow).verdict == Verdict::Compress);

    // 10 GbE-class link (~1250 MiB/s): CPU cost dominates, store it.
    CostModel fast;
    fast.bandwidthBps = 1250.0 * MiB;
    CHECK(decideByCostModel(S, 0.3, C, fast).verdict == Verdict::Store);

    // Tiny file: any saving is below the aggregation threshold.
    CostModel anyBw;
    CHECK(decideByCostModel(30 * 1024, 0.2, C, anyBw).verdict == Verdict::Store);

    // Ratio ~1.0 -> never worth it.
    CHECK(decideByCostModel(S, 1.0, C, anyBw).verdict == Verdict::Store);

    // Gain below the 10% threshold.
    CHECK(decideByCostModel(S, 0.93, C, anyBw).verdict == Verdict::Store);

    // Zero-byte file.
    CHECK(decideByCostModel(0, 0.1, C, anyBw).verdict == Verdict::Store);

    // Stage number is set.
    Decision d = decideByCostModel(S, 0.3, C, slow);
    CHECK(d.stage == 5);
}

static void testFullFunnel() {
    const double MiB = 1024.0 * 1024.0;
    std::vector<std::uint8_t> text = makeText(3 * 64 * 1024);

    // Big text log over a slow link -> the funnel should end in Compress
    // at stage 5 (confirmed by trial + cost math).
    FileProbe p;
    p.fileName = "deploy.log";
    p.fileSize = 200ULL * 1024 * 1024;
    p.head.assign(text.begin(), text.begin() + 32);
    p.sample = text;
    CostModel cm;
    cm.bandwidthBps = 20.0 * MiB;
    cm.compressBps = 250.0 * MiB;   // pin C so the math is deterministic
    Decision d = decideFullFunnel(p, cm);
    CHECK(d.verdict == Verdict::Compress);
    CHECK(d.stage == 5);

    // Same file on a 10 GbE link -> store, link is faster than the CPU.
    CostModel lan;
    lan.bandwidthBps = 1250.0 * MiB;
    lan.compressBps = 250.0 * MiB;
    d = decideFullFunnel(p, lan);
    CHECK(d.verdict == Verdict::Store);
    CHECK(d.stage == 5);

    // A renamed zip must still be caught before the trial ever runs.
    FileProbe z;
    z.fileName = "innocent.txt";
    z.fileSize = 100ULL * 1024 * 1024;
    z.head = {0x50, 0x4B, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00};
    z.sample = makeRandom(64 * 1024);
    d = decideFullFunnel(z, cm);
    CHECK(d.verdict == Verdict::Store);
    CHECK(d.stage == 2);

    // Already-compressed extension: short-circuits at stage 1.
    FileProbe j;
    j.fileName = "movie.mp4";
    j.fileSize = 8ULL * 1024 * 1024 * 1024;
    j.sample = makeRandom(64 * 1024);
    d = decideFullFunnel(j, cm);
    CHECK(d.verdict == Verdict::Store);
    CHECK(d.stage == 1);

    // No sample at all (metadata-only probe): prior verdict stands.
    FileProbe m;
    m.fileName = "notes.txt";
    d = decideFullFunnel(m, cm);
    CHECK(d.verdict == Verdict::Compress);
}

int main() {
    testTrial();
    testCostModel();
    testFullFunnel();
    std::printf("compress_trial: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
