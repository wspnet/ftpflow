// ftpflow tests -- SHA-256 against FIPS 180-4 vectors.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ftpflow/sha256.hpp"
#include "ftpflow/ssh_exec.hpp"

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

static void expectHex(const char* input, const char* expected) {
    const std::string got = Sha256::hex(input, std::strlen(input));
    ++g_checks;
    if (got != expected) {
        ++g_failed;
        std::fprintf(stderr, "FAIL sha256(\"%s\")\n  got      %s\n  expected %s\n",
                     input, got.c_str(), expected);
    }
}

int main() {
    // FIPS 180-4 / NIST vectors.
    expectHex("",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expectHex("abc",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expectHex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Block-boundary lengths (reference digests generated with Python
    // hashlib / OpenSSL): 55, 56, 64 and 65 bytes.
    expectHex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnop",
        "aa353e009edbaebfc6e494c8d847696896cb8b398e0173a4b5c1b636292d87c7");
    {
        const std::string s64(64, 'A');
        ++g_checks;
        if (Sha256::hex(s64.data(), s64.size()) !=
            "d53eda7a637c99cc7fb566d96e9fa109bf15c478410a3f5eb4d4c4e26cd081f6") {
            ++g_failed; std::fprintf(stderr, "FAIL sha256(64xA)\n");
        }
        const std::string s65(65, 'A');
        ++g_checks;
        if (Sha256::hex(s65.data(), s65.size()) !=
            "836203944f4c0280461ad73d31457c22ba19d1d99e232dc231000085899e00a2") {
            ++g_failed; std::fprintf(stderr, "FAIL sha256(65xA)\n");
        }
    }

    // One million 'a' via streaming updates (multi-block path).
    {
        Sha256 h;
        std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) h.update(chunk.data(), chunk.size());
        const auto d = h.final();
        static const char* kMillionA =
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
        std::string got;
        static const char* kHex = "0123456789abcdef";
        for (std::uint8_t b : d) { got.push_back(kHex[b >> 4]); got.push_back(kHex[b & 0xF]); }
        ++g_checks;
        if (got != kMillionA) { ++g_failed; std::fprintf(stderr, "FAIL million-a\n  got %s\n", got.c_str()); }
    }

    // Byte-by-byte streaming must equal one-shot.
    {
        const char* s = "streamed in single bytes for exactness";
        Sha256 h;
        for (const char* p = s; *p; ++p) h.update(p, 1);
        const std::string a = [&] {
            const auto d = h.final();
            static const char* kHex = "0123456789abcdef";
            std::string o;
            for (std::uint8_t b : d) { o.push_back(kHex[b >> 4]); o.push_back(kHex[b & 0xF]); }
            return o;
        }();
        CHECK(a == Sha256::hex(s, std::strlen(s)));
    }

    // SSH exec stub: a no-libssh2 build must return EngineUnavailable so
    // callers can feature-probe; a libssh2 build will really attempt the
    // connection (localhost may or may not run sshd, and 'nobody' won't
    // authenticate) -- either way it must fail cleanly and never hang.
    {
        SshExec ssh;
        SshEndpoint ep;
        ep.host = "localhost";
        ep.user = "nobody";
        SshResult r = ssh.open(ep, "true", {});
        CHECK(!r.ok());
#if FTPFLOW_HAVE_LIBSSH2
        CHECK(r.status != SshStatus::EngineUnavailable);
#else
        CHECK(r.status == SshStatus::EngineUnavailable);
#endif
        CHECK(std::strlen(sshStatusName(r.status)) > 0);
        CHECK(!ssh.isOpen());
        CHECK(ssh.exitCode() == -1);
    }

    std::printf("sha256_ssh: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
