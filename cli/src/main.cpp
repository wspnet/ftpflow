// ftpcli -- ftpflow command line tool.
// Shares core.lib with the desktop shell: what the CLI can do, the GUI can
// do, and vice versa (blueprint: one core, two front ends).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <io.h>
#include <sys/stat.h>

#if defined(FTPFLOW_HAVE_ZSTD)
#include <zstd.h>
#endif

#include "ftpflow/compress_decide.hpp"
#include "ftpflow/ftp_session.hpp"
#include "ftpflow/sftp_session.hpp"
#include "ftpflow/transfer_queue.hpp"
#include "ftpflow/protocol_status.hpp"
#include "ftpflow/ssh_exec.hpp"
#include "ftpflow/ssh_transfer.hpp"
#include "ftpflow/version.hpp"
#include "ftpflow/vfs_local.hpp"
#include "ftpflow/known_hosts.hpp"

namespace {

// ---- host-key verification policy ---------------------------------------
// When empty, connections use AcceptNew (trust-on-first-use without
// persistence) exactly as before. When set (via --known-hosts PATH), the
// store is loaded, host keys are verified strictly, and unknown keys are
// printed as standard SHA256 fingerprints and persisted on first use.
std::string g_knownHostsPath;

// Parse and consume a trailing --known-hosts <path> from `args`, returning
// true if it was present.
bool takeKnownHostsFlag(std::vector<std::string>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--known-hosts" && i + 1 < args.size()) {
            g_knownHostsPath = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            return true;
        }
    }
    return false;
}

// Convert a hex blob to its OpenSSH fingerprint string for display.
std::string fingerprintOfBlobHex(const std::string& hex) {
    return ftpflow::KnownHosts::fingerprintOfHex(hex);
}

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

    const bool verify = !g_knownHostsPath.empty();
    ftpflow::SshExec ssh(verify ? ftpflow::HostKeyPolicy::Strict
                                : ftpflow::HostKeyPolicy::AcceptNew);
    std::string acceptedKey;

    // Strict verification against the known_hosts store when configured.
    std::vector<std::string> trusted;
    if (verify) {
        ftpflow::KnownHosts kh(g_knownHostsPath);
        trusted = kh.trustedHexFor(ep.host, ep.port);
    }

    // Capture remote stdout/stderr, printing each stream as it arrives.
    ssh.setStdoutSink([](const void* p, std::size_t n) {
        std::fwrite(p, 1, n, stdout);
    });
    ssh.setStderrSink([](const void* p, std::size_t n) {
        std::fwrite(p, 1, n, stderr);
    });

    ftpflow::SshResult r = ssh.open(ep, command, trusted, &acceptedKey);
    if (verify && r.status == ftpflow::SshStatus::HostKeyRejected &&
        !r.observedHostKeyHex.empty()) {
        // Trust-on-first-use: show the standard fingerprint, persist, retry.
        std::fprintf(stderr,
            "WARNING: remote host key is not in %s\n"
            "  fingerprint: %s\n"
            "  trusting and retrying...\n",
            g_knownHostsPath.c_str(),
            fingerprintOfBlobHex(r.observedHostKeyHex).c_str());
        ftpflow::KnownHosts kh(g_knownHostsPath);
        std::string err;
        if (!kh.trust(ep.host, ep.port, "ssh-rsa",
                      ftpflow::KnownHosts::hexDecode(r.observedHostKeyHex),
                      &err)) {
            std::fprintf(stderr, "cannot persist host key: %s\n", err.c_str());
            return 1;
        }
        ftpflow::SshExec retry(ftpflow::HostKeyPolicy::Strict);
        retry.setStdoutSink([](const void* p, std::size_t n) {
            std::fwrite(p, 1, n, stdout);
        });
        retry.setStderrSink([](const void* p, std::size_t n) {
            std::fwrite(p, 1, n, stderr);
        });
        r = retry.open(ep, command, kh.trustedHexFor(ep.host, ep.port), nullptr);
    }
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

