// test_tree_walk -- local tree walking + recursive queue transfers.
//
// Part A (always runs): build a local tree on disk, walk it, check the
// flat entry list (paths, dir/file classification, sizes), joinTreePath
// edge cases.
//
// Part B (runs when the dev pyftpdlib is up on 127.0.0.1:2121): FTP
// metadata ops (MKD / DELE / RMD / RNFR+RNTO) + a recursive upload and
// recursive download roundtrip through TransferQueue::enqueueTree.
//
// Part C (runs when the local sshd answers and the test key exists):
// SFTP recursive roundtrip; one large compressible file forces the SSH
// Compress pipeline through the tree path (mkdir -p + zstd agent).
#include "ftpflow/tree_walk.hpp"
#include "ftpflow/transfer_queue.hpp"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

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

static bool tcpProbe(const char* host, unsigned port, int timeoutMs) {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%u", port);
    bool ok = false;
    if (getaddrinfo(host, portStr, &hints, &res) == 0 && res) {
        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s != INVALID_SOCKET) {
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            connect(s, res->ai_addr, (int)res->ai_addrlen);
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            timeval tv{0, timeoutMs * 1000};
            ok = select(0, nullptr, &w, nullptr, &tv) > 0;
            closesocket(s);
        }
    }
    if (res) freeaddrinfo(res);
    WSACleanup();
    return ok;
}

static std::string readAll(const std::string& path) {
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[64 * 1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

static void writeFile(const std::string& path, const std::string& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

static std::string exeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    const std::size_t slash = p.find_last_of("\\/");
    return p.substr(0, slash);
}

// Best-effort removal of a tree this test created (files first, then
// dirs deepest-first, then the root).
static void rmTree(const std::string& dir) {
    std::vector<ftpflow::TreeEntry> entries;
    if (!ftpflow::walkLocalTree(dir, entries)) {
        RemoveDirectoryA(dir.c_str());
        return;
    }
    for (const auto& e : entries) {
        if (!e.isDir) DeleteFileA((dir + "/" + e.relPath).c_str());
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->isDir) RemoveDirectoryA((dir + "/" + it->relPath).c_str());
    }
    RemoveDirectoryA(dir.c_str());
}

static void rmRemoteTreeFtp(ftpflow::FtpSession& ftp, const std::string& dir) {
    std::vector<ftpflow::TreeEntry> entries;
    if (!ftpflow::walkRemoteTree(ftp, dir, entries)) return;
    for (const auto& e : entries) {
        if (!e.isDir) ftp.removeFile(ftpflow::joinTreePath(dir, e.relPath));
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->isDir) ftp.removeDir(ftpflow::joinTreePath(dir, it->relPath));
    }
    ftp.removeDir(dir);
}

struct Recorded {
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<int> failed{0};
};

static void record(Recorded* r, const ftpflow::TransferEvent& e) {
    if (e.kind == ftpflow::TransferEvent::Kind::Started) ++r->started;
    if (e.kind == ftpflow::TransferEvent::Kind::Finished) {
        ++r->finished;
        if (e.state == ftpflow::TransferState::Failed) {
            ++r->failed;
            std::fprintf(stderr, "  [job %llu] failed: %s\n",
                (unsigned long long)e.jobId, e.error.c_str());
        }
    }
}

