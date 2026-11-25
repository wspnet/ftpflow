// test_gbk_navigate -- end-to-end: list root (GBK names) then CWD into a
// Chinese directory, verifying the UTF-8 -> GBK round-trip lets the server
// accept the directory change. Requires gbk_ftp_server.py on 127.0.0.1:2121.
#include "ftpflow/ftp_session.hpp"

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
    FtpOptions opt;
    opt.host = "127.0.0.1";
    opt.port = 2121;
    opt.user = "gbkuser";
    opt.password = "gbkpass";
    opt.encoding = "auto";  // exercise the auto-detect + lock path

    FtpSession ftp;
    FtpResult r = ftp.open(opt);
    CHECK(r.status == FtpStatus::Ok);
    if (r.status != FtpStatus::Ok) {
        std::printf("open failed: %s\n", r.message.c_str());
        return 1;
    }

    // 1. List root: expect the Chinese directory to be decoded to UTF-8.
    ListingPage root = ftp.list("/");
    bool sawChineseDir = false;
    for (const Entry& e : root.entries) {
        std::printf("root entry: [%s] type=%d\n", e.name.c_str(),
                    (int)e.type);
        if (e.name == "\xE4\xB8\xAD\xE6\x96\x87\xE7\x9B\xAE\xE5\xBD\x95")  // 中文目录
            sawChineseDir = true;
    }
    CHECK(sawChineseDir);

    // 2. Navigate into the Chinese directory. This is the case that used to
    //    fail with "Server denied you to change to the given directory".
    ListingPage sub = ftp.list("/\xE4\xB8\xAD\xE6\x96\x87\xE7\x9B\xAE\xE5\xBD\x95");
    bool sawChineseFile = false;
    for (const Entry& e : sub.entries) {
        std::printf("sub entry: [%s] type=%d\n", e.name.c_str(), (int)e.type);
        if (e.name.find("\xE6\x96\x87\xE4\xBB\xB6") != std::string::npos)  // 文件
            sawChineseFile = true;
    }
    CHECK(sawChineseFile);
    if (!sawChineseFile) {
        std::printf("lastErr after sub list: %s\n", ftp.lastError().c_str());
    }

    std::printf("gbk_navigate: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