// ssh push <host> <user> <password|-> [--key <path>] [--level N]
//          [--cmd <template>] <local> <remote>
//   The compressed-transfer fast path: client zstd-streams the local file
//   into the exec channel's stdin; the remote command decompresses stdin
//   to <remote>. The default command assumes a zstd-equipped host; --cmd
//   overrides it ({file} is replaced by the quoted remote path), e.g. on
//   hosts without zstd: --cmd "\"C:\path\ftpcli.exe\" zdecode > {file}".
int cmdSshPush(const std::vector<std::string>& args) {
    if (args.size() < 5) {
        std::fprintf(stderr,
            "usage: ftpcli ssh push <host> <user> <password|-> [--key <path>]\n"
            "                       [--level N] [--cmd <template>] <local> <remote>\n"
            "  remote default: 'zstd -d -c - -o {file}'\n");
        return 2;
    }
    ftpflow::SshEndpoint ep;
    ep.host = args[0];
    ep.user = args[1];
    ep.password = args[2];
    const std::size_t colon = ep.host.rfind(':');
    if (colon != std::string::npos && colon + 1 < ep.host.size()) {
        ep.port = static_cast<std::uint16_t>(std::atoi(ep.host.c_str() + colon + 1));
        ep.host = ep.host.substr(0, colon);
    }
    std::string cmdTemplate = ftpflow::sshDefaultPushCommandTemplate();
    ftpflow::SshPushOptions opts;
    std::string local, remote;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--key" && i + 1 < args.size()) {
            ep.privateKeyPem = readWholeFile(args[++i]);
            if (ep.privateKeyPem.empty()) {
                std::fprintf(stderr, "ssh push: cannot read key file %s\n", args[i].c_str());
                return 2;
            }
        } else if (args[i] == "--level" && i + 1 < args.size()) {
            opts.zstdLevel = std::atoi(args[++i].c_str());
        } else if (args[i] == "--cmd" && i + 1 < args.size()) {
            cmdTemplate = args[++i];
        } else if (local.empty()) {
            local = args[i];
        } else if (remote.empty()) {
            remote = args[i];
        } else {
            std::fprintf(stderr, "ssh push: unexpected arg %s\n", args[i].c_str());
            return 2;
        }
    }
    if (local.empty() || remote.empty()) {
        std::fprintf(stderr, "ssh push: need <local> <remote>\n");
        return 2;
    }

    std::string command = cmdTemplate;
    const std::string quoted = "\"" + remote + "\"";
    for (std::size_t pos; (pos = command.find("{file}")) != std::string::npos;)
        command.replace(pos, 6, quoted);

    const bool verify = !g_knownHostsPath.empty();
    ftpflow::SshExec ssh(verify ? ftpflow::HostKeyPolicy::Strict
                                : ftpflow::HostKeyPolicy::AcceptNew);
    std::string acceptedKey;
    std::vector<std::string> trusted;
    if (verify) {
        ftpflow::KnownHosts kh(g_knownHostsPath);
        trusted = kh.trustedHexFor(ep.host, ep.port);
    }
    ftpflow::SshResult r = ssh.open(ep, command, trusted, &acceptedKey);
    if (verify && r.status == ftpflow::SshStatus::HostKeyRejected &&
        !r.observedHostKeyHex.empty()) {
        std::fprintf(stderr,
            "WARNING: remote host key is not in %s\n"
            "  fingerprint: %s\n"
            "  trusting and retrying...\n",
            g_knownHostsPath.c_str(),
            fingerprintOfBlobHex(r.observedHostKeyHex).c_str());
        ftpflow::KnownHosts kh(g_knownHostsPath);
        std::string err;
        if (!kh.trust(ep.host, ep.port, "ssh-rsa",
                      ftpflow::KnownHosts::hexDecode(r.observedHostKeyHex),
                      &err)) {
            std::fprintf(stderr, "cannot persist host key: %s\n", err.c_str());
            return 1;
        }
        ftpflow::SshExec retry(ftpflow::HostKeyPolicy::Strict);
        r = retry.open(ep, command, kh.trustedHexFor(ep.host, ep.port), nullptr);
        if (!r.ok()) {
            std::fprintf(stderr, "ssh open failed [%s]: %s\n",
                ftpflow::sshStatusName(r.status), r.message.c_str());
            return 1;
        }
        ftpflow::SshPushStats st2;
        ftpflow::SshResult pr2 = ftpflow::sshPushFile(retry, local, opts, &st2);
        if (!pr2.ok()) {
            std::fprintf(stderr, "ssh push failed [%s]: %s\n",
                ftpflow::sshStatusName(pr2.status), pr2.message.c_str());
            return 1;
        }
        std::printf("pushed %llu -> %llu bytes (ratio %.1f%%, exit=%d, level=%d)\n",
            static_cast<unsigned long long>(st2.localBytes),
            static_cast<unsigned long long>(st2.compressedBytes),
            st2.ratio() * 100.0, retry.exitCode(), opts.zstdLevel);
        return 0;
    }
    if (!r.ok()) {
        std::fprintf(stderr, "ssh open failed [%s]: %s\n",
            ftpflow::sshStatusName(r.status), r.message.c_str());
        return 1;
    }

    ftpflow::SshPushStats st;
    ftpflow::SshResult pr = ftpflow::sshPushFile(ssh, local, opts, &st);
    if (!pr.ok()) {
        std::fprintf(stderr, "ssh push failed [%s]: %s\n",
            ftpflow::sshStatusName(pr.status), pr.message.c_str());
        return 1;
    }
    std::printf("pushed %llu -> %llu bytes (ratio %.1f%%, exit=%d, level=%d)\n",
        static_cast<unsigned long long>(st.localBytes),
        static_cast<unsigned long long>(st.compressedBytes),
        st.ratio() * 100.0, ssh.exitCode(), opts.zstdLevel);
    return 0;
}

