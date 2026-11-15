// ftpflow -- SSH compressed transfer pipeline (push direction).
// Part of core.lib: pure C++17, no Qt.
//
// The fast path from the v5.1 blueprint: instead of SFTP's packet-per-write
// protocol, one exec channel runs a streaming decompressor on the remote
// side while the client zstd-compresses on the fly:
//
//   local file -> ZSTD_CStream -> channel stdin -> remote `zstd -d` -> disk
//
// The remote command template is caller-controlled so any agent works
// (system zstd, our own ftpcli zdecode, a shell pipeline). sshPushFile()
// takes an already-open SshExec whose command decompresses stdin to the
// remote path, streams the file, sends EOF, then pumps until the remote
// command exits and reports its status.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "ftpflow/ssh_exec.hpp"
#include "ftpflow/transfer.hpp"

namespace ftpflow {

struct SshPushStats {
    std::uint64_t localBytes = 0;       // bytes read from disk
    std::uint64_t compressedBytes = 0;  // bytes pushed into the channel
    double ratio() const {
        return localBytes == 0
            ? 0.0
            : static_cast<double>(compressedBytes) /
                  static_cast<double>(localBytes);
    }
};

// Default remote command for `zstd`-equipped hosts. {file} is replaced by
// the caller with the quoted remote path.
inline std::string sshDefaultPushCommandTemplate() {
    return "zstd -d -c - -o {file}";
}

// Streams localPath through zstd (level from opts) into the open channel's
// stdin, then sendEof + pump until the remote command exits. Requires
// FTPFLOW_HAVE_ZSTD; otherwise EngineUnavailable. The remote exit status
// becomes SshStatus::IoError with the stderr tail in `message` when
// nonzero.
struct SshPushOptions {
    int zstdLevel = 3;
    int chunkKiB = 256;           // local read + compressed flush size
    int remoteTimeoutMs = 60000;  // overall wait for the remote to exit
    // Polled between chunks (from another thread, e.g. the queue).
    const TransferControl* control = nullptr;
    // Called after each chunk with cumulative stats. Throttle downstream:
    // this fires per 256 KiB.
    std::function<void(const SshPushStats&)> onProgress;
};

SshResult sshPushFile(SshExec& ssh, const std::string& localPath,
                      const SshPushOptions& opts = {},
                      SshPushStats* stats = nullptr);

}  // namespace ftpflow