int main() {
    const std::string tmp = "C:/Users/wspne/AppData/Local/Temp";
    const std::string root = tmp + "/ftpflow_tw";

    // ---- Part A: local walk ------------------------------------------------
    rmTree(root);
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA((root + "/a").c_str(), nullptr);
    CreateDirectoryA((root + "/a/b").c_str(), nullptr);
    writeFile(root + "/top.txt", "top-level");
    writeFile(root + "/a/one.bin", "11111");
    writeFile(root + "/a/b/two.bin", "2222222222");
    writeFile(root + "/a/b/three.bin", "333");

    {
        std::vector<ftpflow::TreeEntry> out;
        CHECK(ftpflow::walkLocalTree(root, out));
        CHECK(out.size() == 6);
        // Sorted by relPath: a, a/b, a/b/three.bin, a/b/two.bin, a/one.bin,
        // top.txt
        CHECK(out[0].relPath == "a" && out[0].isDir);
        CHECK(out[1].relPath == "a/b" && out[1].isDir);
        CHECK(out[2].relPath == "a/b/three.bin" && !out[2].isDir);
        CHECK(out[2].size == 3);
        CHECK(out[3].relPath == "a/b/two.bin" && out[3].size == 10);
        CHECK(out[4].relPath == "a/one.bin" && out[4].size == 5);
        CHECK(out[5].relPath == "top.txt" && out[5].size == 9);

        std::vector<ftpflow::TreeEntry> missing;
        CHECK(!ftpflow::walkLocalTree(root + "/nope", missing));
    }

    {
        CHECK(ftpflow::joinTreePath("/r", "x/y") == "/r/x/y");
        CHECK(ftpflow::joinTreePath("/r/", "x") == "/r/x");
        CHECK(ftpflow::joinTreePath("C:/r", "x") == "C:/r/x");
        CHECK(ftpflow::joinTreePath("/r", "") == "/r");
    }

    // ---- Part B: FTP metadata + recursive roundtrip ------------------------
    if (tcpProbe("127.0.0.1", 2121, 300)) {
        ftpflow::FtpSession ftp;
        ftpflow::FtpOptions fopt;
        fopt.host = "127.0.0.1";
        fopt.port = 2121;
        fopt.user = "tester";
        fopt.password = "secret";
        CHECK(ftp.open(fopt).ok());

        // Metadata ops (MKD / RNFR+RNTO / RMD / DELE).
        CHECK(ftp.mkdir("/tw_dir").ok());
        CHECK(ftp.rename("/tw_dir", "/tw_dir2").ok());
        CHECK(ftp.removeDir("/tw_dir2").ok());
        writeFile(tmp + "/tw_meta.txt", "meta");
        std::uint64_t moved = 0;
        CHECK(ftp.upload(tmp + "/tw_meta.txt", "/tw_meta.txt", 0, &moved).ok());
        CHECK(ftp.rename("/tw_meta.txt", "/tw_meta2.txt").ok());
        CHECK(ftp.removeFile("/tw_meta2.txt").ok());

        // Recursive upload: the Part A tree -> /tw_up (4 file jobs).
        {
            ftpflow::TransferQueue q(2, 50);
            Recorded r;
            q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });
            ftpflow::TransferRequest tr;
            tr.protocol = ftpflow::BackendKind::Ftp;
            tr.ftp = fopt;
            tr.localPath = root;
            tr.remotePath = "/tw_up";
            std::string err;
            const std::vector<std::uint64_t> ids = q.enqueueTree(tr, &err);
            CHECK(ids.size() == 4);
            CHECK(err.empty());
            q.waitAll();
            CHECK(r.finished.load() == 4);
            CHECK(r.failed.load() == 0);

            std::vector<ftpflow::TreeEntry> remote;
            CHECK(ftpflow::walkRemoteTree(ftp, "/tw_up", remote));
            CHECK(remote.size() == 6);
        }

        // Recursive download: /tw_up -> a fresh local tree, byte compare.
        {
            const std::string down = tmp + "/ftpflow_tw_down";
            rmTree(down);
            ftpflow::TransferQueue q(2, 50);
            Recorded r;
            q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });
            ftpflow::TransferRequest tr;
            tr.protocol = ftpflow::BackendKind::Ftp;
            tr.ftp = fopt;
            tr.direction = ftpflow::TransferDirection::Download;
            tr.remotePath = "/tw_up";
            tr.localPath = down;
            std::string err;
            const std::vector<std::uint64_t> ids = q.enqueueTree(tr, &err);
            CHECK(ids.size() == 4);
            q.waitAll();
            CHECK(r.finished.load() == 4);
            CHECK(r.failed.load() == 0);
            CHECK(readAll(down + "/top.txt") == "top-level");
            CHECK(readAll(down + "/a/one.bin") == "11111");
            CHECK(readAll(down + "/a/b/two.bin") == "2222222222");
            CHECK(readAll(down + "/a/b/three.bin") == "333");
            rmTree(down);
        }

        rmRemoteTreeFtp(ftp, "/tw_up");
        std::remove((tmp + "/tw_meta.txt").c_str());
    } else {
        std::printf("[info] no live pyftpdlib on 127.0.0.1:2121 -> skipping Part B\n");
    }

    // ---- Part C: SFTP recursive roundtrip (local sshd + test key) ----------
    {
        const char* keyPath = "C:/Users/wspne/.ssh/ftpflow_test_rsa";
        const std::string pem = readAll(keyPath);
        if (pem.empty() || !tcpProbe("127.0.0.1", 22, 300)) {
            std::printf("[info] no local sshd + key -> skipping Part C\n");
        } else {
            // One big compressible file rides the SSH Compress pipeline
            // (mkdir -p through SFTP + the zdecode agent template).
            const std::string big = root + "/big.txt";
            {
                std::string data;
                const std::string blk = "tree walk compress leg filler ";
                for (int i = 0; i < 220000; ++i) data += blk;
                writeFile(big, data);
            }

            ftpflow::SshEndpoint ep;
            ep.host = "127.0.0.1";
            ep.user = "wspne";
            ep.password = "-";
            ep.privateKeyPem = pem;
            const std::string agent =
                exeDir() + "\\..\\cli\\ftpcli.exe zdecode";
            const std::string remoteRoot = tmp + "/ftpflow_tw_up";

            {
                ftpflow::TransferQueue q(2, 50);
                Recorded r;
                q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });
                ftpflow::TransferRequest tr;
                tr.protocol = ftpflow::BackendKind::Sftp;
                tr.ssh = ep;
                tr.localPath = root;
                tr.remotePath = remoteRoot;
                tr.pushCommandTemplate = agent + " > {file}";
                std::string err;
                const std::vector<std::uint64_t> ids = q.enqueueTree(tr, &err);
                CHECK(ids.size() == 5);
                CHECK(err.empty());
                q.waitAll();
                CHECK(r.finished.load() == 5);
                CHECK(r.failed.load() == 0);
            }

            {
                const std::string down = tmp + "/ftpflow_tw_sdown";
                rmTree(down);
                ftpflow::TransferQueue q(2, 50);
                Recorded r;
                q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });
                ftpflow::TransferRequest tr;
                tr.protocol = ftpflow::BackendKind::Sftp;
                tr.ssh = ep;
                tr.direction = ftpflow::TransferDirection::Download;
                tr.remotePath = remoteRoot;
                tr.localPath = down;
                std::string err;
                const std::vector<std::uint64_t> ids = q.enqueueTree(tr, &err);
                CHECK(ids.size() == 5);
                q.waitAll();
                CHECK(r.finished.load() == 5);
                CHECK(r.failed.load() == 0);
                CHECK(readAll(down + "/top.txt") == "top-level");
                CHECK(readAll(down + "/a/one.bin") == "11111");
                CHECK(readAll(down + "/a/b/two.bin") == "2222222222");
                CHECK(readAll(down + "/a/b/three.bin") == "333");
                CHECK(readAll(down + "/big.txt") == readAll(big));
                rmTree(down);
            }

            // Cleanup the remote tree through SFTP.
            {
                ftpflow::SftpSession sftp(ftpflow::HostKeyPolicy::AcceptNew);
                std::string ak;
                CHECK(sftp.open(ep, {}, &ak).ok());
                std::vector<ftpflow::TreeEntry> remote;
                CHECK(ftpflow::walkRemoteTree(sftp, remoteRoot, remote));
                // 5 files + a + a/b (big.txt rides the compress leg).
                CHECK(remote.size() == 7);
                for (const auto& e : remote) {
                    const std::string p = ftpflow::joinTreePath(remoteRoot, e.relPath);
                    if (e.isDir) sftp.removeDir(p);
                    else sftp.removeFile(p);
                }
                sftp.removeDir(remoteRoot);
            }
            std::remove((root + "/big.txt").c_str());
        }
    }

    rmTree(root);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL TREE-WALK TESTS PASSED\n");
    return g_failed == 0 ? 0 : 1;
}
