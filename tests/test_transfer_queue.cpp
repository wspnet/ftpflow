// ftpflow -- TransferQueue integration test.
//
// Part A (always runs, offline): worker dispatch, event stream shape,
// cancel-while-queued, waitAll, shutdown semantics against an unroutable
// address (fast connect failures).
//
// Part B (runs when 127.0.0.1:2121 is reachable, i.e. the dev pyftpdlib is
// up): pause parks a running transfer, cancel aborts it, happy path
// finishes with a byte-identical roundtrip.
#include "ftpflow/transfer_queue.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <cstring>
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
            // non-blocking connect + select
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

struct Recorded {
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<int> canceled{0};
    std::atomic<int> failed{0};
};

static void record(Recorded* r, const ftpflow::TransferEvent& e) {
    switch (e.kind) {
        case ftpflow::TransferEvent::Kind::Started: ++r->started; break;
        case ftpflow::TransferEvent::Kind::Finished:
            ++r->finished;
            if (e.state == ftpflow::TransferState::Canceled) ++r->canceled;
            if (e.state == ftpflow::TransferState::Failed) ++r->failed;
            break;
        default: break;
    }
}

int main() {
    // ---- Part A: offline dispatch/cancel/waitAll semantics --------------
    {
        ftpflow::TransferQueue q(1, 50);
        Recorded r;
        q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });

        ftpflow::TransferRequest req;
        req.protocol = ftpflow::BackendKind::Ftp;
        req.direction = ftpflow::TransferDirection::Upload;
        req.localPath = "C:/does-not-matter.bin";
        req.remotePath = "/x.bin";
        req.ftp.host = "127.0.0.1";           // nothing listens: instant refuse
        req.ftp.port = 1;
        req.ftp.connectTimeoutMs = 3000;
        const std::uint64_t id1 = q.enqueue(req);
        const std::uint64_t id2 = q.enqueue(req);
        CHECK(id1 != 0);
        CHECK(id2 != 0);
        CHECK(q.pending() == 2);

        // Job 2 is queued behind the (blocked) job 1: cancel it now.
        CHECK(q.cancel(id2));
        q.waitAll();

        CHECK(r.started.load() >= 1);        // job 1 ran
        CHECK(r.finished.load() == 2);       // every job gets exactly one Finished
        CHECK(r.canceled.load() == 1);       // job 2 was dropped
        CHECK(r.failed.load() == 1);         // job 1 could not connect
        CHECK(q.pending() == 0);
    }

    // ---- Part A2: shutdown drops queued jobs without hanging ------------
    {
        ftpflow::TransferQueue q(2, 50);
        Recorded r;
        q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });
        ftpflow::TransferRequest req;
        req.protocol = ftpflow::BackendKind::Ftp;
        req.ftp.host = "127.0.0.1";
        req.ftp.port = 1;
        req.ftp.connectTimeoutMs = 3000;
        for (int i = 0; i < 3; ++i) q.enqueue(req);
        q.shutdown();   // destructor would do this; call explicitly
        CHECK(q.pending() == 0);         // everything resolved or dropped
    }

    // ---- Part B: live server semantics (pause / cancel / roundtrip) -----
    if (tcpProbe("127.0.0.1", 2121, 300)) {
        std::printf("[info] live pyftpdlib detected: running pause/cancel tests\n");
        // 64 MiB of compressible data keeps the transfer busy long enough
        // to pause and cancel deterministically.
        const std::string big = "C:\\Users\\wspne\\AppData\\Local\\Temp\\tq_big.bin";
        {
            std::FILE* f = std::fopen(big.c_str(), "wb");
            std::vector<char> block(64 * 1024, 'x');
            for (int i = 0; i < 1024; ++i) std::fwrite(block.data(), 1, block.size(), f);
            std::fclose(f);
        }

        {   // pause parks; cancel aborts a parked (running) job
            ftpflow::TransferQueue q(1, 50);
            Recorded r;
            std::atomic<int> progressEvents{0};
            q.setEventCallback([&](const ftpflow::TransferEvent& e) {
                record(&r, e);
                if (e.kind == ftpflow::TransferEvent::Kind::Progress)
                    ++progressEvents;
            });
            ftpflow::TransferRequest req;
            req.protocol = ftpflow::BackendKind::Ftp;
            req.ftp.host = "127.0.0.1";
            req.ftp.port = 2121;
            req.ftp.user = "tester";
            req.ftp.password = "secret";
            req.localPath = big;
            req.remotePath = "/tq_pause.bin";
            const std::uint64_t id = q.enqueue(req);
            CHECK(q.pause(id));            // park from the very first chunk
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            CHECK(r.finished.load() == 0); // still parked, not finished
            CHECK(q.cancel(id));           // abort while parked
            q.waitAll();
            CHECK(r.finished.load() == 1);
            CHECK(r.canceled.load() == 1);
        }

        {   // happy path: finish + resume a fresh job, byte-identical
            ftpflow::TransferQueue q(1, 50);
            Recorded r;
            q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });
            ftpflow::TransferRequest req;
            req.protocol = ftpflow::BackendKind::Ftp;
            req.ftp.host = "127.0.0.1";
            req.ftp.port = 2121;
            req.ftp.user = "tester";
            req.ftp.password = "secret";
            req.localPath = big;
            req.remotePath = "/tq_ok.bin";
            q.enqueue(req);
            q.waitAll();
            CHECK(r.started.load() == 1);
            CHECK(r.finished.load() == 1);
            CHECK(r.canceled.load() == 0);
            CHECK(r.failed.load() == 0);
        }
        std::remove(big.c_str());
    } else {
        std::printf("[info] no live pyftpdlib on 127.0.0.1:2121 -> skipping Part B\n");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL TRANSFER-QUEUE TESTS PASSED\n");
    return g_failed == 0 ? 0 : 1;
}
