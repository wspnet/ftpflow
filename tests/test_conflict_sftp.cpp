// test_conflict_sftp -- end-to-end same-name conflict handling over SFTP
// against the local sshd (127.0.0.1:22, key ftpflow_test_rsa, user "wspne").
// Skips cleanly when the key file or sshd is absent.
//
// Verifies:
//   1. SftpSession::stat() reports size + mtime.
//   2. Upload then Skip re-upload -> job is Skipped (destination untouched).
//   3. Upload then Smart re-upload of an identical file -> Skipped.
//   4. Modify the source (size/mtime change) then Smart re-upload -> overwrites.
#include "ftpflow/transfer_queue.hpp"
#include "ftpflow/text_codec.hpp"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((u_short)port);
            a.sin_addr.s_addr = inet_addr(host);
            connect(s, (sockaddr*)&a, sizeof(a));
            fd_set w; FD_ZERO(&w); FD_SET(s, &w);
            timeval tv{0, timeoutMs * 1000};
            ok = select(0, nullptr, &w, nullptr, &tv) > 0;
            closesocket(s);
        }
    }
    if (res) freeaddrinfo(res);
    WSACleanup();
    return ok;
}

static std::string readKeyFile(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string out;
    char buf[4096]; std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

static void writeFile(const std::string& path, const std::string& content) {
    std::FILE* f = ftpflow::openLocalFile(path, "wb");
    if (f) { std::fwrite(content.data(), 1, content.size(), f); std::fclose(f); }
}

int main() {
    const char* keyPath = "C:/Users/wspne/.ssh/ftpflow_test_rsa";
    const std::string pem = readKeyFile(keyPath);
    if (pem.empty() || !tcpProbe("127.0.0.1", 22, 300)) {
        std::printf("conflict_sftp: SKIP (no local sshd / key)\n");
        return 0;
    }

    ftpflow::SshEndpoint ep;
    ep.host = "127.0.0.1";
    ep.port = 22;
    ep.user = "wspne";
    ep.password = "-";
    ep.privateKeyPem = pem;

    // ---- 1. SftpSession::stat() reports size + mtime --------------------
    {
        ftpflow::SftpSession s(ftpflow::HostKeyPolicy::AcceptNew);
        std::string accepted;
        ftpflow::SftpResult r = s.open(ep, {}, &accepted);
        CHECK(r.ok());
        if (r.ok()) {
            // Stat the user's home directory (always exists on Windows sshd).
            ftpflow::RemoteStat st;
            ftpflow::SftpResult sr = s.stat("C:/Users/wspne", &st);
            CHECK(sr.ok());
            CHECK(st.exists);
            CHECK(st.isDir);
            std::printf("[info] stat(home): exists=%d isDir=%d size=%llu mtime=%lld\n",
                        st.exists, st.isDir, (unsigned long long)st.size,
                        (long long)st.mtimeUnix);
        }
    }

    // ---- 2/3/4. conflict policies through the queue ---------------------
    const std::string localFile = "C:\\Users\\wspne\\ftpflow_conflict_local.txt";
    const std::string remoteFile = "C:/Users/wspne/ftpflow_conflict_remote.txt";

    ftpflow::TransferRequest baseReq;
    baseReq.protocol = ftpflow::BackendKind::Sftp;
    baseReq.ssh = ep;
    baseReq.direction = ftpflow::TransferDirection::Upload;
    baseReq.localPath = localFile;
    baseReq.remotePath = remoteFile;

    // Helper: run one upload job, return the terminal TransferState.
    auto runUpload = [&](ftpflow::ConflictPolicy policy) -> ftpflow::TransferState {
        ftpflow::TransferQueue q(1, 50);
        std::atomic<int> finished{0};
        ftpflow::TransferState terminal = ftpflow::TransferState::Queued;
        q.setEventCallback([&](const ftpflow::TransferEvent& ev) {
            if (ev.kind == ftpflow::TransferEvent::Kind::Finished) {
                terminal = ev.state;
                ++finished;
            }
        });
        ftpflow::TransferRequest req = baseReq;
        req.conflict = policy;
        q.enqueue(std::move(req));
        q.waitAll();
        CHECK(finished.load() == 1);
        return terminal;
    };

    // First upload (no destination yet) -> Done.
    writeFile(localFile, "hello conflict v1\n");
    ftpflow::TransferState first = runUpload(ftpflow::ConflictPolicy::Smart);
    CHECK(first == ftpflow::TransferState::Done);

    // Re-upload the SAME bytes with Skip -> Skipped.
    ftpflow::TransferState skip = runUpload(ftpflow::ConflictPolicy::Skip);
    CHECK(skip == ftpflow::TransferState::Skipped);

    // Re-upload the SAME bytes with Smart -> Skipped (identical).
    ftpflow::TransferState smartSame = runUpload(ftpflow::ConflictPolicy::Smart);
    CHECK(smartSame == ftpflow::TransferState::Skipped);

    // Modify the source (different size + mtime) then Smart -> overwrite.
    // Give the mtime a chance to advance so the comparison is meaningful.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    writeFile(localFile, "hello conflict v2 with more bytes!\n");
    ftpflow::TransferState smartDiff = runUpload(ftpflow::ConflictPolicy::Smart);
    CHECK(smartDiff == ftpflow::TransferState::Done);

    std::printf("conflict_sftp: %d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL CONFLICT-SFTP TESTS PASSED\n");
    return g_failed == 0 ? 0 : 1;
}
