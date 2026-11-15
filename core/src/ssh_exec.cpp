// ftpflow -- SSH exec channel implementation (libssh2).
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// Without FTPFLOW_HAVE_LIBSSH2 the whole file degrades to "engine
// unavailable" stubs, so the CLI/tests always link.
#include "ftpflow/ssh_exec.hpp"

#include <chrono>
#include <cstring>

#if defined(FTPFLOW_HAVE_LIBSSH2)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh2.h>
#else
// Stub build: nothing to include.
#endif

namespace ftpflow {

#if defined(FTPFLOW_HAVE_LIBSSH2)

struct SshExec::Impl {
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_CHANNEL* channel = nullptr;
    SOCKET sock = INVALID_SOCKET;
    std::vector<std::uint8_t> pendingWrite;  // unwritten stdin backlog

    std::function<void(const void*, std::size_t)> onStdout;
    std::function<void(const void*, std::size_t)> onStderr;

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

// Waits on a socket until readable/writable, bounded by timeoutMs.
bool waitSocket(SOCKET s, bool writable, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv{(long)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000)};
    const int rc = select(0, writable ? nullptr : &fds,
                          writable ? &fds : nullptr, nullptr, &tv);
    return rc > 0;
}

SshResult mapSshErr(SshStatus st, const char* what, LIBSSH2_SESSION* sess) {
    SshResult r;
    r.status = st;
    int err = 0;
    std::string msg;
    if (sess) {
        err = libssh2_session_last_errno(sess);
        char* raw = nullptr;  // points at libssh2-internal storage when ok
        libssh2_session_last_error(sess, &raw, nullptr, 0);
        if (raw) msg = raw;
    }
    r.libssh2Errno = err;
    r.message = std::string(what) + (msg.empty() ? "" : ": ") + msg;
    return r;
}

}  // namespace

SshExec::SshExec(HostKeyPolicy policy) : policy_(policy) {
    static WsaGuard g_wsa;  // process-wide Winsock init
}

SshExec::~SshExec() { close(); delete impl_; }

SshResult SshExec::open(const SshEndpoint& ep,
                        const std::string& command,
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
        SshResult r; r.status = SshStatus::ConnectFailed;
        r.message = "cannot resolve " + ep.host;
        return r;
    }
    impl_->sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (impl_->sock == INVALID_SOCKET ||
        connect(impl_->sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        SshResult r; r.status = SshStatus::ConnectFailed;
        r.message = "connect failed: " + ep.host + ":" + portStr;
        if (impl_->sock != INVALID_SOCKET) { closesocket(impl_->sock); impl_->sock = INVALID_SOCKET; }
        return r;
    }
    freeaddrinfo(res);

    impl_->session = libssh2_session_init();
    if (!impl_->session) { close(); return {SshStatus::HandshakeFailed, "session_init"}; }
    libssh2_session_set_blocking(impl_->session, 0);

    int rc = 0;
    while ((rc = libssh2_session_handshake(impl_->session, (int)impl_->sock))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (rc != 0) {
        SshResult r = mapSshErr(SshStatus::HandshakeFailed, "handshake", impl_->session);
        close();
        return r;
    }

    // Host key verification: exact match of the full public key blob.
    {
        std::size_t keyLen = 0;
        int type = 0;
        const char* key = libssh2_session_hostkey(impl_->session, &keyLen, &type);
        if (!key || keyLen == 0) { close(); return {SshStatus::HandshakeFailed, "no host key"}; }
        const std::string hex = Impl::hexOf(
            reinterpret_cast<const unsigned char*>(key), keyLen);
        bool trusted = false;
        for (const std::string& t : trustedHostKeyHex) {
            if (t == hex) { trusted = true; break; }
        }
        if (!trusted && policy_ == HostKeyPolicy::Strict) {
            close();
            SshResult r; r.status = SshStatus::HostKeyRejected;
            r.message = "unknown host key sha256-blob " + hex.substr(0, 32) + "...";
            return r;
        }
        if (!trusted && acceptedHostKeyHex) *acceptedHostKeyHex = hex;
    }

    // Authentication: private key when provided, else password.
    if (!ep.privateKeyPem.empty()) {
        // publickeyfiledata = nullptr: libssh2 extracts the public key
        // from the private key blob itself.
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
        SshResult r = mapSshErr(SshStatus::AuthFailed, "auth", impl_->session);
        close();
        return r;
    }

    while ((impl_->channel = libssh2_channel_open_session(impl_->session))
           == nullptr && libssh2_session_last_errno(impl_->session) == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, false, 5000);
    }
    if (!impl_->channel) {
        SshResult r = mapSshErr(SshStatus::ChannelFailed, "open channel", impl_->session);
        close();
        return r;
    }

    while ((rc = libssh2_channel_exec(impl_->channel, command.c_str()))
           == LIBSSH2_ERROR_EAGAIN) {
        waitSocket(impl_->sock, true, 5000);
    }
    if (rc != 0) {
        SshResult r = mapSshErr(SshStatus::ChannelFailed, "exec", impl_->session);
        close();
        return r;
    }

    state_ = State::Open;
    exitCode_ = -1;
    return {};
}

