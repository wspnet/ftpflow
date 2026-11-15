// test_ssh_transfer -- the remote-agent leg of the compressed push path.
//
// sshPushFile() (client side) is exercised end-to-end manually against a
// real sshd; what an automated test can pin down is the *agent contract*:
// ftpcli zdecode must consume exactly what a ZSTD_CStream produces (with
// multi-chunk writes, exactly like sshPushFile ships them) and emit
// byte-identical output through a pipe.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(FTPFLOW_HAVE_ZSTD)
#include <zstd.h>
#endif

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

#if defined(FTPFLOW_HAVE_ZSTD)

namespace {

std::string exeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    const std::size_t slash = p.find_last_of("\\/");
    return p.substr(0, slash);
}

}  // namespace

int main() {
    // Payload: compressible head + incompressible tail, ~6 MiB.
    std::vector<std::uint8_t> plain;
    for (int i = 0; i < 200000; ++i) {
        const char* blk = "ftpflow zdecode agent contract ";
        plain.insert(plain.end(), blk, blk + 31);
    }
    for (std::size_t i = 0; i < 512 * 1024; ++i)
        plain.push_back((std::uint8_t)(i * 2654435761u >> 24));

    // Producer side: mirror sshPushFile's exact streaming shape
    // (256 KiB reads, compressStream loop, endStream epilogue).
    std::vector<std::uint8_t> zipped;
    {
        ZSTD_CStream* cs = ZSTD_createCStream();
        CHECK(cs != nullptr);
        CHECK(!ZSTD_isError(ZSTD_initCStream(cs, 3)));
        std::vector<std::uint8_t> out(256 * 1024 + 128 * 1024);
        std::size_t pos = 0;
        while (pos < plain.size()) {
            const std::size_t n = std::min<std::size_t>(256 * 1024, plain.size() - pos);
            ZSTD_inBuffer zin{plain.data() + pos, n, 0};
            while (zin.pos < zin.size) {
                ZSTD_outBuffer zout{out.data(), out.size(), 0};
                CHECK(!ZSTD_isError(ZSTD_compressStream(cs, &zout, &zin)));
                zipped.insert(zipped.end(), out.data(), out.data() + zout.pos);
            }
            pos += n;
        }
        for (;;) {
            ZSTD_outBuffer zout{out.data(), out.size(), 0};
            const std::size_t r = ZSTD_endStream(cs, &zout);
            CHECK(!ZSTD_isError(r));
            zipped.insert(zipped.end(), out.data(), out.data() + zout.pos);
            if (r == 0) break;
        }
        ZSTD_freeCStream(cs);
    }
    std::printf("plain %zu -> zstd %zu bytes (%.1f%%)\n", plain.size(),
                zipped.size(),
                100.0 * (double)zipped.size() / (double)plain.size());
    CHECK(zipped.size() < plain.size() / 2);

    // Agent leg: pipe the compressed stream into `ftpcli zdecode` exactly
    // like a remote shell would (`... zdecode > out`), then compare. Paths
    // under test are quote-free; cmd /c mangles leading-quoted commands.
    // ftpcli sits in the sibling cli/ directory of the test binary.
    const std::string cliPath = exeDir() + "\\..\\cli\\ftpcli.exe";
    const std::string outPath = exeDir() + "\\zdecode_test_out.bin";
    const std::string cmdLine = cliPath + " zdecode > " + outPath;
    FILE* agent = _popen(cmdLine.c_str(), "wb");
    CHECK(agent != nullptr);
    if (agent) {
        const std::size_t wrote = std::fwrite(zipped.data(), 1, zipped.size(), agent);
        CHECK(wrote == zipped.size());
        const int prc = _pclose(agent);
        CHECK(prc == 0);
    }

    std::vector<std::uint8_t> result;
    {
        std::FILE* f = std::fopen(outPath.c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            std::uint8_t buf[64 * 1024];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                result.insert(result.end(), buf, buf + n);
            std::fclose(f);
        }
    }
    CHECK(result.size() == plain.size());
    CHECK(result == plain);
    DeleteFileA(outPath.c_str());

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL SSH-TRANSFER TESTS PASSED\n");
    return g_failed == 0 ? 0 : 1;
}

#else  // !FTPFLOW_HAVE_ZSTD

int main() {
    std::printf("built without zstd; nothing to test\n");
    return 0;
}

#endif