// zdecode -- the remote-agent stand-in: reads a zstd stream from stdin and
// writes the decompressed bytes to stdout (binary-safe). Used as the
// --cmd payload on hosts without a system zstd, and doubles as the
// reference consumer of sshPushFile's output format.
int cmdZdecode(const std::vector<std::string>& args) {
    (void)args;
#if defined(FTPFLOW_HAVE_ZSTD)
    _setmode(_fileno(stdin), 0x8000);    // _O_BINARY
    _setmode(_fileno(stdout), 0x8000);

    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) return 2;
    if (ZSTD_isError(ZSTD_initDStream(ds))) { ZSTD_freeDStream(ds); return 2; }

    std::vector<std::uint8_t> in(ZSTD_DStreamInSize());
    std::vector<std::uint8_t> out(ZSTD_DStreamOutSize());
    int rc = 0;
    for (;;) {
        const std::size_t got = std::fread(in.data(), 1, in.size(), stdin);
        ZSTD_inBuffer zin{in.data(), got, 0};
        do {
            ZSTD_outBuffer zout{out.data(), out.size(), 0};
            const std::size_t r = ZSTD_decompressStream(ds, &zout, &zin);
            if (ZSTD_isError(r)) {
                std::fprintf(stderr, "zdecode: %s\n", ZSTD_getErrorName(r));
                rc = 1;
                break;
            }
            if (zout.pos > 0) std::fwrite(out.data(), 1, zout.pos, stdout);
        } while (zin.pos < zin.size);
        if (rc != 0 || got < in.size()) break;   // EOF
    }
    std::fflush(stdout);
    ZSTD_freeDStream(ds);
    return rc;
#else
    std::fprintf(stderr, "zdecode: built without zstd\n");
    return 2;
#endif
}

}  // namespace

