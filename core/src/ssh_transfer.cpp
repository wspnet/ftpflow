// ftpflow -- SSH compressed transfer pipeline implementation.
#include "ftpflow/ssh_transfer.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "ftpflow/text_codec.hpp"

#if defined(FTPFLOW_HAVE_ZSTD)
#include <zstd.h>
#endif

namespace ftpflow {

#if defined(FTPFLOW_HAVE_ZSTD)

namespace {

std::string trimTail(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

}  // namespace

SshResult sshPushFile(SshExec& ssh, const std::string& localPath,
                      const SshPushOptions& opts, SshPushStats* out) {
    if (!ssh.isOpen()) return {SshStatus::Closed, "channel not open"};

    std::FILE* f = openLocalFile(localPath, "rb");
    if (!f) return {SshStatus::IoError, "cannot open local " + localPath};
    if (opts.resumeFrom > 0) {
        if (_fseeki64(f, (long long)opts.resumeFrom, SEEK_SET) != 0) {
            std::fclose(f);
            return {SshStatus::IoError,
                    "cannot seek local file to resume offset"};
        }
    }

    ZSTD_CStream* cs = ZSTD_createCStream();
    if (!cs) {
        std::fclose(f);
        return {SshStatus::IoError, "ZSTD_createCStream failed"};
    }
    if (ZSTD_isError(ZSTD_initCStream(cs, opts.zstdLevel))) {
        ZSTD_freeCStream(cs);
        std::fclose(f);
        return {SshStatus::IoError, "ZSTD_initCStream failed"};
    }

    SshPushStats st;
    st.resumeFrom = opts.resumeFrom;
    std::string remoteErr;   // last ~2 KiB of remote stderr
    ssh.setStderrSink([&remoteErr](const void* p, std::size_t n) {
        remoteErr.append(static_cast<const char*>(p), n);
        if (remoteErr.size() > 2048) remoteErr.erase(0, remoteErr.size() - 2048);
    });

    const std::size_t inSize = (std::size_t)opts.chunkKiB * 1024;
    std::vector<std::uint8_t> in(inSize);
    std::vector<std::uint8_t> compressed(inSize + inSize / 2);  // headroom

    auto fail = [&](SshStatus s, const std::string& msg) {
        ZSTD_freeCStream(cs);
        std::fclose(f);
        return SshResult{s, msg};
    };
    // Ship compressed bytes; on a channel that died mid-flight, keep going
    // to the exit-status phase so the real remote cause is reported.
    auto ship = [&](const void* p, std::size_t n) {
        SshResult w = ssh.write(p, n, 30000);
        if (w.ok()) st.compressedBytes += n;
        return w;
    };

    // Compress + ship the file in chunks; pump after each chunk so remote
    // stderr surfaces early and the channel never starves the read side.
    bool channelDied = false;
    bool canceled = false;
    for (;;) {
        // Control poll point: park while paused, abort on cancel.
        if (opts.control) {
            while (opts.control->pause && !opts.control->cancel) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (opts.control->cancel) { canceled = true; break; }
        }
        const std::size_t got = std::fread(in.data(), 1, in.size(), f);
        if (got == 0) break;
        st.localBytes += got;
        ZSTD_inBuffer zin{in.data(), got, 0};
        while (zin.pos < zin.size) {
            ZSTD_outBuffer zout{compressed.data(), compressed.size(), 0};
            const std::size_t r = ZSTD_compressStream(cs, &zout, &zin);
            if (ZSTD_isError(r)) {
                return fail(SshStatus::IoError, std::string("zstd: ") +
                               ZSTD_getErrorName(r));
            }
            if (zout.pos > 0) {
                SshResult w = ship(compressed.data(), zout.pos);
                if (w.status == SshStatus::Closed) { channelDied = true; break; }
                if (!w.ok()) return fail(w.status, w.message);
            }
        }
        if (channelDied) break;
        ssh.pump(0);   // non-blocking drain (EAGAIN-safe)
        if (!ssh.isOpen()) { channelDied = true; break; }
        throttle(opts.control, got);   // respect the global rate limit
        if (opts.onProgress) opts.onProgress(st);
    }
    if (canceled) {
        ZSTD_freeCStream(cs);
        std::fclose(f);
        ssh.close();
        return {SshStatus::Canceled, "canceled by caller"};
    }
    if (std::ferror(f)) {
        return fail(SshStatus::IoError, "local read error: " + localPath);
    }

    if (!channelDied) {
        // Flush the compressor epilogue.
        for (;;) {
            ZSTD_outBuffer zout{compressed.data(), compressed.size(), 0};
            const std::size_t r = ZSTD_endStream(cs, &zout);
            if (ZSTD_isError(r)) {
                return fail(SshStatus::IoError,
                            std::string("zstd end: ") + ZSTD_getErrorName(r));
            }
            if (zout.pos > 0) {
                SshResult w = ship(compressed.data(), zout.pos);
                if (w.status == SshStatus::Closed) { channelDied = true; break; }
                if (!w.ok()) return fail(w.status, w.message);
            }
            if (r == 0) break;   // everything flushed
        }
    }
    ZSTD_freeCStream(cs);
    std::fclose(f);

    if (!channelDied) {
        // Half-close: the remote decompressor exits on stdin EOF.
        SshResult eof = ssh.sendEof();
        if (!eof.ok()) return eof;
    }

    // Pump until the channel closes (remote command exited) or timeout.
    const int sliceMs = 200;
    for (int waited = 0; waited < opts.remoteTimeoutMs; waited += sliceMs) {
        if (!ssh.pump(sliceMs)) break;   // channel closed, exit status read
        if (!ssh.isOpen()) break;
    }
    if (ssh.isOpen()) {
        ssh.close();
        return {SshStatus::IoError, "remote command did not exit in time"};
    }
    if (ssh.exitCode() != 0) {
        return {SshStatus::IoError,
                "remote exit " + std::to_string(ssh.exitCode()) +
                    (remoteErr.empty() ? "" : ": " + trimTail(remoteErr))};
    }
    if (out) *out = st;
    return {};
}

#else  // !FTPFLOW_HAVE_ZSTD

SshResult sshPushFile(SshExec&, const std::string&, const SshPushOptions&,
                      SshPushStats*) {
    return {SshStatus::EngineUnavailable,
            "built without zstd (build with FTPFLOW_WITH_ZSTD / --full)"};
}

#endif

}  // namespace ftpflow
