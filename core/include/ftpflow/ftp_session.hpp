// ftpflow -- FTP/FTPS session over libcurl (Schannel TLS).
// Part of core.lib: pure C++17, no Qt.
//
// One easy handle per session; libcurl pools the control connection across
// operations on the same handle, so open() + a sequence of list/download/
// upload calls reuse one TCP/TLS session. Without FTPFLOW_HAVE_CURL every
// operation fails with EngineUnavailable (same degrade pattern as SshExec).
//
// Listing: FTP LIST output parsed as unix-style with a DOS-style fallback
// (Windows IIS). Download resume uses FTP REST via CURLOPT_RESUME.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ftpflow/transfer.hpp"
#include "ftpflow/vfs.hpp"

namespace ftpflow {

enum class FtpStatus : std::uint8_t {
    Ok = 0,
    EngineUnavailable,
    BadUrl,
    ConnectFailed,
    AuthFailed,
    ListFailed,
    DownloadFailed,
    UploadFailed,
    IoError,
    Canceled,    // caller flipped the TransferControl cancel flag
};

inline const char* ftpStatusName(FtpStatus s) {
    switch (s) {
        case FtpStatus::Ok: return "ok";
        case FtpStatus::EngineUnavailable: return "engine-unavailable";
        case FtpStatus::BadUrl: return "bad-url";
        case FtpStatus::ConnectFailed: return "connect-failed";
        case FtpStatus::AuthFailed: return "auth-failed";
        case FtpStatus::ListFailed: return "list-failed";
        case FtpStatus::DownloadFailed: return "download-failed";
        case FtpStatus::UploadFailed: return "upload-failed";
        case FtpStatus::IoError: return "io-error";
        case FtpStatus::Canceled: return "canceled";
    }
    return "?";
}

struct FtpResult {
    FtpStatus status = FtpStatus::Ok;
    std::string message;
    int curlCode = 0;        // raw CURLE code when applicable
    bool ok() const { return status == FtpStatus::Ok; }
};

struct FtpOptions {
    std::string host;          // host or host:port (no scheme)
    std::uint16_t port = 21;
    std::string user = "anonymous";
    std::string password = "ftpflow@localhost";
    std::string initialPath = "/";   // remote working root
    bool tls = false;               // AUTH TLS (explicit FTPS)
    bool implicitTls = false;       // ftps:// implicit TLS on 990
    int connectTimeoutMs = 10000;
};

class FtpSession {
public:
    FtpSession();
    ~FtpSession();

    FtpSession(const FtpSession&) = delete;
    FtpSession& operator=(const FtpSession&) = delete;

    // Connects and verifies authentication with a cheap PWD round trip.
    FtpResult open(const FtpOptions& opt);

    Capabilities capabilities() const;

    // Lists one remote directory (whole listing; FTP has no cursor).
    ListingPage list(const std::string& dir);

    // Remote file size via SIZE (binary mode); 0 + non-Ok on failure.
    FtpResult size(const std::string& remotePath, std::uint64_t* out);

    // Downloads remote -> local. `resumeBytes` continues an interrupted
    // transfer (FTP REST) by appending to the local file from that offset.
    // `ctl` is polled inside libcurl's progress hook (pause parks, cancel
    // aborts -> FtpStatus::Canceled).
    FtpResult download(const std::string& remotePath,
                       const std::string& localPath,
                       std::uint64_t resumeBytes = 0,
                       std::uint64_t* bytesMoved = nullptr,
                       const TransferControl* ctl = nullptr);

    // Uploads local -> remote (STOR). `resumeBytes` uses APPE-style
    // continuation via CURLOPT_APPEND.
    FtpResult upload(const std::string& localPath,
                     const std::string& remotePath,
                     std::uint64_t resumeBytes = 0,
                     std::uint64_t* bytesMoved = nullptr,
                     const TransferControl* ctl = nullptr);

    // (moved, total) via libcurl's progress hook, roughly per network
    // chunk. Throttle downstream.
    void setProgressCallback(std::function<void(std::uint64_t, std::uint64_t)> fn);

    // Metadata operations (MKD / DELE / RMD / RNFR+RNTO through libcurl).
    // Same error contract as the transfer calls; lastError() holds the
    // server response tail when one fails.
    FtpResult mkdir(const std::string& remotePath);
    FtpResult removeFile(const std::string& remotePath);
    FtpResult removeDir(const std::string& remotePath);
    FtpResult rename(const std::string& from, const std::string& to);

    bool isOpen() const { return opened_; }
    void close();

    // True when the most recent failed operation was a transport drop
    // (CURLE_RECV_ERROR / CURLE_SEND_ERROR) rather than a server-side or
    // business error. Mirrors SftpSession::connectionLost() for the GUI's
    // auto-reconnect decision.
    bool connectionLost() const;

    // Human-readable cause of the most recent failed operation (empty
    // after a success). Lets callers distinguish "empty listing" from
    // "listing failed".
    std::string lastError() const;

private:
    struct Impl;
    FtpResult quoteCommand(const std::string& cmd);
    Impl* impl_ = nullptr;
    bool opened_ = false;
    FtpOptions opt_;
};

}  // namespace ftpflow
