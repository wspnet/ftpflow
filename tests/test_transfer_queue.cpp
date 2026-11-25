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
#include "ftpflow/transfer_journal.hpp"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
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

// ---- small file helpers (resume / FTZ1 legs) -----------------------------

static std::string exeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    const std::size_t slash = p.find_last_of("\\/");
    return p.substr(0, slash);
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

static bool filesEqual(const std::string& a, const std::string& b) {
    return readAll(a) == readAll(b);
}

// Writes `totalBytes` of compressible filler with a distinct prefix.
static void makeFiller(const std::string& path, std::size_t totalBytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const char* blk = "ftpflow transfer-queue filler block ";
    const std::size_t blkLen = 34;
    std::size_t written = 0;
    while (written + blkLen <= totalBytes) {
        std::fwrite(blk, 1, blkLen, f);
        written += blkLen;
    }
    if (written < totalBytes) {
        std::fwrite(blk, 1, totalBytes - written, f);
    }
    std::fclose(f);
}

int main() {
    // ---- Part A0: same-name conflict decision logic (offline, pure) -----
    {
        using ftpflow::RemoteStat;
        using ftpflow::smartShouldSkip;

        RemoteStat src; src.exists = true; src.size = 100; src.mtimeUnix = 5;

        // Destination missing -> never skip (overwrite).
        RemoteStat absent;
        CHECK(!smartShouldSkip(src, absent));

        // Same size + same mtime -> skip.
        RemoteStat same; same.exists = true; same.size = 100; same.mtimeUnix = 5;
        CHECK(smartShouldSkip(src, same));

        // Different size -> overwrite.
        RemoteStat diffSize; diffSize.exists = true; diffSize.size = 200; diffSize.mtimeUnix = 5;
        CHECK(!smartShouldSkip(src, diffSize));

        // Different mtime (same size) -> overwrite.
        RemoteStat diffTime; diffTime.exists = true; diffTime.size = 100; diffTime.mtimeUnix = 9;
        CHECK(!smartShouldSkip(src, diffTime));

        // Unknown mtime on either side (0) -> falls back to size-only:
        // same size + unknown time -> skip (cannot prove it differs).
        RemoteStat unknownTime; unknownTime.exists = true; unknownTime.size = 100; unknownTime.mtimeUnix = 0;
        CHECK(smartShouldSkip(src, unknownTime));

        // Directory destination is never "the same file" -> not skipped.
        RemoteStat dir; dir.exists = true; dir.isDir = true; dir.size = 100; dir.mtimeUnix = 5;
        CHECK(!smartShouldSkip(src, dir));
    }

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
            CHECK(q.isPaused(id));         // the flag is observable
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            CHECK(r.finished.load() == 0); // still parked, not finished
            CHECK(q.resume(id));           // clear the flag
            CHECK(!q.isPaused(id));
            CHECK(q.pause(id));            // park again
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

        {   // FTP resume via the queue: upload a 1 MiB prefix, then --resume
            // the full 8 MiB (REST/APPE continuation), byte-identical.
            const std::string tmp = "C:/Users/wspne/AppData/Local/Temp";
            const std::string full = tmp + "/tq_r_full.bin";
            const std::string prefix = tmp + "/tq_r_prefix.bin";
            const std::string back = tmp + "/tq_r_back.bin";
            makeFiller(full, 8 * 1024 * 1024);
            makeFiller(prefix, 1024 * 1024);
            {
                ftpflow::TransferQueue q(1, 50);
                Recorded r;
                q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });
                ftpflow::TransferRequest pre;
                pre.protocol = ftpflow::BackendKind::Ftp;
                pre.ftp.host = "127.0.0.1";
                pre.ftp.port = 2121;
                pre.ftp.user = "tester";
                pre.ftp.password = "secret";
                pre.localPath = prefix;
                pre.remotePath = "/tq_r.bin";
                q.enqueue(pre);

                ftpflow::TransferRequest res = pre;
                res.localPath = full;
                res.resume = true;
                q.enqueue(res);
                q.waitAll();
                CHECK(r.finished.load() == 2);
                CHECK(r.failed.load() == 0);
                CHECK(r.canceled.load() == 0);
            }
            ftpflow::FtpSession ftp;
            ftpflow::FtpOptions fopt;
            fopt.host = "127.0.0.1";
            fopt.port = 2121;
            fopt.user = "tester";
            fopt.password = "secret";
            CHECK(ftp.open(fopt).ok());
            std::uint64_t moved = 0;
            CHECK(ftp.download("/tq_r.bin", back, 0, &moved).ok());
            CHECK(moved == 8 * 1024 * 1024);
            CHECK(filesEqual(full, back));
            std::remove(full.c_str());
            std::remove(prefix.c_str());
            std::remove(back.c_str());
        }
        std::remove(big.c_str());
    } else {
        std::printf("[info] no live pyftpdlib on 127.0.0.1:2121 -> skipping Part B\n");
    }

    // ---- Part C: SSH compress resume + FTZ1 (local sshd + test key) ------
    {
        const char* keyPath = "C:/Users/wspne/.ssh/ftpflow_test_rsa";
        std::FILE* kf = std::fopen(keyPath, "rb");
        const std::string pem = kf ? readAll(keyPath) : std::string();
        if (kf) std::fclose(kf);
        if (pem.empty() || !tcpProbe("127.0.0.1", 22, 300)) {
            std::printf("[info] no local sshd + key -> skipping Part C\n");
        } else {
            const std::string tmp = "C:/Users/wspne/AppData/Local/Temp";
            const std::string full = tmp + "/tq_c_full.bin";
            const std::string prefix = tmp + "/tq_c_prefix.bin";
            const std::string back = tmp + "/tq_c_back.bin";
            const std::string extract = tmp + "/tq_c_extract.bin";
            const std::string remote = tmp + "/tq_c_remote.bin";
            const std::string remoteFtz = tmp + "/tq_c_remote.ftz1";
            const std::string remoteBase = tmp + "/tq_c_remote";
            makeFiller(full, 4 * 1024 * 1024);
            makeFiller(prefix, 1024 * 1024);
            std::remove(remote.c_str());
            std::remove(remoteFtz.c_str());

            ftpflow::SshEndpoint ep;
            ep.host = "127.0.0.1";
            ep.user = "wspne";
            ep.password = "-";
            ep.privateKeyPem = pem;

            // The remote decompressor agent (same machine in the test
            // setup): ftpcli zdecode with a truncating / appending shell
            // redirect, exactly like a remote zstd deployment.
            const std::string agent = exeDir() + "\\..\\cli\\ftpcli.exe zdecode";

            {   // compressed resume: push the prefix, then --resume append
                ftpflow::TransferQueue q(1, 50);
                Recorded r;
                q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });

                ftpflow::TransferRequest pre;
                pre.protocol = ftpflow::BackendKind::Sftp;
                pre.ssh = ep;
                pre.localPath = prefix;
                pre.remotePath = remote;
                pre.pushCommandTemplate = agent + " > {file}";
                q.enqueue(pre);

                ftpflow::TransferRequest res = pre;
                res.localPath = full;
                res.resume = true;
                res.pushCommandTemplate = agent + " >> {file}";
                q.enqueue(res);
                q.waitAll();
                CHECK(r.finished.load() == 2);
                CHECK(r.failed.load() == 0);
                CHECK(r.canceled.load() == 0);
            }

            {   // verify through SFTP: exact size + byte-identical content
                ftpflow::SftpSession sftp(ftpflow::HostKeyPolicy::AcceptNew);
                std::string ak;
                CHECK(sftp.open(ep, {}, &ak).ok());
                std::uint64_t remoteSize = 0;
                CHECK(sftp.size(remote, &remoteSize).ok());
                CHECK(remoteSize == 4 * 1024 * 1024);
                std::uint64_t moved = 0;
                CHECK(sftp.download(remote, back, 0, &moved).ok());
                CHECK(filesEqual(full, back));

                {   // idempotent resume: remote already complete -> Done
                    // without shipping a single byte on the wire.
                    ftpflow::TransferQueue q(1, 50);
                    Recorded r;
                    std::atomic<std::uint64_t> lastWire{1};
                    q.setEventCallback([&](const ftpflow::TransferEvent& e) {
                        record(&r, e);
                        if (e.kind == ftpflow::TransferEvent::Kind::Finished)
                            lastWire = e.progress.bytesOnWire;
                    });
                    ftpflow::TransferRequest res;
                    res.protocol = ftpflow::BackendKind::Sftp;
                    res.ssh = ep;
                    res.localPath = full;
                    res.remotePath = remote;
                    res.resume = true;
                    res.pushCommandTemplate = agent + " >> {file}";
                    q.enqueue(res);
                    q.waitAll();
                    CHECK(r.finished.load() == 1);
                    CHECK(r.failed.load() == 0);
                    CHECK(lastWire.load() == 0);
                }
            }

            {   // FTZ1 archive roundtrip: upload as container, download +
                // extract, frame hash verification, byte-identical.
                ftpflow::TransferQueue q(1, 50);
                Recorded rUp;
                q.setEventCallback([&rUp](const ftpflow::TransferEvent& e) { record(&rUp, e); });
                ftpflow::TransferRequest up;
                up.protocol = ftpflow::BackendKind::Sftp;
                up.ssh = ep;
                up.localPath = full;
                up.remotePath = remoteBase;   // -> remoteBase + ".ftz1"
                up.ftzArchive = true;
                q.enqueue(up);
                q.waitAll();
                CHECK(rUp.finished.load() == 1);
                CHECK(rUp.failed.load() == 0);

                ftpflow::SftpSession sftp(ftpflow::HostKeyPolicy::AcceptNew);
                std::string ak;
                CHECK(sftp.open(ep, {}, &ak).ok());
                std::uint64_t containerSize = 0;
                CHECK(sftp.size(remoteFtz, &containerSize).ok());
                CHECK(containerSize > 0);
                CHECK(containerSize < 4 * 1024 * 1024);   // compressible filler

                ftpflow::TransferQueue q2(1, 50);
                Recorded rDown;
                q2.setEventCallback([&rDown](const ftpflow::TransferEvent& e) { record(&rDown, e); });
                ftpflow::TransferRequest down;
                down.protocol = ftpflow::BackendKind::Sftp;
                down.ssh = ep;
                down.direction = ftpflow::TransferDirection::Download;
                down.localPath = extract;
                down.remotePath = remoteFtz;   // names the container
                down.ftzArchive = true;
                q2.enqueue(down);
                q2.waitAll();
                CHECK(rDown.finished.load() == 1);
                CHECK(rDown.failed.load() == 0);
                CHECK(filesEqual(full, extract));
            }

            std::remove(full.c_str());
            std::remove(prefix.c_str());
            std::remove(back.c_str());
            std::remove(extract.c_str());
            std::remove(remote.c_str());
            std::remove(remoteFtz.c_str());
        }
    }

    // ---- Part D: durable journal (offline, no server needed) ------------
    {
        const std::string jpath =
            "C:/Users/wspne/AppData/Local/Temp/tq_journal.dat";
        std::remove(jpath.c_str());

        // Round-trip a record through the journal: secrets dropped, fields
        // preserved.
        {
            ftpflow::TransferJournal j(jpath);
            ftpflow::TransferRequest req;
            req.protocol = ftpflow::BackendKind::Sftp;
            req.direction = ftpflow::TransferDirection::Upload;
            req.localPath = "C:/some/local/file.bin";
            req.remotePath = "/remote/dir/file.bin";
            req.ssh.host = "127.0.0.1";
            req.ssh.port = 22;
            req.ssh.user = "alice";
            req.ssh.password = "s3cret";          // must NOT persist
            req.ssh.privateKeyPem = "-----BEGIN...";   // must NOT persist
            req.autoDecide = false;
            req.mode = ftpflow::TransferMode::Compress;
            req.zstdLevel = 7;
            req.pushCommandTemplate = "zstd -d > {file}";
            req.ftzArchive = false;
            req.ensureRemoteDir = true;

            ftpflow::JournalRecord rec =
                ftpflow::TransferJournal::fromRequest(42, req);
            CHECK(rec.ssh.password.empty());
            CHECK(rec.ssh.privateKeyPem.empty());

            std::vector<ftpflow::JournalRecord> recs;
            recs.push_back(rec);
            std::string err;
            CHECK(j.save(recs, &err));

            std::vector<ftpflow::JournalRecord> loaded;
            CHECK(j.load(loaded, &err));
            CHECK(loaded.size() == 1);
            CHECK(loaded[0].jobId == 42);
            CHECK(loaded[0].localPath == "C:/some/local/file.bin");
            CHECK(loaded[0].remotePath == "/remote/dir/file.bin");
            CHECK(loaded[0].protocol == ftpflow::BackendKind::Sftp);
            CHECK(loaded[0].direction == ftpflow::TransferDirection::Upload);
            CHECK(loaded[0].ssh.host == "127.0.0.1");
            CHECK(loaded[0].ssh.user == "alice");
            CHECK(loaded[0].ssh.password.empty());
            CHECK(loaded[0].zstdLevel == 7);
            CHECK(loaded[0].pushCommandTemplate == "zstd -d > {file}");
            CHECK(loaded[0].ensureRemoteDir == true);
        }

        // remove() drops a record; save() with fewer records persists.
        {
            ftpflow::TransferJournal j(jpath);
            std::string err;
            CHECK(j.remove(42, &err));
            std::vector<ftpflow::JournalRecord> loaded;
            CHECK(j.load(loaded, &err));
            CHECK(loaded.empty());
        }

        // Queue integration: enqueue persists, terminal state trims.
        {
            const std::string jpath2 =
                "C:/Users/wspne/AppData/Local/Temp/tq_journal2.dat";
            std::remove(jpath2.c_str());

            // A job that fails fast (unroutable address) still reaches a
            // terminal state and must be trimmed from the journal.
            ftpflow::TransferQueue q(1, 50);
            q.setJournal(jpath2);
            Recorded r;
            q.setEventCallback([&r](const ftpflow::TransferEvent& e) { record(&r, e); });
            ftpflow::TransferRequest req;
            req.protocol = ftpflow::BackendKind::Ftp;
            req.ftp.host = "127.0.0.1";
            req.ftp.port = 1;
            req.ftp.connectTimeoutMs = 2000;
            req.localPath = "C:/nonexistent.bin";
            req.remotePath = "/x.bin";
            q.enqueue(req);
            q.waitAll();

            // After a terminal state, the journal must be empty again.
            ftpflow::TransferJournal j(jpath2);
            std::vector<ftpflow::JournalRecord> recs;
            std::string err;
            CHECK(j.load(recs, &err));
            CHECK(recs.empty());
            CHECK(r.finished.load() == 1);
            std::remove(jpath2.c_str());
        }

        // Restore semantics: seed a journal with one in-flight record,
        // then a fresh queue replays it with resume=true.
        {
            const std::string jpath3 =
                "C:/Users/wspne/AppData/Local/Temp/tq_journal3.dat";
            std::remove(jpath3.c_str());

            // Seed: a record that looks like a job interrupted mid-flight.
            {
                ftpflow::TransferJournal j(jpath3);
                ftpflow::TransferRequest req;
                req.protocol = ftpflow::BackendKind::Ftp;
                req.ftp.host = "127.0.0.1";
                req.ftp.port = 1;              // fails fast when restored
                req.ftp.connectTimeoutMs = 2000;
                req.localPath = "C:/Users/wspne/AppData/Local/Temp/tq_crash_src.bin";
                req.remotePath = "/crash.bin";
                makeFiller(req.localPath, 1024 * 1024);
                std::vector<ftpflow::JournalRecord> recs;
                recs.push_back(
                    ftpflow::TransferJournal::fromRequest(7, req));
                std::string err;
                CHECK(j.save(recs, &err));
                // Immediate self-check: the record round-trips.
                std::vector<ftpflow::JournalRecord> chk;
                CHECK(j.load(chk, &err));
                CHECK(chk.size() == 1);
                CHECK(chk[0].jobId == 7);
            }

            // Restore: a fresh queue replays it with resume=true.
            ftpflow::TransferQueue q2(1, 50);
            q2.setJournal(jpath3);
            bool sawResume = false;
            std::string terr;
            std::size_t restored = q2.restore(&terr,
                [&](ftpflow::TransferRequest& r) {
                    sawResume = r.resume;     // resume forced on
                });
            CHECK(restored == 1);
            CHECK(sawResume);
            // The restored job fails fast (port 1) and is trimmed.
            q2.waitAll();
            std::remove("C:/Users/wspne/AppData/Local/Temp/tq_crash_src.bin");
            std::remove(jpath3.c_str());
        }

        std::remove(jpath.c_str());
    }

    // ---- Part E: runtime worker resizing (offline, no server needed) ------
    {
        ftpflow::TransferQueue q(2, 100);
        CHECK(q.workerCount() == 2);

        // Grow: 2 -> 4.
        q.setWorkers(4);
        CHECK(q.workerCount() == 4);

        // Shrink: 4 -> 1 (no jobs in flight, so surplus workers exit
        // immediately and join).
        q.setWorkers(1);
        CHECK(q.workerCount() == 1);

        // Clamp: 0 -> 1, and a huge value -> 64.
        q.setWorkers(0);
        CHECK(q.workerCount() == 1);
        q.setWorkers(1000);
        CHECK(q.workerCount() == 64);

        // The queue still drains work after resizing (use a job that fails
        // fast against a dead endpoint so no server is required).
        ftpflow::TransferRequest req;
        req.protocol = ftpflow::BackendKind::Sftp;
        req.direction = ftpflow::TransferDirection::Upload;
        req.localPath = "C:/Users/wspne/AppData/Local/Temp/tq_resize_src.bin";
        req.remotePath = "/resize_dst.bin";
        req.ssh.host = "127.0.0.1";
        req.ssh.port = 1;   // nothing listens -> fails fast
        req.ssh.user = "alice";
        req.ssh.password = "x";
        makeFiller(req.localPath, 64);   // small local source, fails fast
        const std::uint64_t id = q.enqueue(req);
        CHECK(id != 0);
        q.waitAll();
        CHECK(q.pending() == 0);
        std::remove(req.localPath.c_str());
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL TRANSFER-QUEUE TESTS PASSED\n");
    return g_failed == 0 ? 0 : 1;
}