SshResult SshExec::write(const void* data, std::size_t len, int timeoutMs) {
    if (state_ != State::Open || !impl_ || !impl_->channel) {
        return {SshStatus::Closed, "channel not open"};
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    impl_->pendingWrite.insert(impl_->pendingWrite.end(), p, p + len);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (!impl_->pendingWrite.empty()) {
        auto n = libssh2_channel_write(impl_->channel,
            reinterpret_cast<const char*>(impl_->pendingWrite.data()),
            impl_->pendingWrite.size());
        if (n == LIBSSH2_ERROR_EAGAIN) {
            if (std::chrono::steady_clock::now() > deadline) {
                return {SshStatus::IoError, "write timeout"};
            }
            waitSocket(impl_->sock, true, 100);
            continue;
        }
        if (n < 0) {
            return mapSshErr(SshStatus::IoError, "channel write", impl_->session);
        }
        impl_->pendingWrite.erase(impl_->pendingWrite.begin(),
                                  impl_->pendingWrite.begin() + n);
        stats_.bytesWritten += (std::uint64_t)n;
    }
    return {};
}

void SshExec::setStdoutSink(std::function<void(const void*, std::size_t)> fn) {
    if (impl_ == nullptr) impl_ = new Impl();
    impl_->onStdout = std::move(fn);
}
void SshExec::setStderrSink(std::function<void(const void*, std::size_t)> fn) {
    if (impl_ == nullptr) impl_ = new Impl();
    impl_->onStderr = std::move(fn);
}

bool SshExec::pump(int timeoutMs) {
    if (state_ != State::Open || !impl_ || !impl_->channel) return false;

    char buf[64 * 1024];
    bool gotData = true;
    while (gotData) {
        gotData = false;
        for (int stream = 0; stream <= 1; ++stream) {  // 0 = stdout, 1 = stderr
            auto n = (stream == 0)
                ? libssh2_channel_read(impl_->channel, buf, sizeof(buf))
                : libssh2_channel_read_stderr(impl_->channel, buf, sizeof(buf));
            if (n == LIBSSH2_ERROR_EAGAIN) continue;
            if (n > 0) {
                stats_.bytesRead += (std::uint64_t)n;
                if (stream == 0 && impl_->onStdout) impl_->onStdout(buf, (std::size_t)n);
                if (stream == 1 && impl_->onStderr) impl_->onStderr(buf, (std::size_t)n);
                gotData = true;
            }
        }
    }

    if (libssh2_channel_eof(impl_->channel)) {
        // The exit status arrives in a channel-close message that must be
        // consumed on a non-blocking session. Loop wait_closed until it
        // stops returning EAGAIN, then read the status.
        int wc;
        while ((wc = libssh2_channel_wait_closed(impl_->channel))
               == LIBSSH2_ERROR_EAGAIN) {
            waitSocket(impl_->sock, false, 200);
        }
        exitCode_ = libssh2_channel_get_exit_status(impl_->channel);
        close();
        return false;
    }
    waitSocket(impl_->sock, false, timeoutMs);
    return true;
}

void SshExec::close() {
    if (impl_) {
        if (impl_->channel) {
            libssh2_channel_send_eof(impl_->channel);
            libssh2_channel_wait_closed(impl_->channel);
            libssh2_channel_free(impl_->channel);
            impl_->channel = nullptr;
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
        impl_->pendingWrite.clear();
    }
    if (state_ == State::Open) state_ = State::Closed;
}

int SshExec::exitCode() const { return exitCode_; }

#else  // !FTPFLOW_HAVE_LIBSSH2

// Stub build: Impl stays empty so `delete impl_` (always nullptr here) is
// well-formed without the libssh2-based definition.
struct SshExec::Impl {};

SshExec::SshExec(HostKeyPolicy policy) : policy_(policy) {}
SshExec::~SshExec() { delete impl_; }

SshResult SshExec::open(const SshEndpoint&, const std::string&,
                        const std::vector<std::string>&, std::string*) {
    return {SshStatus::EngineUnavailable,
            "built without libssh2 (build with FTPFLOW_WITH_LIBSSH2 / --full preset)"};
}
SshResult SshExec::write(const void*, std::size_t, int) {
    return {SshStatus::EngineUnavailable, "built without libssh2"};
}
bool SshExec::pump(int) { return false; }
void SshExec::close() {}
int SshExec::exitCode() const { return -1; }

#endif

}  // namespace ftpflow
