// test_text_codec -- filename byte-encoding conversion (UTF-8/GBK/auto).
//
// Pure unit checks (no network): isValidUtf8, normalizeEncoding, and
// toUtf8 for the three modes. GBK bytes are hardcoded (Chinese filenames on
// a GBK-encoded IIS FTP server) so the test is deterministic on any host.
#include "ftpflow/text_codec.hpp"

#include <cstdio>
#include <string>

using namespace ftpflow;

static int g_checks = 0;
static int g_failed = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++g_failed;                                                      \
        }                                                                    \
    } while (0)

int main() {
    // ---- normalizeEncoding -------------------------------------------------
    CHECK(normalizeEncoding("") == "auto");
    CHECK(normalizeEncoding("auto") == "auto");
    CHECK(normalizeEncoding("utf-8") == "utf-8");
    CHECK(normalizeEncoding("UTF8") == "utf-8");
    CHECK(normalizeEncoding("gbk") == "gbk");
    CHECK(normalizeEncoding("GB2312") == "gbk");
    CHECK(normalizeEncoding("GB18030") == "gbk");
    CHECK(normalizeEncoding("cp936") == "gbk");
    CHECK(normalizeEncoding("936") == "gbk");
    CHECK(normalizeEncoding("ansi") == "gbk");
    CHECK(normalizeEncoding("bogus") == "auto");

    // ---- isValidUtf8 -------------------------------------------------------
    CHECK(isValidUtf8(""));                       // empty is valid
    CHECK(isValidUtf8("plain ascii"));
    CHECK(isValidUtf8("\xE4\xB8\xAD\xE6\x96\x87"));  // "中文" UTF-8
    CHECK(!isValidUtf8("\xC0\xAF"));              // overlong 2-byte
    CHECK(!isValidUtf8("\xE4\xB8"));              // truncated 3-byte
    CHECK(!isValidUtf8("\xFF\xFE"));              // stray lead bytes
    CHECK(!isValidUtf8("\xED\xA0\x80"));          // surrogate (U+D800)

    // ---- toUtf8: utf-8 passthrough ----------------------------------------
    const std::string zhUtf8 = "\xE4\xB8\xAD\xE6\x96\x87";  // 中文
    CHECK(toUtf8(zhUtf8, "utf-8") == zhUtf8);
    CHECK(toUtf8(zhUtf8, "auto") == zhUtf8);      // valid UTF-8 kept as-is

    // ---- toUtf8: GBK -> UTF-8 ---------------------------------------------
    // "中文" in GBK: 中=D6 D0, 文=CE C4.
    const std::string zhGbk = "\xD6\xD0\xCE\xC4";
    const std::string gbkOut = toUtf8(zhGbk, "gbk");
    CHECK(gbkOut == zhUtf8);                      // GBK decoded to UTF-8
    // auto mode: GBK bytes are NOT valid UTF-8, so they must decode as GBK.
    CHECK(toUtf8(zhGbk, "auto") == zhUtf8);

    // ---- ASCII names are untouched by every mode --------------------------
    CHECK(toUtf8("Areas", "gbk") == "Areas");
    CHECK(toUtf8("Areas", "auto") == "Areas");
    CHECK(toUtf8("Web.config", "gbk") == "Web.config");

    // ---- fromUtf8: UTF-8 -> server bytes (inverse of toUtf8) --------------
    // UTF-8 path round-trips to GBK bytes on a GBK server.
    CHECK(fromUtf8(zhUtf8, "gbk") == zhGbk);
    // utf-8 / auto modes leave the path untouched (server is assumed UTF-8).
    CHECK(fromUtf8(zhUtf8, "utf-8") == zhUtf8);
    CHECK(fromUtf8(zhUtf8, "auto") == zhUtf8);
    // ASCII paths pass through every mode.
    CHECK(fromUtf8("Areas", "gbk") == "Areas");
    CHECK(fromUtf8("/Areas/bin", "gbk") == "/Areas/bin");
    // Full round-trip: UTF-8 -> GBK -> UTF-8 recovers the original name.
    CHECK(toUtf8(fromUtf8(zhUtf8, "gbk"), "gbk") == zhUtf8);

    std::printf("text_codec: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
