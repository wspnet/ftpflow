// ftpcli -- ftpflow command line tool.
// Shares core.lib with the desktop shell: what the CLI can do, the GUI can
// do, and vice versa (blueprint: one core, two front ends).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ftpflow/compress_decide.hpp"
#include "ftpflow/protocol_status.hpp"
#include "ftpflow/ssh_exec.hpp"
#include "ftpflow/version.hpp"
#include "ftpflow/vfs_local.hpp"

namespace {

int cmdVersion() {
    std::printf("ftpflow %s (ftpcli, core.lib shared)\n", ftpflow::versionString());
    const auto st = ftpflow::protocolEngineStatus();
    std::printf("engines: ftp=%d ssh=%d zstd=%d exec-compression=%d\n",
        st.ftpAvailable, st.sshAvailable, st.zstdAvailable,
        st.execChannelCompression);
    return 0;
}

ftpflow::FileProbe probeFile(const std::string& path, bool withSample) {
    ftpflow::FileProbe probe;
    probe.fileName = path;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return probe;

    char nameOnly[512];
    const char* slash = std::strrchr(path.c_str(), '\\');
    const char* slash2 = std::strrchr(path.c_str(), '/');
    const char* last = slash && slash2 ? (slash > slash2 ? slash : slash2)
                     : (slash ? slash : (slash2 ? slash2 : nullptr));
    std::snprintf(nameOnly, sizeof(nameOnly), "%s", last ? last + 1 : path.c_str());
    probe.fileName = nameOnly;

    std::uint8_t head[32] = {};
    std::size_t n = std::fread(head, 1, sizeof(head), f);
    probe.head.assign(head, head + n);

    if (withSample) {
        // head/mid/tail 3x64 KiB sampling (stage 3 feed).
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        probe.fileSize = size > 0 ? static_cast<std::uint64_t>(size) : 0;
        constexpr long kWin = 64 * 1024;
        long mid = size / 2;
        long tail = size > kWin ? size - kWin : 0;
        for (long off : {0L, mid, tail}) {
            if (off < 0 || off >= size) continue;
            std::fseek(f, off, SEEK_SET);
            std::vector<std::uint8_t> buf(kWin);
            std::size_t got = std::fread(buf.data(), 1, kWin, f);
            probe.sample.insert(probe.sample.end(), buf.data(), buf.data() + got);
        }
    }
    std::fclose(f);
    return probe;
}

int cmdDecide(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr,
            "usage: ftpcli decide [--bw MIBPS] <file> [file...]\n"
            "  runs the 5-stage funnel; --bw overrides the link speed used\n"
            "  by the stage-5 cost model (default 20 MiB/s).\n");
        return 2;
    }
    ftpflow::CostModel cm;
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--bw" && i + 1 < args.size()) {
            cm.bandwidthBps = std::atof(args[++i].c_str()) * 1024.0 * 1024.0;
        } else {
            paths.push_back(args[i]);
        }
    }
    if (paths.empty()) {
        std::fprintf(stderr, "decide: no files given\n");
        return 2;
    }
    int rc = 0;
    std::printf("%-28s %-10s %-4s %s\n", "FILE", "VERDICT", "STG", "REASON");
    for (const std::string& path : paths) {
        ftpflow::FileProbe probe = probeFile(path, true);
        ftpflow::Decision d = ftpflow::decideFullFunnel(probe, cm);
        const char* v = d.verdict == ftpflow::Verdict::Compress ? "COMPRESS"
                      : d.verdict == ftpflow::Verdict::Store ? "STORE" : "???";
        std::printf("%-28s %-10s %-4d %s\n", probe.fileName.c_str(), v,
                    d.stage, d.reason.c_str());
        if (probe.head.empty() && probe.fileSize == 0) rc = 1;
    }
    return rc;
}

int cmdInfo(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "usage: ftpcli info <dir>\n");
        return 2;
    }
    ftpflow::LocalFsBackend fs;
    std::string err;
    if (!fs.open(args[0], &err)) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    auto caps = fs.capabilities();
    std::printf("local backend: mlsd=%d utf8=%d resume=%d exec=%d\n",
        caps.structuredListing, caps.utf8Paths, caps.resume, caps.execChannel);
    ftpflow::ListingPage page = fs.list({});
    std::printf("first page: %zu entries, more=%d\n",
        page.entries.size(), page.hasMore ? 1 : 0);
    for (const auto& e : page.entries) {
        std::printf("  %-32s %10llu  %lld\n", e.name.c_str(),
            static_cast<unsigned long long>(e.size),
            static_cast<long long>(e.mtimeUnix));
    }
    return 0;
}

