// test_ftp_mlsd -- MLSD (RFC 3659) listing parsing + end-to-end FTP list.
//
// Part A (always runs): pure parseMlsdLine / parseMlsdTimestamp unit checks
// (fact ordering, types, sizes, modify timestamps, malformed lines).
//
// Part B (runs when the dev pyftpdlib is up on 127.0.0.1:2121): a real
// FtpSession::list() should prefer MLSD, so entries carry accurate mtime
// (LIST text has no reliable date) and capabilities().structuredListing
// flips true after the first listing.
#include "ftpflow/ftp_session.hpp"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <string>
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
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            timeval tv{0, timeoutMs * 1000};
            ok = select(0, nullptr, &w, nullptr, &tv) > 0;
            closesocket(s);
        }
        freeaddrinfo(res);
    }
    WSACleanup();
    return ok;
}

int main() {
    // ---- Part A: pure MLSD parsing --------------------------------------
    {
        // Well-formed file entry with size + modify.
        Entry e;
        CHECK(parseMlsdLine("type=file;size=123;modify=20260116070000; file.txt", e));
        CHECK(e.name == "file.txt");
        CHECK(e.type == EntryType::File);
        CHECK(e.size == 123);
        CHECK(e.mtimeUnix != 0);

        // Directory entry.
        Entry d;
        CHECK(parseMlsdLine("type=dir;modify=20260116070000; subdir", d));
        CHECK(d.name == "subdir");
        CHECK(d.type == EntryType::Directory);

        // Facts in any order; UNIX.mode parsed as octal.
        Entry m;
        CHECK(parseMlsdLine("modify=20200102030405;UNIX.mode=0755;type=file;size=7; x",
                            m));
        CHECK(m.type == EntryType::File);
        CHECK(m.size == 7);
        CHECK(m.permissions == 0755);

        // Filename containing spaces.
        Entry sp;
        CHECK(parseMlsdLine("type=file;size=1; a name with spaces.txt", sp));
        CHECK(sp.name == "a name with spaces.txt");

        // cdir/pdir pseudo-entries (name "." / "..") are rejected like LIST.
        Entry cdir;
        CHECK(!parseMlsdLine("type=cdir;modify=20260116070000; .", cdir));
        CHECK(!parseMlsdLine("type=pdir;modify=20260116070000; ..", cdir));

        // Malformed: no "; " separator -> reject.
        Entry bad;
        CHECK(!parseMlsdLine("type=file;size=1;notmlsd", bad));
        CHECK(!parseMlsdLine("garbage line", bad));
        CHECK(!parseMlsdLine("", bad));
        // LIST text (starts with '-') has no "; " -> reject.
        CHECK(!parseMlsdLine("-rw-r--r-- 1 u g 123 Jan 1 00:00 x", bad));
    }

    // ---- timestamp parsing (UTC) ----------------------------------------
    {
        // 1970-01-01 00:00:00 -> 0.
        CHECK(parseMlsdTimestamp("19700101000000") == 0);
        // 1970-01-01 00:00:01 -> 1.
        CHECK(parseMlsdTimestamp("19700101000001") == 1);
        // 1970-01-01 01:00:00 -> 3600.
        CHECK(parseMlsdTimestamp("19700101010000") == 3600);
        // 2000-01-01 00:00:00 -> 946684800 (well-known epoch).
        CHECK(parseMlsdTimestamp("20000101000000") == 946684800);
        // Fractional seconds are ignored after the 14th char.
        CHECK(parseMlsdTimestamp("20000101000000.123") == 946684800);
        // Malformed -> 0.
        CHECK(parseMlsdTimestamp("") == 0);
        CHECK(parseMlsdTimestamp("20000101") == 0);          // too short
        CHECK(parseMlsdTimestamp("20001301000000") == 0);    // month 13
        CHECK(parseMlsdTimestamp("200001010000xx") == 0);    // non-digit
    }

    // ---- Part B: end-to-end FTP list via MLSD (needs pyftpdlib) ----------
    if (tcpProbe("127.0.0.1", 2121, 500)) {
        FtpSession f;
        FtpOptions opt;
        opt.host = "127.0.0.1";
        opt.port = 2121;
        opt.user = "tester";
        opt.password = "secret";
        opt.initialPath = "/";
        FtpResult r = f.open(opt);
        CHECK(r.ok());
        if (r.ok()) {
            // Initial capability is unprobed (false until first MLSD list).
            CHECK(f.capabilities().structuredListing == false);

            ListingPage p = f.list("/");
            const std::string err = f.lastError();
            CHECK(err.empty());
            CHECK(!p.entries.empty());
            // After a successful MLSD list, structuredListing flips true.
            CHECK(f.capabilities().structuredListing == true);

            // hello.txt exists in the fixture with a nonzero mtime (MLSD's
            // modify fact; LIST text parsing leaves mtime == 0).
            bool sawHello = false;
            for (const Entry& e : p.entries) {
                if (e.name == "hello.txt") {
                    sawHello = true;
                    CHECK(e.type == EntryType::File);
                    CHECK(e.size > 0);
                    CHECK(e.mtimeUnix != 0);   // MLSD supplies modify
                }
                if (e.name == "sub") {
                    CHECK(e.type == EntryType::Directory);
                }
            }
            CHECK(sawHello);
        }
    } else {
        std::printf("[info] pyftpdlib not on 127.0.0.1:2121 -> skip FTP MLSD leg\n");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
