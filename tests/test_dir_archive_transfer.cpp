// test_dir_archive_transfer -- end-to-end directory-archive transfer against
// the local sshd (127.0.0.1:22, key ftpflow_test_rsa, user "wspne"). Skips
// cleanly when sshd or the key is unavailable.
//
// Upload: pack a local folder -> SFTP the .tar.zst -> remote `tar --zstd -xf`
//         extracts it -> verify the remote tree is intact.
// Download: remote `tar --zstd -cf` packs a folder -> SFTP it down -> local
//         extract -> verify the local tree.
#include "ftpflow/transfer_queue.hpp"
#include "ftpflow/text_codec.hpp"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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
            fd_set w; FD_ZERO(&w); FD_SET(s, &w);
            timeval tv{0, timeoutMs * 1000};
            ok = select(0, nullptr, &w, nullptr, &tv) > 0;
            closesocket(s);
        }
        freeaddrinfo(res);
    }
    WSACleanup();
    return ok;
}

static std::string readKeyFile(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string s;
    char buf[4096]; std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

static bool writeFile(const std::string& path, const std::string& content) {
    std::FILE* f = openLocalFile(path, "wb");
    if (!f) return false;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return true;
}

static bool readFile(const std::string& path, std::string& out) {
    std::FILE* f = openLocalFile(path, "rb");
    if (!f) return false;
    char buf[4096]; std::size_t n; out.clear();
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

int main() {
    const char* keyPath = "C:/Users/wspne/.ssh/ftpflow_test_rsa";
    const std::string pem = readKeyFile(keyPath);
    if (pem.empty() || !tcpProbe("127.0.0.1", 22, 300)) {
        std::printf("dir_archive_transfer: SKIP (no local sshd / key)\n");
        return 0;
    }

    SshEndpoint ep;
    ep.host = "127.0.0.1";
    ep.port = 22;
    ep.user = "wspne";
    ep.password = "-";
    ep.privateKeyPem = pem;

    // Local test tree under the user's temp.
    const std::string base = "C:\\Users\\wspne\\ftpflow_dirarchive_xfer";
    const std::string srcDir = base + "\\src";
    const std::string dlDir = base + "\\dl";
    createDirUtf8(base);
    createDirUtf8(srcDir);
    createDirUtf8(srcDir + "\\sub");
    createDirUtf8(dlDir);
    writeFile(srcDir + "\\a.txt", "alpha\n");
    writeFile(srcDir + "\\sub\\b.txt", "beta content\n");

    // Remote paths (wspne home on the local sshd).
    const std::string remoteRoot = "C:\\Users\\wspne\\ftpflow_dirarchive_remote";
    const std::string remoteUp = remoteRoot + "\\up";
    const std::string remoteDl = remoteRoot + "\\down";
    // Convert to forward-slash SSH-style paths for exec (OpenSSH on Windows
    // accepts forward slashes).
    auto fwd = [](const std::string& p) {
        std::string s = p;
        for (auto& c : s) if (c == '\\') c = '/';
        return s;
    };
    const std::string remoteUpFwd = fwd(remoteUp);
    const std::string remoteDlFwd = fwd(remoteDl);

    TransferQueue q(1, 50);
    std::atomic<int> finished = 0;
    std::string lastError;
    q.setEventCallback([&](const TransferEvent& ev) {
        if (ev.kind == TransferEvent::Kind::Finished) {
            finished = 1;
            lastError = ev.error;
        }
    });

    // ---- Upload: archive srcDir -> remoteUp (auto-extract) --------------
    TransferRequest up;
    up.protocol = BackendKind::Sftp;
    up.ssh = ep;
    up.direction = TransferDirection::Upload;
    up.localPath = srcDir;
    up.remotePath = remoteUpFwd;
    up.dirArchive = true;
    q.enqueue(up);
    q.waitAll();
    CHECK(finished.load() == 1);
    // Remote tree should exist (extracted by the remote tar).
    {
        std::string c;
        const bool a = readFile(remoteUp + "\\a.txt", c) && c == "alpha\n";
        const bool b = readFile(remoteUp + "\\sub\\b.txt", c) && c == "beta content\n";
        CHECK(a);
        CHECK(b);
    }

    // ---- Download: archive remoteUp -> local dlDir ----------------------
    finished = 0;
    TransferRequest dn;
    dn.protocol = BackendKind::Sftp;
    dn.ssh = ep;
    dn.direction = TransferDirection::Download;
    dn.localPath = dlDir;
    dn.remotePath = remoteUpFwd;
    dn.dirArchive = true;
    q.enqueue(dn);
    q.waitAll();
    CHECK(finished.load() == 1);
    {
        std::string c;
        const bool a = readFile(dlDir + "\\a.txt", c) && c == "alpha\n";
        const bool b = readFile(dlDir + "\\sub\\b.txt", c) && c == "beta content\n";
        CHECK(a);
        CHECK(b);
    }

    q.shutdown();
    std::printf("dir_archive_transfer: %d checks, %d failed (lastError=%s)\n",
                g_checks, g_failed, lastError.c_str());
    return g_failed == 0 ? 0 : 1;
}
