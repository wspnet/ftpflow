// ftpcli -- ftpflow command line tool.
// Shares core.lib with the desktop shell: what the CLI can do, the GUI can
// do, and vice versa (blueprint: one core, two front ends).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ftpflow/compress_decide.hpp"
#include "ftpflow/protocol_status.hpp"
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
        std::fprintf(stderr, "usage: ftpcli decide <file> [file...]\n");
        return 2;
    }
    int rc = 0;
    std::printf("%-28s %-10s %-4s %s\n", "FILE", "VERDICT", "STG", "REASON");
    for (const std::string& path : args) {
        ftpflow::FileProbe probe = probeFile(path, true);
        ftpflow::Decision d = ftpflow::decideCompressibility(probe);
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

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::printf(
            "ftpcli %s\n"
            "  ftpcli version\n"
            "  ftpcli decide <file...>    run the compressibility funnel\n"
            "  ftpcli info <dir>          directory listing smoke test\n",
            ftpflow::versionString());
        return args.empty() ? 2 : 0;
    }
    const std::string& cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "version") return cmdVersion();
    if (cmd == "decide") return cmdDecide(rest);
    if (cmd == "info") return cmdInfo(rest);
    std::fprintf(stderr, "unknown command: %s (try --help)\n", cmd.c_str());
    return 2;
}
