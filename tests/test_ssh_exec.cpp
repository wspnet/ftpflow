// ftpflow -- SSH exec channel integration test (local sshd + test key).
//
// Verifies the primitive that powers the server-command panel: SshExec runs
// a one-shot command, streams stdout/stderr through sinks, closes stdin so
// stdin-reading commands terminate, and reports the remote exit code.
//
// Part A always runs (feature-probe semantics). Part B runs only when the
// local sshd answers on 127.0.0.1:22 and the test key exists; otherwise it
// prints an [info] line and skips (so CI on a bare box stays green).
#include "ftpflow/ssh_exec.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failed;
        std::fprintf(stderr, "FAIL %s\n", what);
    }
}

std::string readAll(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

bool tcpProbe(const char* host, int port, int timeoutMs) {
#ifdef _WIN32
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
#endif
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == static_cast<SOCKET>(-1)) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = inet_addr(host);
    // Timeout on the connect itself.
    const int to = timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&to), sizeof(to));
    const bool ok = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
#ifdef _WIN32
    closesocket(s);
    WSACleanup();
#else
    close(s);
#endif
    return ok;
}

}  // namespace

int main() {
    // ---- Part A: no-libssh2 builds must fail cleanly, never hang --------
    {
        ftpflow::SshExec ssh;
        ftpflow::SshEndpoint ep;
        ep.host = "localhost";
        ep.user = "nobody";
        ftpflow::SshResult r = ssh.open(ep, "true", {});
        check(!r.ok(), "nobody@localhost must fail cleanly");
#if FTPFLOW_HAVE_LIBSSH2
        check(r.status != ftpflow::SshStatus::EngineUnavailable,
              "libssh2 build must attempt the connection");
#else
        check(r.status == ftpflow::SshStatus::EngineUnavailable,
              "stub build reports engine-unavailable");
#endif
    }

    // ---- Part B: real command execution over the local sshd -------------
    const char* keyPath = "C:/Users/wspne/.ssh/ftpflow_test_rsa";
    const std::string pem = readAll(keyPath);
    if (pem.empty() || !tcpProbe("127.0.0.1", 22, 300)) {
        std::printf("[info] no local sshd + key -> skipping Part B\n");
        std::printf("ssh_exec: %d checks, %d failed\n", g_checks, g_failed);
        return g_failed == 0 ? 0 : 1;
    }

    ftpflow::SshEndpoint ep;
    ep.host = "127.0.0.1";
    ep.user = "wspne";
    ep.password = "-";
    ep.privateKeyPem = pem;

    {   // echo: stdout captured, exit 0
        ftpflow::SshExec ssh(ftpflow::HostKeyPolicy::AcceptNew);
        std::string out, err;
        ssh.setStdoutSink([&](const void* p, std::size_t n) {
            out.append(static_cast<const char*>(p), n);
        });
        ssh.setStderrSink([&](const void* p, std::size_t n) {
            err.append(static_cast<const char*>(p), n);
        });
        std::string ak;
        check(ssh.open(ep, "echo hello-ftpflow", {}, &ak).ok(),
              "open echo channel");
        ssh.sendEof();
        while (ssh.pump(500)) {}
        check(out.find("hello-ftpflow") != std::string::npos, "stdout has payload");
        check(err.empty(), "no stderr for echo");
        check(ssh.exitCode() == 0, "echo exit 0");
    }

    {   // non-zero exit code is reported
        ftpflow::SshExec ssh(ftpflow::HostKeyPolicy::AcceptNew);
        std::string ak;
        check(ssh.open(ep, "exit 7", {}, &ak).ok(), "open exit-7 channel");
        ssh.sendEof();
        while (ssh.pump(500)) {}
        check(ssh.exitCode() == 7, "exit code 7 reported");
    }

    {   // stderr routing: `>&2` redirects to the stderr sink
        ftpflow::SshExec ssh(ftpflow::HostKeyPolicy::AcceptNew);
        std::string out, err;
        ssh.setStdoutSink([&](const void* p, std::size_t n) {
            out.append(static_cast<const char*>(p), n);
        });
        ssh.setStderrSink([&](const void* p, std::size_t n) {
            err.append(static_cast<const char*>(p), n);
        });
        std::string ak;
        check(ssh.open(ep, "echo oops-err 1>&2", {}, &ak).ok(),
              "open stderr channel");
        ssh.sendEof();
        while (ssh.pump(500)) {}
        check(out.empty(), "stdout empty when redirected to stderr");
        check(err.find("oops-err") != std::string::npos, "stderr captured");
    }

    {   // stdin-close: `cat` reads stdin until EOF; sendEof() must make it
        // return rather than hang.
        ftpflow::SshExec ssh(ftpflow::HostKeyPolicy::AcceptNew);
        std::string ak;
        check(ssh.open(ep, "cat", {}, &ak).ok(), "open cat channel");
        ssh.sendEof();
        int spins = 0;
        while (ssh.pump(500)) {
            if (++spins > 20) break;   // safety valve; shouldn't trip
        }
        check(spins <= 20, "cat terminates after stdin EOF (no hang)");
    }

    {   // interactive shell: PTY + shell, write a command, read its echo.
        ftpflow::SshExec ssh(ftpflow::HostKeyPolicy::AcceptNew);
        std::string out;
        ssh.setStdoutSink([&](const void* p, std::size_t n) {
            out.append(static_cast<const char*>(p), n);
        });
        std::string ak;
        check(ssh.openShell(ep, {}, 80, 24, &ak).ok(), "open PTY shell");

        // Drain the initial prompt (may include a banner).
        int spins = 0;
        while (ssh.pump(200)) {
            if (++spins > 50) break;
        }
        check(ssh.isOpen(), "shell stays open after prompt drain");

        // Run a command that echoes an unmistakable marker.
        check(ssh.write("echo __PTY_MARKER__\n", 20).ok(), "write command");
        std::string markerOut;
        spins = 0;
        while (markerOut.find("__PTY_MARKER__") == std::string::npos) {
            if (spins++ > 50) break;
            ssh.pump(200);
            // append the newest chunk into markerOut via a fresh sink is not
            // possible here; instead we poll the accumulated `out`.
            if (out.find("__PTY_MARKER__") != std::string::npos) {
                markerOut = out;
                break;
            }
        }
        check(out.find("__PTY_MARKER__") != std::string::npos,
              "shell echoes command output");

        // Resize must succeed on the open PTY.
        check(ssh.resize(120, 40).ok(), "resize PTY");

        // Close the shell cleanly (exit).
        ssh.write("exit\n", 5);
        spins = 0;
        while (ssh.pump(200) && spins++ < 50) {}
    }

    std::printf("ssh_exec: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
