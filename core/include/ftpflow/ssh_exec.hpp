// ftpflow -- SSH exec channel: the compression fast path.
// Part of core.lib: pure C++17, no Qt.
//
// One TCP session -> SSH handshake -> auth -> ONE exec channel. The remote
// command (the lightweight decompressor agent) reads its stdin from what we
// write and streams results back on stdout; the same channel serves both
// upload (client->stdin) and download (stdout->client).
//
// The implementation links libssh2 when built with FTPFLOW_WITH_LIBSSH2;
// without it every call fails with EngineUnavailable so the rest of core
// (CLI, tests, GUI) keeps building and behaving.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ftpflow {

enum class SshStatus : std::uint8_t {
    Ok = 0,
    EngineUnavailable,   // built without libssh2
    ConnectFailed,
    HandshakeFailed,
    HostKeyRejected,     // strict policy + unknown key
    AuthFailed,
    ChannelFailed,
    IoError,
    Closed,
    Canceled,   // caller flipped the TransferControl cancel flag
};

struct SshResult {
    SshStatus status = SshStatus::Ok;
    std::string message;         // human-readable detail
    int libssh2Errno = 0;        // raw libssh2 errno when applicable
    // When status is HostKeyRejected, this holds the observed host public-key
    // blob as lowercase hex so the caller can show a fingerprint and (after
    // user confirmation) persist it as trusted.
    std::string observedHostKeyHex;
    bool ok() const { return status == SshStatus::Ok; }
};

enum class HostKeyPolicy : std::uint8_t {
    Strict,    // reject anything not in the trusted list (production default)
    AcceptNew, // trust-on-first-use: unknown keys are accepted and reported
};

struct SshEndpoint {
    std::string host;
    std::uint16_t port = 22;
    std::string user;
    std::string password;               // password auth
    std::string privateKeyPem;          // or key auth (PEM/OpenSSH in memory)
    std::string privateKeyPassphrase;
};

struct SshIoStats {
    std::uint64_t bytesWritten = 0;   // into the channel stdin
    std::uint64_t bytesRead = 0;      // out of the channel stdout/stderr
};

inline const char* sshStatusName(SshStatus s) {
    switch (s) {
        case SshStatus::Ok: return "ok";
        case SshStatus::EngineUnavailable: return "engine-unavailable";
        case SshStatus::ConnectFailed: return "connect-failed";
        case SshStatus::HandshakeFailed: return "handshake-failed";
        case SshStatus::HostKeyRejected: return "host-key-rejected";
        case SshStatus::AuthFailed: return "auth-failed";
        case SshStatus::ChannelFailed: return "channel-failed";
        case SshStatus::IoError: return "io-error";
        case SshStatus::Closed: return "closed";
        case SshStatus::Canceled: return "canceled";
    }
    return "?";
}

class SshExec {
public:
    explicit SshExec(HostKeyPolicy policy = HostKeyPolicy::Strict);
    ~SshExec();

    SshExec(const SshExec&) = delete;
    SshExec& operator=(const SshExec&) = delete;

    // Connect, handshake, verify the host key, authenticate, and start
    // `command` on a single exec channel.
    //
    // trustedHostKeyHex: full host public-key blobs as lowercase hex,
    // matched exactly. Unknown-key handling follows the policy.
    // acceptedHostKeyHex: when policy is AcceptNew and the key was unknown,
    // the observed blob is returned here so the caller can persist it.
    SshResult open(const SshEndpoint& ep,
                   const std::string& command,
                   const std::vector<std::string>& trustedHostKeyHex,
                   std::string* acceptedHostKeyHex = nullptr);

    // Interactive shell: allocate a PTY (xterm-256color) and start the
    // remote login shell on it. This is the interactive-terminal path; it
    // differs from open() in three ways:
    //   * a PTY is requested with the given size (cols x rows),
    //   * the channel runs a shell rather than a single command,
    //   * stdin stays open for bidirectional input -- do NOT sendEof().
    // With a PTY, stdout and stderr are multiplexed onto one stream, so the
    // output arrives via the stdout sink only (setStderrSink is a no-op).
    SshResult openShell(const SshEndpoint& ep,
                        const std::vector<std::string>& trustedHostKeyHex,
                        int cols, int rows,
                        std::string* acceptedHostKeyHex = nullptr);

    // Resize the remote PTY (window-size change). Only valid on a shell
    // channel opened with openShell().
    SshResult resize(int cols, int rows);

    // Feed one chunk into the remote command's stdin. Returns Closed once
    // the channel is gone. Never blocks longer than `timeoutMs`.
    SshResult write(const void* data, std::size_t len, int timeoutMs = 5000);

    // Drain stdout/stderr until quiet or the channel closes. Callbacks run
    // on the calling thread. Returns false once the channel has closed
    // (check exitCode()).
    bool pump(int timeoutMs = 500);

    // Register stdout/stderr sinks; invoked from pump() on the caller's
    // thread. Set before pump() to capture the remote command's output.
    void setStdoutSink(std::function<void(const void*, std::size_t)> fn);
    void setStderrSink(std::function<void(const void*, std::size_t)> fn);

    // Half-close our side: the remote command sees EOF on stdin. Needed by
    // streaming agents (e.g. `zstd -d`) that run until stdin ends; close()
    // alone also sends EOF but then discards the exit status.
    SshResult sendEof();

    void close();

    // Remote exit status when the channel closed cleanly, else -1.
    int exitCode() const;

    SshIoStats stats() const { return stats_; }
    bool isOpen() const { return state_ == State::Open; }

private:
    struct Impl;
    enum class State : std::uint8_t { Idle, Open, Closed };

    // Shared front half of open()/openShell(): resolve, connect, handshake,
    // verify the host key, authenticate, and open a session channel. On
    // success impl_->channel is valid and state_ stays Idle (the caller then
    // starts the channel as exec or pty+shell).
    SshResult connectSession(const SshEndpoint& ep,
                             const std::vector<std::string>& trustedHostKeyHex,
                             std::string* acceptedHostKeyHex);

    Impl* impl_ = nullptr;
    State state_ = State::Idle;
    HostKeyPolicy policy_ = HostKeyPolicy::Strict;
    SshIoStats stats_;
    int exitCode_ = -1;
};

}  // namespace ftpflow
