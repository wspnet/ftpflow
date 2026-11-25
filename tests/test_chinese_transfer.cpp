// test_chinese_transfer -- end-to-end: verify Chinese filename upload/download
// against a real server. Exercises both (a) a fresh session that resolves the
// server encoding from the login greeting WITHOUT listing any directory (the
// GUI transfer-queue path), and (b) the auto-detect + lock path after a list.
//
// Usage: test_chinese_transfer <host> <user> <pass> [port]
#include "ftpflow/ftp_session.hpp"
#include "ftpflow/text_codec.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
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

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <host> <user> <pass> [port]\n", argv[0]);
        return 2;
    }
    FtpOptions opt;
    opt.host = argv[1];
    opt.user = argv[2];
    opt.password = argv[3];
    if (argc >= 5) opt.port = static_cast<std::uint16_t>(std::atoi(argv[4]));
    opt.encoding = "auto";

    const std::string localPath = "C:\\Users\\wspne\\测试上传中文.txt";
    std::FILE* w = openLocalFile(localPath, "wb");
    if (w) { std::fputs("chinese transfer round trip\n", w); std::fclose(w); }

    // 0. Regression for "Chinese filename upload fails at STOR". The GUI
    //    transfer queue opens its OWN FtpSession from a request snapshot and
    //    uploads without ever listing the target directory. If that directory
    //    contains only ASCII names, list()-based auto-detect cannot lock the
    //    encoding, so makeUrl() used to send UTF-8 bytes to a GBK server and
    //    STOR was rejected (CURLE_UPLOAD_FAILED, "Upload failed ... before it
    //    took off"). open() now resolves the encoding from the login greeting.
    {
        FtpSession bare;
        FtpResult ro = bare.open(opt);
        CHECK(ro.status == FtpStatus::Ok);
        if (ro.status != FtpStatus::Ok) {
            std::printf("bare open failed: %s\n", ro.message.c_str());
        } else {
            std::printf("bare-session effectiveEncoding (no list) = [%s]\n",
                        bare.effectiveEncoding().c_str());
            const std::string rp = "/ftpflow_裸会话中文.txt";
            std::uint64_t mv = 0;
            FtpResult up0 = bare.upload(localPath, rp, 0, &mv);
            std::printf("bare upload status=%s moved=%llu msg=[%s]\n",
                        ftpStatusName(up0.status), (unsigned long long)mv,
                        up0.message.c_str());
            CHECK(up0.status == FtpStatus::Ok);
            if (up0.status == FtpStatus::Ok) bare.removeFile(rp);  // cleanup
        }
    }

    FtpSession ftp;
    FtpResult r = ftp.open(opt);
    CHECK(r.status == FtpStatus::Ok);
    if (r.status != FtpStatus::Ok) {
        std::printf("open failed: %s\n", r.message.c_str());
        return 1;
    }

    // 1. List root to trigger auto-detect + lock (mirrors the GUI).
    ListingPage root = ftp.list("/");
    std::printf("listed %zu entries\n", root.entries.size());
    const std::string locked = ftp.effectiveEncoding();
    std::printf("effectiveEncoding after list = [%s]\n", locked.c_str());

    // 2. Upload a Chinese-named file using the locked encoding.
    const std::string remotePath = "/测试上传中文.txt";
    std::uint64_t moved = 0;
    FtpResult up = ftp.upload(localPath, remotePath, 0, &moved);
    std::printf("upload status=%s moved=%llu msg=%s\n",
                ftpStatusName(up.status),
                (unsigned long long)moved, up.message.c_str());
    CHECK(up.status == FtpStatus::Ok);

    // 3. Download it back to a different local name.
    const std::string downPath = "C:\\Users\\wspne\\下载回中文.txt";
    std::uint64_t dmoved = 0;
    FtpResult dn = ftp.download(remotePath, downPath, 0, &dmoved);
    std::printf("download status=%s moved=%llu msg=%s\n",
                ftpStatusName(dn.status),
                (unsigned long long)dmoved, dn.message.c_str());
    CHECK(dn.status == FtpStatus::Ok);

    // 4. Verify round-trip content.
    if (dn.status == FtpStatus::Ok) {
        std::FILE* rd = openLocalFile(downPath, "rb");
        char buf[128] = {0};
        if (rd) { std::fread(buf, 1, sizeof(buf) - 1, rd); std::fclose(rd); }
        std::printf("downloaded content: [%s]\n", buf);
        CHECK(std::string(buf) == "chinese transfer round trip\n");
    }

    // 5. Cross-session check (the GUI transfer-queue path): the queue opens
    //    its OWN FtpSession from a request snapshot. Verify that a fresh
    //    session using the *locked* encoding can download the Chinese-named
    //    file -- without this, a GBK server receives UTF-8 bytes for the
    //    RETR path and the download stalls forever.
    if (locked == "gbk") {
        FtpOptions tOpt = opt;
        tOpt.encoding = locked;   // what makeRequest()+effectiveEncoding() sets
        FtpSession xfer;
        CHECK(xfer.open(tOpt).status == FtpStatus::Ok);
        const std::string xdown = "C:\\Users\\wspne\\下载跨会话中文.txt";
        std::uint64_t xmoved = 0;
        FtpResult xdn = xfer.download(remotePath, xdown, 0, &xmoved);
        std::printf("cross-session download status=%s moved=%llu msg=[%s]\n",
                    ftpStatusName(xdn.status), (unsigned long long)xmoved,
                    xdn.message.c_str());
        CHECK(xdn.status == FtpStatus::Ok);
        if (xdn.status == FtpStatus::Ok) {
            std::FILE* xr = openLocalFile(xdown, "rb");
            char xbuf[128] = {0};
            if (xr) { std::fread(xbuf, 1, sizeof(xbuf) - 1, xr); std::fclose(xr); }
            CHECK(std::string(xbuf) == "chinese transfer round trip\n");
        }
    }

    // 6. Cross-directory move regression. "Drag onto a folder" in the file
    //    list becomes FtpSession::rename() with different parents on each
    //    side. On IIS both operands must be PHYSICAL absolute paths
    //    (homeDir-prefixed): the virtual "/a/x.txt" form answers 550 and the
    //    move silently fails.
    {
        const std::string stamp =
            std::to_string(static_cast<long long>(std::time(nullptr)));
        const std::string dirA = "/ftpflow_mv_a" + stamp;
        const std::string dirB = "/ftpflow_mv_b" + stamp;
        const std::string from = dirA + "/x.txt";
        const std::string to = dirB + "/x.txt";

        FtpResult ma = ftp.mkdir(dirA);
        FtpResult mb = ftp.mkdir(dirB);
        std::printf("move-src mkdir=%s move-dst mkdir=%s\n",
                    ftpStatusName(ma.status), ftpStatusName(mb.status));
        CHECK(ma.status == FtpStatus::Ok);
        CHECK(mb.status == FtpStatus::Ok);

        std::uint64_t mv = 0;
        FtpResult upm = ftp.upload(localPath, from, 0, &mv);
        CHECK(upm.status == FtpStatus::Ok);

        FtpResult rn = ftp.rename(from, to);
        std::printf("cross-dir move status=%s msg=[%s]\n",
                    ftpStatusName(rn.status), rn.message.c_str());
        CHECK(rn.status == FtpStatus::Ok);

        auto hasEntry = [&](const std::string& dir, const std::string& name) {
            ListingPage p = ftp.list(dir);
            for (const Entry& e : p.entries)
                if (e.name == name) return true;
            return false;
        };
        CHECK(hasEntry(dirB, "x.txt"));
        CHECK(!hasEntry(dirA, "x.txt"));

        // Cleanup.
        ftp.removeFile(to);
        ftp.removeDir(dirA);
        ftp.removeDir(dirB);
    }

    // Cleanup the file we created on the server.
    if (up.status == FtpStatus::Ok) ftp.removeFile(remotePath);

    std::printf("chinese_transfer: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
