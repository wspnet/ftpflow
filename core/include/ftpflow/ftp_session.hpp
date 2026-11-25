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

// ---- MLSD (RFC 3659) line parsing (exposed for unit testing) --------------

// Parse one MLSD line "type=file;size=123;modify=YYYYMMDDHHMMSS[.fff]; name"
// into `e`. Returns false when the line is not a well-formed MLSD entry.
bool parseMlsdLine(const std::string& line, Entry& e);

// Parse an RFC 3659 "modify" timestamp into seconds since epoch (UTC);
// returns 0 on malformed input.
std::int64_t parseMlsdTimestamp(const std::string& s);

struct FtpOptions {
    std::string host;          // host or host:port (no scheme)
    std::uint16_t port = 21;
    std::string user = "anonymous";
    std::string password = "ftpflow@localhost";
    std::string initialPath = "/";   // remote working root
    bool tls = false;               // AUTH TLS (explicit FTPS)
    bool implicitTls = false;       // ftps:// implicit TLS on 990
    int connectTimeoutMs = 10000;
    // Filename byte encoding the server uses. "auto" (default) keeps valid
    // UTF-8 and treats everything else as GBK; "utf-8" forces UTF-8; "gbk"
    // forces the Chinese ANSI code page (covers IIS on a Chinese box).
    std::string encoding = "auto";
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

    // Lists one remote directory forcing `LIST -a` (show hidden entries).
    // Some servers (IIS / hosting panels) hide dot-files and system files
    // from the default LIST/MLSD, which makes a directory look empty while
    // RMD still answers 550 ("not empty"). Used by recursive deletion to
    // surface and remove those hidden stragglers before RMD.
    ListingPage listAll(const std::string& dir);

    // Remote file size via SIZE (binary mode); 0 + non-Ok on failure.
    FtpResult size(const std::string& remotePath, std::uint64_t* out);

    // Stat a remote file: fills `out` (exists + size). FTP cannot cheaply
    // report mtime through libcurl (MDTM needs control-response parsing), so
    // mtimeUnix stays 0 ("unknown"); Smart conflict detection falls back to
    // size-only comparison on FTP.
    FtpResult stat(const std::string& remotePath, RemoteStat* out);

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

    // The encoding actually in effect after a list(): in "auto" mode a GBK
    // listing locks this to "gbk" so that downstream reverse conversions
    // (CWD, MKD/DELE/RMD, rename, and -- critically -- the transfer queue's
    // separate FtpSession) send GBK bytes instead of round-tripping UTF-8.
    std::string effectiveEncoding() const { return opt_.encoding; }

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
    // Issues `cmd` verbatim via CURLOPT_QUOTE, prefixed with an explicit
    // `CWD <cwdDir>`. libcurl sends QUOTE commands BEFORE its own CWD (see
    // the CURLOPT_QUOTE docs), so without this prefix a relative command
    // would run in whatever directory the connection was last left in.
    // CWD-ing ourselves first lets a relative basename work on strict
    // servers (IIS / Windows hosting panels) that reject absolute paths in
    // DELE/RMD/MKD/RNFR -- exactly what FileZilla does.
    FtpResult quoteCommand(const std::string& cwdDir, const std::string& cmd);
    // MLSD (RFC 3659) listing attempt; returns false and leaves lastErr set
    // when the server rejects MLSD so the caller falls back to LIST.
    bool listViaMlsd(const std::string& dir, std::vector<Entry>& out);
    // Converts an internal remote path (e.g. "/wwwroot/images") into the
    // physical absolute path the server expects on the control channel. On
    // IIS user isolation this prefixes the login root (homeDir, e.g.
    // "/zmailvip163com8"); on a normal server it returns the path unchanged
    // (which is already physical-absolute). Always starts with '/'.
    std::string toServerAbsPath(const std::string& path) const;
    Impl* impl_ = nullptr;
    bool opened_ = false;
    FtpOptions opt_;
};

}  // namespace ftpflow