namespace {

// ftp <ls|get|put|size> <host[:port]> [user] [password] ...
//   The FTP/FTPS smoke surface over FtpSession. Password may be "-" to
//   use the anonymous default.
ftpflow::FtpOptions parseFtpEndpoint(const std::vector<std::string>& a,
                                     std::size_t& i) {
    ftpflow::FtpOptions opt;
    opt.host = a[i++];
    const std::size_t colon = opt.host.rfind(':');
    if (colon != std::string::npos && colon + 1 < opt.host.size()) {
        opt.port = static_cast<std::uint16_t>(std::atoi(opt.host.c_str() + colon + 1));
        opt.host = opt.host.substr(0, colon);
    }
    if (i < a.size()) {
        opt.user = a[i++];
        if (i < a.size()) {
            const std::string pw = a[i++];
            opt.password = (pw == "-") ? std::string() : pw;
        }
    }
    return opt;
}

int cmdFtp(const std::vector<std::string>& a) {
    if (a.size() < 2) {
        std::fprintf(stderr,
            "usage: ftpcli ftp <cmd> <host[:port]> [user] [password] ...\n"
            "  ls   <host> [user] [pass] [dir]        list a directory\n"
            "  size <host> [user] [pass] <file>      remote file size\n"
            "  get  <host> [user] [pass] <r> <l>     download (resume off)\n"
            "  put  <host> [user] [pass] <l> <r>     upload\n"
            "  password '-' = anonymous default\n");
        return 2;
    }
    const std::string& cmd = a[0];
    if (cmd != "ls" && cmd != "size" && cmd != "get" && cmd != "put") {
        std::fprintf(stderr, "ftp: unknown subcommand %s\n", cmd.c_str());
        return 2;
    }
    std::size_t i = 1;
    ftpflow::FtpOptions opt = parseFtpEndpoint(a, i);

    ftpflow::FtpSession ftp;
    ftpflow::FtpResult r = ftp.open(opt);
    if (!r.ok()) {
        std::fprintf(stderr, "ftp open failed [%s]: %s\n",
            ftpflow::ftpStatusName(r.status), r.message.c_str());
        return 1;
    }

    if (cmd == "ls") {
        std::string dir = i < a.size() ? a[i] : "/";
        ftpflow::ListingPage page = ftp.list(dir);
        if (page.entries.empty()) {
            std::fprintf(stderr, "ftp ls: empty or failed (dir %s)\n", dir.c_str());
            return 1;
        }
        std::printf("%-40s %12s %s\n", "NAME", "SIZE", "TYPE");
        for (const auto& e : page.entries) {
            const char* t = e.type == ftpflow::EntryType::Directory ? "dir"
                          : e.type == ftpflow::EntryType::Symlink ? "link" : "file";
            std::printf("%-40s %12llu %s\n", e.name.c_str(),
                static_cast<unsigned long long>(e.size), t);
        }
        return 0;
    }
    if (cmd == "size") {
        if (i >= a.size()) { std::fprintf(stderr, "size: need <file>\n"); return 2; }
        std::uint64_t n = 0;
        ftpflow::FtpResult sr = ftp.size(a[i], &n);
        if (!sr.ok()) {
            std::fprintf(stderr, "ftp size failed: %s\n", sr.message.c_str());
            return 1;
        }
        std::printf("%llu\n", static_cast<unsigned long long>(n));
        return 0;
    }
    if (cmd == "get") {
        if (i + 1 >= a.size()) { std::fprintf(stderr, "get: need <remote> <local>\n"); return 2; }
        std::uint64_t moved = 0;
        ftpflow::FtpResult gr = ftp.download(a[i], a[i + 1], 0, &moved);
        if (!gr.ok()) {
            std::fprintf(stderr, "ftp get failed: %s\n", gr.message.c_str());
            return 1;
        }
        std::printf("downloaded %llu bytes\n", static_cast<unsigned long long>(moved));
        return 0;
    }
    // put
    if (i + 1 >= a.size()) { std::fprintf(stderr, "put: need <local> <remote>\n"); return 2; }
    std::uint64_t moved = 0;
    ftpflow::FtpResult pr = ftp.upload(a[i], a[i + 1], 0, &moved);
    if (!pr.ok()) {
        std::fprintf(stderr, "ftp put failed: %s\n", pr.message.c_str());
        return 1;
    }
    std::printf("uploaded %llu bytes\n", static_cast<unsigned long long>(moved));
    return 0;
}

// sftp <cmd> <host[:port]> <user> <password|-> [--key <path>] ...
//   ls   [dir]              list a directory (structured readdir)
//   size <remote>           remote file size
//   get  <remote> <local>   download
//   put  <local> <remote>   upload
//   mkdir|rmdir|rm|chmod    metadata ops
int cmdSftp(const std::vector<std::string>& a) {
    if (a.size() < 4) {
        std::fprintf(stderr,
            "usage: ftpcli sftp <cmd> <host[:port]> <user> <password|-> [--key <path>] ...\n"
            "  ls <dir> | size <r> | get <r> <l> | put <l> <r>\n"
            "  mkdir <dir> | rmdir <dir> | rm <file> | chmod <path> <mode>\n"
            "  password '-' + --key <path> selects private-key auth.\n");
        return 2;
    }
    const std::string& cmd = a[0];
    ftpflow::SshEndpoint ep;
    ep.host = a[1];
    ep.user = a[2];
    ep.password = a[3];
    const std::size_t colon = ep.host.rfind(':');
    if (colon != std::string::npos && colon + 1 < ep.host.size()) {
        ep.port = static_cast<std::uint16_t>(std::atoi(ep.host.c_str() + colon + 1));
        ep.host = ep.host.substr(0, colon);
    }
    std::vector<std::string> rest;
    for (std::size_t i = 4; i < a.size(); ++i) {
        if (a[i] == "--key" && i + 1 < a.size()) {
            ep.privateKeyPem = readWholeFile(a[++i]);
            if (ep.privateKeyPem.empty()) {
                std::fprintf(stderr, "sftp: cannot read key file %s\n", a[i].c_str());
                return 2;
            }
        } else {
            rest.push_back(a[i]);
        }
    }

    const bool verify = !g_knownHostsPath.empty();
    ftpflow::SftpSession sftp(verify ? ftpflow::HostKeyPolicy::Strict
                                     : ftpflow::HostKeyPolicy::AcceptNew);
    std::string acceptedKey;
    std::vector<std::string> trusted;
    if (verify) {
        ftpflow::KnownHosts kh(g_knownHostsPath);
        trusted = kh.trustedHexFor(ep.host, ep.port);
    }
    ftpflow::SftpResult r = sftp.open(ep, trusted, &acceptedKey);
    if (verify && r.status == ftpflow::SftpStatus::HostKeyRejected &&
        !r.observedHostKeyHex.empty()) {
        std::fprintf(stderr,
            "WARNING: remote host key is not in %s\n"
            "  fingerprint: %s\n"
            "  trusting and retrying...\n",
            g_knownHostsPath.c_str(),
            fingerprintOfBlobHex(r.observedHostKeyHex).c_str());
        ftpflow::KnownHosts kh(g_knownHostsPath);
        std::string err;
        if (!kh.trust(ep.host, ep.port, "ssh-rsa",
                      ftpflow::KnownHosts::hexDecode(r.observedHostKeyHex),
                      &err)) {
            std::fprintf(stderr, "cannot persist host key: %s\n", err.c_str());
            return 1;
        }
        // Reopen this same session object is not possible (SftpSession is
        // non-copyable), so rebuild a strict one against the now-trusted key.
        ftpflow::SftpSession retry(ftpflow::HostKeyPolicy::Strict);
        r = retry.open(ep, kh.trustedHexFor(ep.host, ep.port), nullptr);
        if (!r.ok()) {
            std::fprintf(stderr, "sftp open failed [%s]: %s\n",
                ftpflow::sftpStatusName(r.status), r.message.c_str());
            return 1;
        }
        sftp = std::move(retry);
    }
    if (!r.ok()) {
        std::fprintf(stderr, "sftp open failed [%s]: %s\n",
            ftpflow::sftpStatusName(r.status), r.message.c_str());
        return 1;
    }
    if (!acceptedKey.empty()) {
        std::fprintf(stderr, "host key (TOFU accepted): sha256-blob %s\n",
            acceptedKey.substr(0, 64).c_str());
    }

    auto fail = [](const ftpflow::SftpResult& x) {
        std::fprintf(stderr, "sftp failed [%s]: %s\n",
            ftpflow::sftpStatusName(x.status), x.message.c_str());
        return 1;
    };

    if (cmd == "ls") {
        const std::string dir = rest.empty() ? "." : rest[0];
        ftpflow::ListingPage page = sftp.list(dir);
        if (!page.cursor.empty() && page.entries.empty()) {
            // list() reports failure through cursor+empty (session/protocol
            // errors carry the message).
            std::fprintf(stderr, "sftp ls failed: %s\n", page.cursor.c_str());
            return 1;
        }
        std::printf("%-40s %12s %s\n", "NAME", "SIZE", "TYPE");
        for (const auto& e : page.entries) {
            const char* t = e.type == ftpflow::EntryType::Directory ? "dir"
                          : e.type == ftpflow::EntryType::Symlink ? "link" : "file";
            std::printf("%-40s %12llu %s\n", e.name.c_str(),
                static_cast<unsigned long long>(e.size), t);
        }
        return 0;
    }
    if (cmd == "size") {
        if (rest.empty()) { std::fprintf(stderr, "size: need <remote>\n"); return 2; }
        std::uint64_t n = 0;
        ftpflow::SftpResult sr = sftp.size(rest[0], &n);
        if (!sr.ok()) return fail(sr);
        std::printf("%llu\n", static_cast<unsigned long long>(n));
        return 0;
    }
    if (cmd == "get") {
        bool resume = false;
        if (!rest.empty() && rest[0] == "--resume") { resume = true; rest.erase(rest.begin()); }
        if (rest.size() < 2) { std::fprintf(stderr, "get: need <remote> <local>\n"); return 2; }
        std::uint64_t resumeBytes = 0;
        if (resume) {
            struct _stat64 st{};
            if (_stat64(rest[1].c_str(), &st) == 0) resumeBytes = (std::uint64_t)st.st_size;
        }
        std::uint64_t moved = 0;
        ftpflow::SftpResult gr = sftp.download(rest[0], rest[1], resumeBytes, &moved);
        if (!gr.ok()) return fail(gr);
        std::printf("downloaded %llu bytes%s\n",
            static_cast<unsigned long long>(moved),
            resume ? " (resumed)" : "");
        return 0;
    }
    if (cmd == "put") {
        if (rest.size() < 2) { std::fprintf(stderr, "put: need <local> <remote>\n"); return 2; }
        std::uint64_t moved = 0;
        ftpflow::SftpResult pr = sftp.upload(rest[0], rest[1], 0, &moved);
        if (!pr.ok()) return fail(pr);
        std::printf("uploaded %llu bytes\n", static_cast<unsigned long long>(moved));
        return 0;
    }
    if (cmd == "mkdir") {
        if (rest.empty()) { std::fprintf(stderr, "mkdir: need <dir>\n"); return 2; }
        ftpflow::SftpResult mr = sftp.mkdir(rest[0]);
        if (mr.ok()) { std::printf("mkdir ok\n"); return 0; }
        return fail(mr);
    }
    if (cmd == "rmdir") {
        if (rest.empty()) { std::fprintf(stderr, "rmdir: need <dir>\n"); return 2; }
        ftpflow::SftpResult dr = sftp.removeDir(rest[0]);
        if (dr.ok()) { std::printf("rmdir ok\n"); return 0; }
        return fail(dr);
    }
    if (cmd == "rm") {
        if (rest.empty()) { std::fprintf(stderr, "rm: need <file>\n"); return 2; }
        ftpflow::SftpResult rr = sftp.removeFile(rest[0]);
        if (rr.ok()) { std::printf("rm ok\n"); return 0; }
        return fail(rr);
    }
    if (cmd == "chmod") {
        if (rest.size() < 2) { std::fprintf(stderr, "chmod: need <path> <mode>\n"); return 2; }
        ftpflow::SftpResult cr = sftp.chmod(rest[0],
            (std::uint32_t)std::strtoul(rest[1].c_str(), nullptr, 8));
        if (cr.ok()) { std::printf("chmod ok\n"); return 0; }
        return fail(cr);
    }
    std::fprintf(stderr, "sftp: unknown subcommand %s\n", cmd.c_str());
    return 2;
}

// queue <ftp|sftp|ssh> <host[:port]> <user> <pw|-> [--key K] [--workers N]
//       [--level N] [--cmd T] [--store] [--resume] [--ftz] [--down] [-r]
//       <local> <remote> [<local> <remote>...]
//   Enqueues transfers through the concurrent TransferQueue and prints the
//   event stream. 'ssh' uploads auto-decide Compress vs Store through the
//   5-stage funnel (--store forces SFTP store). --resume picks up partial
//   transfers (FTP REST/APPE, SFTP seek, SSH append; byte-exact). --ftz
//   ships SSH/SFTP transfers as FTZ1 containers (remotePath + ".ftz1" on
//   upload; remotePath names the container on download), frame-verified.
//   -r treats each pair as directory ROOTS and enqueues the whole tree.
int cmdQueue(const std::vector<std::string>& a) {
    if (a.size() < 6) {
        std::fprintf(stderr,
            "usage: ftpcli queue <ftp|sftp|ssh> <host[:port]> <user> <pw|-> [--key K]\n"
            "                     [--workers N] [--level N] [--cmd T] [--store]\n"
            "                     [--resume] [--ftz] [--down] [-r]\n"
            "                     [--journal PATH] [--restore]\n"
            "                     <local> <remote> [...]\n"
            "  --resume:  continue from the already-transferred prefix.\n"
            "  --ftz:     FTZ1 container mode (ssh/sftp endpoints only).\n"
            "  --down:    download direction. -r: recursive (dir roots).\n"
            "  --journal: persist in-flight jobs to PATH (resumable on crash).\n"
            "  --restore: replay PATH before the new jobs, resume=true.\n");
        return 2;
    }
    const std::string& proto = a[0];
    if (proto != "ftp" && proto != "sftp" && proto != "ssh") {
        std::fprintf(stderr, "queue: unknown protocol %s\n", proto.c_str());
        return 2;
    }
    ftpflow::SshEndpoint ep;
    ftpflow::FtpOptions fopt;
    ep.host = fopt.host = a[1];
    ep.user = fopt.user = a[2];
    ep.password = fopt.password = a[3];
    const std::size_t colon = ep.host.rfind(':');
    if (colon != std::string::npos && colon + 1 < ep.host.size()) {
        ep.port = fopt.port = static_cast<std::uint16_t>(
            std::atoi(ep.host.c_str() + colon + 1));
        ep.host = fopt.host = ep.host.substr(0, colon);
    }
    unsigned workers = 2;
    int level = 3;
    std::string cmdTemplate;
    bool store = false;
    bool doResume = false;
    bool ftz = false;
    bool down = false;
    bool recursive = false;
    std::string journalPath;
    bool restore = false;
    std::vector<std::string> pos;
    for (std::size_t i = 4; i < a.size(); ++i) {
        if (a[i] == "--key" && i + 1 < a.size()) {
            ep.privateKeyPem = readWholeFile(a[++i]);
            if (ep.privateKeyPem.empty()) {
                std::fprintf(stderr, "queue: cannot read key file %s\n", a[i].c_str());
                return 2;
            }
        } else if (a[i] == "--workers" && i + 1 < a.size()) {
            workers = (unsigned)std::atoi(a[++i].c_str());
        } else if (a[i] == "--level" && i + 1 < a.size()) {
            level = std::atoi(a[++i].c_str());
        } else if (a[i] == "--cmd" && i + 1 < a.size()) {
            cmdTemplate = a[++i];
        } else if (a[i] == "--store") {
            store = true;
        } else if (a[i] == "--resume") {
            doResume = true;
        } else if (a[i] == "--ftz") {
            ftz = true;
        } else if (a[i] == "--down") {
            down = true;
        } else if (a[i] == "-r" || a[i] == "--recursive") {
            recursive = true;
        } else if (a[i] == "--journal" && i + 1 < a.size()) {
            journalPath = a[++i];
        } else if (a[i] == "--restore") {
            restore = true;
        } else {
            pos.push_back(a[i]);
        }
    }
    if (pos.size() < 2 || pos.size() % 2 != 0) {
        std::fprintf(stderr, "queue: need an even number of <local> <remote> args\n");
        return 2;
    }

    ftpflow::TransferQueue queue(workers);

    // Durable journal: persist in-flight jobs, optionally restore first.
    if (!journalPath.empty()) {
        queue.setJournal(journalPath);
        if (restore) {
            std::string terr;
            // Restored jobs need credentials again (the journal drops them);
            // supply the endpoint from this invocation's command line.
            const std::size_t n = queue.restore(&terr,
                [&](ftpflow::TransferRequest& r) {
                    r.ftp = fopt;
                    r.ssh = ep;
                    r.pushCommandTemplate = cmdTemplate;
                });
            if (!terr.empty())
                std::fprintf(stderr, "queue: restore warning: %s\n", terr.c_str());
            std::printf("[queue] restored %llu in-flight job(s) from %s\n",
                        (unsigned long long)n, journalPath.c_str());
        }
    }

    queue.setEventCallback([](const ftpflow::TransferEvent& e) {
        const char* kind = e.kind == ftpflow::TransferEvent::Kind::Started   ? "start"
                          : e.kind == ftpflow::TransferEvent::Kind::Progress ? "prog "
                                                                             : "done ";
        const char* st = e.state == ftpflow::TransferState::Done    ? "done    "
                        : e.state == ftpflow::TransferState::Failed  ? "failed  "
                        : e.state == ftpflow::TransferState::Canceled ? "canceled"
                                                                     : "running ";
        const char* mode = e.mode == ftpflow::TransferMode::Compress ? "compress"
                          : e.mode == ftpflow::TransferMode::Ftz1     ? "ftz1    "
                                                                      : "store   ";
        const double pct = e.progress.bytesTotal > 0
            ? 100.0 * (double)e.progress.bytesMoved / (double)e.progress.bytesTotal
            : 0.0;
        std::printf("[job %llu] %s %s mode=%s %.1f%% (%llu/%llu, wire %llu, ratio %.3f)%s%s\n",
            (unsigned long long)e.jobId, kind, st, mode,
            pct,
            (unsigned long long)e.progress.bytesMoved,
            (unsigned long long)e.progress.bytesTotal,
            (unsigned long long)e.progress.bytesOnWire,
            e.progress.compressionRatio,
            e.error.empty() ? "" : " err=",
            e.error.c_str());
        if (e.kind == ftpflow::TransferEvent::Kind::Started &&
            e.decision.stage > 0) {
            std::printf("[job %llu]       decision: stage %d, %s\n",
                (unsigned long long)e.jobId, e.decision.stage,
                e.decision.reason.c_str());
        }
        std::fflush(stdout);
    });

    for (std::size_t i = 0; i + 1 < pos.size(); i += 2) {
        const std::string& local = pos[i];
        const std::string& remote = pos[i + 1];
        ftpflow::TransferRequest req;
        req.direction = down ? ftpflow::TransferDirection::Download
                             : ftpflow::TransferDirection::Upload;
        req.protocol = proto == "ftp" ? ftpflow::BackendKind::Ftp
                                       : ftpflow::BackendKind::Sftp;
        req.localPath = local;
        req.remotePath = remote;
        req.ftp = fopt;
        req.ssh = ep;
        req.zstdLevel = level;
        req.pushCommandTemplate = cmdTemplate;
        req.resume = doResume;
        req.ftzArchive = ftz;
        if (store) {
            req.autoDecide = false;
            req.mode = ftpflow::TransferMode::Store;
        }
        if (recursive) {
            std::string terr;
            const std::vector<std::uint64_t> ids =
                queue.enqueueTree(std::move(req), &terr);
            if (!terr.empty()) {
                std::fprintf(stderr, "queue: recursive walk failed: %s\n",
                    terr.c_str());
                return 1;
            }
            if (ids.empty()) {
                std::printf("[queue] nothing to transfer under %s\n",
                    local.c_str());
            } else {
                std::printf("[queue] enqueued %llu jobs: %s -> %s (recursive)\n",
                    (unsigned long long)ids.size(), local.c_str(), remote.c_str());
            }
        } else {
            const std::uint64_t id = queue.enqueue(std::move(req));
            std::printf("[queue] enqueued job %llu: %s -> %s\n",
                (unsigned long long)id, local.c_str(), remote.c_str());
        }
    }
    queue.waitAll();
    std::printf("[queue] drained\n");
    return 0;
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
            "  ftpcli ftp ls|size|get|put <host> ...  FTP smoke surface\n"
            "  ftpcli sftp ls|size|get|put <host> ... [--key K]  SFTP surface\n"
            "  ftpcli ssh exec <host[:port]> <user> <password> <cmd...>\n"
            "  ftpcli ssh push <host> <user> <pw|-> [--key K] [--level N]\n"
            "                   [--cmd T] <local> <remote>   zstd exec-channel push\n"
            "  ftpcli zdecode                         stdin zstd -> stdout (agent)\n"
            "  ftpcli queue <ftp|sftp|ssh> ...        concurrent transfer queue\n"
            "         [--resume] continue partial transfers (byte-exact)\n"
            "         [--ftz]    ship as FTZ1 containers (frame-verified)\n"
            "  --known-hosts <path>   strict host-key verify + persist (ssh/sftp/queue)\n",
            ftpflow::versionString());
        return args.empty() ? 2 : 0;
    }
    const std::string& cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    // A trailing --known-hosts <path> enables strict host-key verification
    // with persistence; strip it (applies to ssh/sftp/queue) before dispatch.
    takeKnownHostsFlag(rest);
    if (cmd == "version") return cmdVersion();
    if (cmd == "decide") return cmdDecide(rest);
    if (cmd == "info") return cmdInfo(rest);
    if (cmd == "ftp") return cmdFtp(rest);
    if (cmd == "sftp") return cmdSftp(rest);
    if (cmd == "queue") return cmdQueue(rest);
    if (cmd == "zdecode") return cmdZdecode(rest);
    if (cmd == "ssh") {
        if (rest.empty() || (rest[0] != "exec" && rest[0] != "push")) {
            std::fprintf(stderr, "usage: ftpcli ssh exec|push ...\n");
            return 2;
        }
        const bool push = rest[0] == "push";
        return push
            ? cmdSshPush(std::vector<std::string>(rest.begin() + 1, rest.end()))
            : cmdSshExec(std::vector<std::string>(rest.begin() + 1, rest.end()));
    }
    std::fprintf(stderr, "unknown command: %s (try --help)\n", cmd.c_str());
    return 2;
}
