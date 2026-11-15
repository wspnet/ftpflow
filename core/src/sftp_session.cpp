// ftpflow -- SFTP session implementation (libssh2 SFTP subsystem).
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// Without FTPFLOW_HAVE_LIBSSH2 the whole file degrades to "engine
// unavailable" stubs, so the CLI/tests always link.
#include "ftpflow/sftp_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(FTPFLOW_HAVE_LIBSSH2)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/stat.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#else
// Stub build: nothing to include.
#endif

namespace ftpflow {

#if defined(FTPFLOW_HAVE_LIBSSH2)

struct SftpSession::Impl {
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_SFTP* sftp = nullptr;
    SOCKET sock = INVALID_SOCKET;

    std::function<void(std::uint64_t, std::uint64_t)> onProgress;
    std::uint64_t progressTotal = 0;   // set per transfer by the caller side

    static std::string hexOf(const unsigned char* p, std::size_t n) {
        static const char* kHex = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
            s.push_back(kHex[p[i] >> 4]);
            s.push_back(kHex[p[i] & 0xF]);
        }
        return s;
    }
};

namespace {

struct WsaGuard {
    bool ok = false;
    WsaGuard() {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WsaGuard() { if (ok) WSACleanup(); }
};

bool waitSocket(SOCKET s, bool writable, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv{(long)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000)};
    const int rc = select(0, writable ? nullptr : &fds,
                          writable ? &fds : nullptr, nullptr, &tv);
    return rc > 0;
}

SftpResult mapSshErr(SftpStatus st, const char* what, LIBSSH2_SESSION* sess) {
    SftpResult r;
    r.status = st;
    int err = 0;
    std::string msg;
    if (sess) {
        err = libssh2_session_last_errno(sess);
        char* raw = nullptr;  // libssh2-internal storage when ok
        libssh2_session_last_error(sess, &raw, nullptr, 0);
        if (raw) msg = raw;
    }
    r.libssh2Errno = err;
    r.message = std::string(what) + (msg.empty() ? "" : ": ") + msg;
    return r;
}

// Map a failed sftp_open/stat onto a user-facing status. libssh2 surfaces
// the SFTP protocol-level code via sftp_last_error().
SftpStatus classifySftpErr(LIBSSH2_SFTP* sftp) {
    const unsigned long proto = libssh2_sftp_last_error(sftp);
    if (proto == LIBSSH2_FX_NO_SUCH_FILE || proto == LIBSSH2_FX_NO_SUCH_PATH)
        return SftpStatus::NotFound;
    if (proto == LIBSSH2_FX_PERMISSION_DENIED)
        return SftpStatus::AuthFailed;
    return SftpStatus::IoError;
}

// Control poll point shared by download/upload: parks while paused, tells
// the caller to abort on cancel.
bool ctlParkOrAbort(const TransferControl* ctl) {
    if (!ctl) return false;
    while (ctl->pause && !ctl->cancel) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return ctl->cancel;
}

}  // namespace

SftpSession::SftpSession(HostKeyPolicy policy) : policy_(policy) {
    static WsaGuard g_wsa;  // process-wide Winsock init
}

SftpSession::~SftpSession() { close(); delete impl_; }

