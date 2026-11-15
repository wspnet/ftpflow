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
};

struct SshResult {
    SshStatus status = SshStatus::Ok;
    std::string message;         // human-readable detail
    int libssh2Errno = 0;        // raw libssh2 errno when applicable
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

    // Feed one chunk into the remote command's stdin. Returns Closed once
    // the channel is gone. Never blocks longer than `timeoutMs`.
    SshResult write(const void* data, std::size_t len, int timeoutMs = 5000);

    // Drain stdout/stderr until quiet or the channel closes. Callbacks run
    // on the calling thread. Returns false once the channel has closed
    // (check exitCode()).
    bool pump(int timeoutMs = 500);

    void close();

    // Remote exit status when the channel closed cleanly, else -1.
    int exitCode() const;

    SshIoStats stats() const { return stats_; }
    bool isOpen() const { return state_ == State::Open; }

private:
    struct Impl;
    enum class State : std::uint8_t { Idle, Open, Closed };
    Impl* impl_ = nullptr;
    State state_ = State::Idle;
    HostKeyPolicy policy_ = HostKeyPolicy::Strict;
    SshIoStats stats_;
    int exitCode_ = -1;
};

}  // namespace ftpflow