// ssh exec <host> <user> <password|-> [--key <path>] <command...>
//   Connects, handshakes, accepts the host key on first use (prints its
//   fingerprint), runs `command` on a single exec channel, and streams
//   stdout/stderr back. This is the end-to-end check for the exec-channel
//   compression fast path. A password of "-" plus --key <path> selects
//   private-key auth (PEM/OpenSSH read from disk).
static std::string readWholeFile(const std::string& path) {
    std::string s;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return s;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        s.resize(static_cast<std::size_t>(n));
        std::fread(&s[0], 1, static_cast<std::size_t>(n), f);
    }
    std::fclose(f);
    return s;
}

int cmdSshExec(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        std::fprintf(stderr,
            "usage: ftpcli ssh exec <host> <user> <password|-> [--key <path>] <command...>\n"
            "  runs <command> on one exec channel; first-use host key is\n"
            "  accepted and printed (TOFU) so it can be pinned later.\n"
            "  password '-' + --key <path> selects private-key auth.\n");
        return 2;
    }
    ftpflow::SshEndpoint ep;
    ep.host = args[0];
    ep.user = args[1];
    ep.password = args[2];
    // <host> may carry :port
    const std::size_t colon = ep.host.rfind(':');
    if (colon != std::string::npos && colon + 1 < ep.host.size()) {
        ep.port = static_cast<std::uint16_t>(std::atoi(ep.host.c_str() + colon + 1));
        ep.host = ep.host.substr(0, colon);
    }
    std::vector<std::string> commandParts;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--key" && i + 1 < args.size()) {
            ep.privateKeyPem = readWholeFile(args[++i]);
            if (ep.privateKeyPem.empty()) {
                std::fprintf(stderr, "ssh exec: cannot read key file %s\n", args[i].c_str());
                return 2;
            }
        } else {
            commandParts.push_back(args[i]);
        }
    }
    std::string command;
    for (std::size_t i = 0; i < commandParts.size(); ++i) {
        if (i > 0) command += ' ';
        command += commandParts[i];
    }
    if (command.empty()) {
        std::fprintf(stderr, "ssh exec: no command given\n");
        return 2;
    }

    ftpflow::SshExec ssh(ftpflow::HostKeyPolicy::AcceptNew);
    std::string acceptedKey;

    // Capture remote stdout/stderr, printing each stream as it arrives.
    ssh.setStdoutSink([](const void* p, std::size_t n) {
        std::fwrite(p, 1, n, stdout);
    });
    ssh.setStderrSink([](const void* p, std::size_t n) {
        std::fwrite(p, 1, n, stderr);
    });

    ftpflow::SshResult r = ssh.open(ep, command, {}, &acceptedKey);
    if (!r.ok()) {
        std::fprintf(stderr, "ssh open failed [%s]: %s\n",
            ftpflow::sshStatusName(r.status), r.message.c_str());
        return 1;
    }
    if (!acceptedKey.empty()) {
        std::fprintf(stderr, "host key (TOFU accepted): sha256-blob %s\n",
            acceptedKey.substr(0, 64).c_str());
    }

    // Drain stdout/stderr until the channel closes.
    while (ssh.pump(500)) {
    }
    std::fflush(stdout);
    std::fflush(stderr);

    std::printf("\nexit=%d bytes_read=%llu bytes_written=%llu\n",
        ssh.exitCode(),
        static_cast<unsigned long long>(ssh.stats().bytesRead),
        static_cast<unsigned long long>(ssh.stats().bytesWritten));
    return ssh.exitCode() == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::printf(
            "ftpcli %s\n"
            "  ftpcli version\n"
            "  ftpcli decide [--bw MIBPS] <file...>   run the 5-stage funnel\n"
            "  ftpcli info <dir>                      directory listing smoke test\n"
            "  ftpcli ssh exec <host[:port]> <user> <password> <cmd...>\n",
            ftpflow::versionString());
        return args.empty() ? 2 : 0;
    }
    const std::string& cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "version") return cmdVersion();
    if (cmd == "decide") return cmdDecide(rest);
    if (cmd == "info") return cmdInfo(rest);
    if (cmd == "ssh") {
        if (rest.empty() || rest[0] != "exec") {
            std::fprintf(stderr, "usage: ftpcli ssh exec <host> <user> <password> <cmd...>\n");
            return 2;
        }
        return cmdSshExec(std::vector<std::string>(rest.begin() + 1, rest.end()));
    }
    std::fprintf(stderr, "unknown command: %s (try --help)\n", cmd.c_str());
    return 2;
}