SftpResult SftpSession::open(const SshEndpoint& ep,
                             const std::vector<std::string>& trustedHostKeyHex,
                             std::string* acceptedHostKeyHex) {
    if (impl_ == nullptr) impl_ = new Impl();
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%u", (unsigned)ep.port);
    if (getaddrinfo(ep.host.c_str(), portStr, &hints, &res) != 0 || !res) {
        return {SftpStatus::ConnectFailed, "cannot resolve " + ep.host};
    }
    impl_->sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (impl_->sock == INVALID_SOCKET ||
        connect(impl_->sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        SftpResult r; r.status = SftpStatus::ConnectFailed;
        r.message = "connect failed: " + ep.host + ":" + portStr;
        if (impl_->sock != INVALID_SOCKET) {
            closesocket(impl_->sock); impl_->sock = INVALID_SOCKET;
        }
        return r;
    }
    freeaddrinfo(res);

    impl_->session = libssh2_session_init();
    if (!impl_->session) { close(); return {SftpStatus::HandshakeFailed, "session_init"}; }
    libssh2_session_set_blocking(impl_->session, 0);

    int rc = 0;
    while ((rc = libssh2_session_handshake(impl_->session, (int)impl_->sock))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (rc != 0) {
        SftpResult r = mapSshErr(SftpStatus::HandshakeFailed, "handshake", impl_->session);
        close();
        return r;
    }

    // Host key verification: exact match of the full public key blob.
    {
        std::size_t keyLen = 0;
        int type = 0;
        const char* key = libssh2_session_hostkey(impl_->session, &keyLen, &type);
        if (!key || keyLen == 0) { close(); return {SftpStatus::HandshakeFailed, "no host key"}; }
        const std::string hex = Impl::hexOf(
            reinterpret_cast<const unsigned char*>(key), keyLen);
        bool trusted = false;
        for (const std::string& t : trustedHostKeyHex) {
            if (t == hex) { trusted = true; break; }
        }
        if (!trusted && policy_ == HostKeyPolicy::Strict) {
            close();
            SftpResult r; r.status = SftpStatus::HostKeyRejected;
            r.message = "unknown host key sha256-blob " + hex.substr(0, 32) + "...";
            return r;
        }
        if (!trusted && acceptedHostKeyHex) *acceptedHostKeyHex = hex;
    }

    // Authentication: private key when provided, else password.
    if (!ep.privateKeyPem.empty()) {
        while ((rc = libssh2_userauth_publickey_frommemory(
                    impl_->session, ep.user.c_str(), ep.user.size(),
                    nullptr, 0,
                    ep.privateKeyPem.data(), ep.privateKeyPem.size(),
                    ep.privateKeyPassphrase.empty()
                        ? nullptr : ep.privateKeyPassphrase.c_str()))
               == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(impl_->sock, true, 5000);
        }
    } else {
        while ((rc = libssh2_userauth_password(impl_->session, ep.user.c_str(),
                                                ep.password.c_str()))
               == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(impl_->sock, true, 5000);
        }
    }
    if (rc != 0) {
        SftpResult r = mapSshErr(SftpStatus::AuthFailed, "auth", impl_->session);
        close();
        return r;
    }

    // Start the SFTP subsystem.
    while ((impl_->sftp = libssh2_sftp_init(impl_->session)) == nullptr &&
           libssh2_session_last_errno(impl_->session) == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, false, 5000);
    }
    if (!impl_->sftp) {
        SftpResult r = mapSshErr(SftpStatus::SftpFailed, "sftp_init", impl_->session);
        close();
        return r;
    }

    state_ = State::Open;
    return {};
}

Capabilities SftpSession::capabilities() const {
    Capabilities c;
    c.structuredListing = true;   // readdir + attrs
    c.utf8Paths = true;
    c.resume = true;              // server-side seek offsets
    c.chmod = true;               // setstat permissions
    c.symlinkFollow = true;       // stat follows links
    c.execChannel = true;         // same SSH server
    c.timestampSubSecond = false; // attrs carry seconds
    return c;
}

ListingPage SftpSession::list(const std::string& dir,
                              const std::string& startAfter,
                              std::uint32_t maxEntries) {
    ListingPage page;
    if (state_ != State::Open || !impl_->sftp) {
        page.cursor = "session-not-open";
        return page;
    }

    LIBSSH2_SFTP_HANDLE* h = nullptr;
    while ((h = libssh2_sftp_opendir(impl_->sftp, dir.c_str())) == nullptr &&
           libssh2_session_last_errno(impl_->session) == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, false, 5000);
    }
    if (!h) {
        SftpResult r = mapSshErr(classifySftpErr(impl_->sftp), ("opendir " + dir).c_str(),
                                 impl_->session);
        page.cursor = r.message;
        return page;
    }

    char name[1024];
    char longentry[2048];
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    std::vector<Entry> all;
    for (;;) {
        const int n = libssh2_sftp_readdir_ex(h, name, sizeof(name),
                                              longentry, sizeof(longentry), &attrs);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(impl_->sock, false, 1000);
            continue;
        }
        if (n <= 0) break;   // 0 = end of directory
        Entry e;
        e.name.assign(name, (std::size_t)n);
        if (e.name == "." || e.name == "..") continue;
        if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            e.permissions = (std::uint32_t)attrs.permissions;
            if (LIBSSH2_SFTP_S_ISLNK(attrs.permissions)) e.type = EntryType::Symlink;
            else if (LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) e.type = EntryType::Directory;
            else if (LIBSSH2_SFTP_S_ISREG(attrs.permissions)) e.type = EntryType::File;
        }
        if ((attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) && e.type == EntryType::File) {
            e.size = attrs.filesize;
        }
        if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
            e.mtimeUnix = (std::int64_t)attrs.mtime;
        }
        all.push_back(std::move(e));
    }
    libssh2_sftp_closedir(h);

    std::sort(all.begin(), all.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });

    std::size_t i = 0;
    if (!startAfter.empty()) {
        auto it = std::upper_bound(all.begin(), all.end(), startAfter,
            [](const std::string& key, const Entry& e) { return key < e.name; });
        i = static_cast<std::size_t>(it - all.begin());
    }
    for (; i < all.size() && page.entries.size() < maxEntries; ++i) {
        page.entries.push_back(std::move(all[i]));
    }
    page.hasMore = i < all.size();
    if (page.hasMore) page.cursor = page.entries.back().name;
    return page;
}

