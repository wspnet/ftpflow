// ftpflow -- SFTP session over libssh2 (the SFTP subsystem).
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// Connection/auth/handshake mirror SshExec (non-blocking session, EAGAIN
// loops around socket waits); instead of an exec channel we start the SFTP
// subsystem. Listing uses readdir+attrs (structured data, no LIST text
// parsing), transfers stream through sftp_read/sftp_write, resume works by
// server-side seek offsets.
//
// Without FTPFLOW_HAVE_LIBSSH2 every call fails with EngineUnavailable so
// the CLI/tests/GUI keep building and behaving.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ftpflow/ssh_exec.hpp"   // SshEndpoint, SshStatus, HostKeyPolicy
#include "ftpflow/transfer.hpp"  // TransferControl
#include "ftpflow/vfs.hpp"

namespace ftpflow {

enum class SftpStatus : std::uint8_t {
    Ok = 0,
    EngineUnavailable,   // built without libssh2
    ConnectFailed,
    HandshakeFailed,
    HostKeyRejected,
    AuthFailed,
    SftpFailed,          // subsystem unavailable on the server
    ListFailed,
    DownloadFailed,
    UploadFailed,
    NotFound,            // remote path missing (stat/open ENOENT-ish)
    IoError,
    Canceled,            // caller flipped the cancel flag
};

inline const char* sftpStatusName(SftpStatus s) {
    switch (s) {
        case SftpStatus::Ok: return "ok";
        case SftpStatus::EngineUnavailable: return "engine-unavailable";
        case SftpStatus::ConnectFailed: return "connect-failed";
        case SftpStatus::HandshakeFailed: return "handshake-failed";
        case SftpStatus::HostKeyRejected: return "host-key-rejected";
        case SftpStatus::AuthFailed: return "auth-failed";
        case SftpStatus::SftpFailed: return "sftp-failed";
        case SftpStatus::ListFailed: return "list-failed";
        case SftpStatus::DownloadFailed: return "download-failed";
        case SftpStatus::UploadFailed: return "upload-failed";
        case SftpStatus::NotFound: return "not-found";
        case SftpStatus::IoError: return "io-error";
        case SftpStatus::Canceled: return "canceled";
    }
    return "?";
}

struct SftpResult {
    SftpStatus status = SftpStatus::Ok;
    std::string message;
    int libssh2Errno = 0;
    // When status is HostKeyRejected, holds the observed host public-key blob
    // as lowercase hex (for fingerprint display + trust-on-first-use).
    std::string observedHostKeyHex;
    bool ok() const { return status == SftpStatus::Ok; }
};

class SftpSession {
public:
    explicit SftpSession(HostKeyPolicy policy = HostKeyPolicy::Strict);
    ~SftpSession();

    SftpSession(const SftpSession&) = delete;
    SftpSession& operator=(const SftpSession&) = delete;
    SftpSession(SftpSession&& other) noexcept;
    SftpSession& operator=(SftpSession&& other) noexcept;

    // Connect, handshake, verify host key (same contract as SshExec::open:
    // exact-match hex blobs; AcceptNew returns the observed blob), auth,
    // then start the SFTP subsystem.
    SftpResult open(const SshEndpoint& ep,
                    const std::vector<std::string>& trustedHostKeyHex = {},
                    std::string* acceptedHostKeyHex = nullptr);

    Capabilities capabilities() const;

    // Lists one remote directory. Sorted by name; supports the cursor
    // contract (startAfter = last name of the previous page).
    ListingPage list(const std::string& dir,
                     const std::string& startAfter = {},
                     std::uint32_t maxEntries = 0xFFFFFFFFu);

    // Remote file/directory stat: size (files) via *out.
    SftpResult size(const std::string& remotePath, std::uint64_t* out);

    // Downloads remote -> local in 64 KiB chunks. `resumeBytes` seeks the
    // remote handle and appends to the local file. `ctl` is polled between
    // chunks (pause parks, cancel aborts -> SftpStatus::Canceled). `moved`
    // in the progress callback counts from zero (resume bytes excluded).
    SftpResult download(const std::string& remotePath,
                        const std::string& localPath,
                        std::uint64_t resumeBytes = 0,
                        std::uint64_t* bytesMoved = nullptr,
                        const TransferControl* ctl = nullptr);

    // Uploads local -> remote. `resumeBytes` opens without truncation and
    // seeks the remote handle to that offset.
    SftpResult upload(const std::string& localPath,
                      const std::string& remotePath,
                      std::uint64_t resumeBytes = 0,
                      std::uint64_t* bytesMoved = nullptr,
                      const TransferControl* ctl = nullptr);

    // (moved, total) after each 64 KiB chunk. Throttle downstream: this
    // fires per chunk.
    void setProgressCallback(std::function<void(std::uint64_t, std::uint64_t)> fn);

    SftpResult mkdir(const std::string& remotePath, std::uint32_t mode = 0755);
    SftpResult removeFile(const std::string& remotePath);
    SftpResult removeDir(const std::string& remotePath);
    SftpResult rename(const std::string& from, const std::string& to);
    SftpResult chmod(const std::string& remotePath, std::uint32_t mode);

    bool isOpen() const { return state_ == State::Open; }
    void close();

    // Cause of the most recent failed operation ("" after success).
    std::string lastError() const;

private:    struct Impl;
    enum class State : std::uint8_t { Idle, Open, Closed };
    Impl* impl_ = nullptr;
    State state_ = State::Idle;
    HostKeyPolicy policy_ = HostKeyPolicy::Strict;
};

}  // namespace ftpflow