SftpResult SftpSession::size(const std::string& remotePath, std::uint64_t* out) {
    if (out) *out = 0;
    if (state_ != State::Open || !impl_->sftp) {
        return {SftpStatus::IoError, "session not open"};
    }
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    int rc = 0;
    while ((rc = libssh2_sftp_stat(impl_->sftp, remotePath.c_str(), &attrs))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, false, 5000);
    }
    if (rc != 0) {
        return mapSshErr(classifySftpErr(impl_->sftp), ("stat " + remotePath).c_str(),
                         impl_->session);
    }
    if (out && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) *out = attrs.filesize;
    return {};
}

SftpResult SftpSession::download(const std::string& remotePath,
                                 const std::string& localPath,
                                 std::uint64_t resumeBytes,
                                 std::uint64_t* bytesMoved,
                                 const TransferControl* ctl) {
    if (bytesMoved) *bytesMoved = 0;
    if (state_ != State::Open || !impl_->sftp) {
        return {SftpStatus::IoError, "session not open"};
    }

    LIBSSH2_SFTP_HANDLE* h = nullptr;
    while ((h = libssh2_sftp_open(impl_->sftp, remotePath.c_str(),
                                  LIBSSH2_FXF_READ, 0)) == nullptr &&
           libssh2_session_last_errno(impl_->session) == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, false, 5000);
    }
    if (!h) {
        return mapSshErr(classifySftpErr(impl_->sftp),
                         ("open " + remotePath + " for read").c_str(),
                         impl_->session);
    }

    impl_->progressTotal = resumeBytes;
    {   // total = remote size (best effort; 0 = unknown)
        LIBSSH2_SFTP_ATTRIBUTES st{};
        if (libssh2_sftp_fstat(h, &st) == 0 && (st.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
            impl_->progressTotal = st.filesize;
        }
    }

    const char* mode = resumeBytes > 0 ? "ab" : "wb";
    std::FILE* f = std::fopen(localPath.c_str(), mode);
    if (!f) {
        libssh2_sftp_close(h);
        return {SftpStatus::IoError, "cannot open local " + localPath};
    }
    if (resumeBytes > 0) {
        libssh2_sftp_seek64(h, resumeBytes);
    }

    std::vector<char> buf(64 * 1024);
    SftpResult result;
    std::uint64_t moved = 0;
    for (;;) {
        if (ctlParkOrAbort(ctl)) {
            result = {SftpStatus::Canceled, "canceled by caller"};
            break;
        }
        ssize_t n = 0;
        while ((n = libssh2_sftp_read(h, buf.data(), (unsigned int)buf.size()))
               == LIBSSH2_ERROR_EAGAIN) {
            if (!waitSocket(impl_->sock, false, 10000)) {
                result = {SftpStatus::IoError, "read timeout"};
                break;
            }
        }
        if (n == 0) break;               // EOF
        if (n < 0 || !result.ok()) {
            if (result.ok()) {
                result = mapSshErr(classifySftpErr(impl_->sftp),
                                   ("read " + remotePath).c_str(), impl_->session);
            }
            break;
        }
        if (std::fwrite(buf.data(), 1, (std::size_t)n, f) != (std::size_t)n) {
            result = {SftpStatus::IoError, "local write failed: " + localPath};
            break;
        }
        moved += (std::uint64_t)n;
        if (bytesMoved) *bytesMoved = moved;
        if (impl_->onProgress) impl_->onProgress(moved, impl_->progressTotal);
    }

    if (std::fclose(f) != 0 && result.ok()) {
        result = {SftpStatus::IoError, "local close failed: " + localPath};
    }
    libssh2_sftp_close(h);
    return result;
}

SftpResult SftpSession::upload(const std::string& localPath,
                               const std::string& remotePath,
                               std::uint64_t resumeBytes,
                               std::uint64_t* bytesMoved,
                               const TransferControl* ctl) {
    if (bytesMoved) *bytesMoved = 0;
    if (state_ != State::Open || !impl_->sftp) {
        return {SftpStatus::IoError, "session not open"};
    }

    std::FILE* f = std::fopen(localPath.c_str(), "rb");
    if (!f) return {SftpStatus::IoError, "cannot open local " + localPath};
    impl_->progressTotal = resumeBytes;
    {
        struct _stat64 st{};
        if (_stat64(localPath.c_str(), &st) == 0) {
            impl_->progressTotal = (std::uint64_t)st.st_size;
        }
    }

    const long openFlags = resumeBytes > 0
        ? (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT)
        : (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC);
    LIBSSH2_SFTP_HANDLE* h = nullptr;
    while ((h = libssh2_sftp_open(impl_->sftp, remotePath.c_str(),
                                  openFlags,
                                  LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                                  LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH))
               == nullptr &&
           libssh2_session_last_errno(impl_->session) == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (!h) {
        std::fclose(f);
        return mapSshErr(classifySftpErr(impl_->sftp),
                         ("open " + remotePath + " for write").c_str(),
                         impl_->session);
    }
    if (resumeBytes > 0) {
        libssh2_sftp_seek64(h, resumeBytes);
        if (std::fseek(f, (long)resumeBytes, SEEK_SET) != 0) {
            libssh2_sftp_close(h); std::fclose(f);
            return {SftpStatus::IoError, "local seek failed: " + localPath};
        }
    }

    std::vector<char> buf(64 * 1024);
    SftpResult result;
    std::uint64_t moved = 0;
    for (;;) {
        if (ctlParkOrAbort(ctl)) {
            result = {SftpStatus::Canceled, "canceled by caller"};
            break;
        }
        const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
        if (got == 0) {
            if (std::ferror(f)) {
                result = {SftpStatus::IoError, "local read failed: " + localPath};
            }
            break;   // EOF (or error, reported above)
        }
        std::size_t off = 0;
        bool aborted = false;
        while (off < got) {
            ssize_t n = 0;
            while ((n = libssh2_sftp_write(h, buf.data() + off,
                                           (unsigned int)(got - off)))
                   == LIBSSH2_ERROR_EAGAIN) {
                if (!waitSocket(impl_->sock, true, 10000)) {
                    result = {SftpStatus::IoError, "write timeout"};
                    break;
                }
            }
            if (n < 0 || !result.ok()) {
                if (result.ok()) {
                    result = mapSshErr(classifySftpErr(impl_->sftp),
                                       ("write " + remotePath).c_str(),
                                       impl_->session);
                }
                aborted = true;
                break;
            }
            off += (std::size_t)n;
            moved += (std::uint64_t)n;
            if (bytesMoved) *bytesMoved = moved;
        }
        if (aborted || !result.ok()) break;
        if (impl_->onProgress) impl_->onProgress(moved, impl_->progressTotal);
    }

    std::fclose(f);
    libssh2_sftp_close(h);
    return result;
}

void SftpSession::setProgressCallback(
        std::function<void(std::uint64_t, std::uint64_t)> fn) {
    if (impl_ == nullptr) impl_ = new Impl();
    impl_->onProgress = std::move(fn);
}

SftpResult SftpSession::mkdir(const std::string& remotePath, std::uint32_t mode) {
    if (state_ != State::Open || !impl_->sftp) {
        return {SftpStatus::IoError, "session not open"};
    }
    int rc = 0;
    while ((rc = libssh2_sftp_mkdir(impl_->sftp, remotePath.c_str(), mode))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (rc != 0) {
        return mapSshErr(classifySftpErr(impl_->sftp),
                         ("mkdir " + remotePath).c_str(), impl_->session);
    }
    return {};
}

SftpResult SftpSession::removeFile(const std::string& remotePath) {
    if (state_ != State::Open || !impl_->sftp) {
        return {SftpStatus::IoError, "session not open"};
    }
    int rc = 0;
    while ((rc = libssh2_sftp_unlink(impl_->sftp, remotePath.c_str()))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (rc != 0) {
        return mapSshErr(classifySftpErr(impl_->sftp),
                         ("rm " + remotePath).c_str(), impl_->session);
    }
    return {};
}

SftpResult SftpSession::removeDir(const std::string& remotePath) {
    if (state_ != State::Open || !impl_->sftp) {
        return {SftpStatus::IoError, "session not open"};
    }
    int rc = 0;
    while ((rc = libssh2_sftp_rmdir(impl_->sftp, remotePath.c_str()))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (rc != 0) {
        return mapSshErr(classifySftpErr(impl_->sftp),
                         ("rmdir " + remotePath).c_str(), impl_->session);
    }
    return {};
}

SftpResult SftpSession::rename(const std::string& from, const std::string& to) {
    if (state_ != State::Open || !impl_->sftp) {
        return {SftpStatus::IoError, "session not open"};
    }
    int rc = 0;
    while ((rc = libssh2_sftp_rename_ex(impl_->sftp, from.c_str(), (unsigned int)from.size(),
                                        to.c_str(), (unsigned int)to.size(),
                                        LIBSSH2_SFTP_RENAME_OVERWRITE |
                                        LIBSSH2_SFTP_RENAME_ATOMIC |
                                        LIBSSH2_SFTP_RENAME_NATIVE))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (rc != 0) {
        return mapSshErr(classifySftpErr(impl_->sftp),
                         ("rename " + from + " -> " + to).c_str(), impl_->session);
    }
    return {};
}

SftpResult SftpSession::chmod(const std::string& remotePath, std::uint32_t mode) {
    if (state_ != State::Open || !impl_->sftp) {
        return {SftpStatus::IoError, "session not open"};
    }
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    attrs.permissions = mode;
    int rc = 0;
    while ((rc = libssh2_sftp_setstat(impl_->sftp, remotePath.c_str(), &attrs))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (rc != 0) {
        return mapSshErr(classifySftpErr(impl_->sftp),
                         ("chmod " + remotePath).c_str(), impl_->session);
    }
    return {};
}

void SftpSession::close() {
    if (impl_) {
        if (impl_->sftp) {
            libssh2_sftp_shutdown(impl_->sftp);
            impl_->sftp = nullptr;
        }
        if (impl_->session) {
            libssh2_session_disconnect(impl_->session, "bye");
            libssh2_session_free(impl_->session);
            impl_->session = nullptr;
        }
        if (impl_->sock != INVALID_SOCKET) {
            closesocket(impl_->sock);
            impl_->sock = INVALID_SOCKET;
        }
    }
    if (state_ == State::Open) state_ = State::Closed;
}

#else  // !FTPFLOW_HAVE_LIBSSH2

// Stub build: Impl stays empty so `delete impl_` is well-formed without
// the libssh2-based definition.
struct SftpSession::Impl {};

SftpSession::SftpSession(HostKeyPolicy policy) : policy_(policy) {}
SftpSession::~SftpSession() { delete impl_; }

SftpResult SftpSession::open(const SshEndpoint&, const std::vector<std::string>&,
                             std::string*) {
    return {SftpStatus::EngineUnavailable,
            "built without libssh2 (build with FTPFLOW_WITH_LIBSSH2 / --full preset)"};
}
Capabilities SftpSession::capabilities() const { return {}; }
ListingPage SftpSession::list(const std::string&, const std::string&, std::uint32_t) {
    return {};
}
SftpResult SftpSession::size(const std::string&, std::uint64_t*) {
    return {SftpStatus::EngineUnavailable, "built without libssh2"};
}
SftpResult SftpSession::download(const std::string&, const std::string&,
                                 std::uint64_t, std::uint64_t*, const TransferControl*) {
    return {SftpStatus::EngineUnavailable, "built without libssh2"};
}
SftpResult SftpSession::upload(const std::string&, const std::string&,
                               std::uint64_t, std::uint64_t*, const TransferControl*) {
    return {SftpStatus::EngineUnavailable, "built without libssh2"};
}
void SftpSession::setProgressCallback(
        std::function<void(std::uint64_t, std::uint64_t)>) {}
SftpResult SftpSession::mkdir(const std::string&, std::uint32_t) {
    return {SftpStatus::EngineUnavailable, "built without libssh2"};
}
SftpResult SftpSession::removeFile(const std::string&) {
    return {SftpStatus::EngineUnavailable, "built without libssh2"};
}
SftpResult SftpSession::removeDir(const std::string&) {
    return {SftpStatus::EngineUnavailable, "built without libssh2"};
}
SftpResult SftpSession::rename(const std::string&, const std::string&) {
    return {SftpStatus::EngineUnavailable, "built without libssh2"};
}
SftpResult SftpSession::chmod(const std::string&, std::uint32_t) {
    return {SftpStatus::EngineUnavailable, "built without libssh2"};
}
void SftpSession::close() {}

#endif

}  // namespace ftpflow
