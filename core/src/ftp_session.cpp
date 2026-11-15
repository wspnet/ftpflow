// ftpflow -- FTP/FTPS session implementation (libcurl, Schannel).
#include "ftpflow/ftp_session.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(FTPFLOW_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace ftpflow {

#if defined(FTPFLOW_HAVE_CURL)

namespace {

struct CurlGlobal {
    bool ok = false;
    CurlGlobal() { ok = ::curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }
    ~CurlGlobal() { if (ok) ::curl_global_cleanup(); }
};

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ---- LIST line parsers ------------------------------------------------------

bool parseUnixListLine(const std::string& line, Entry& e) {
    // "-rw-r--r-- 1 owner group 12345 Jan 12 09:33 name" / "drwx... d name"
    // Exactly 8 fields precede the name (perms links owner group size mon
    // day time-or-year); the name is the tail, spaces included.
    if (line.size() < 10) return false;
    const char c0 = line[0];
    if (c0 == 'd') e.type = EntryType::Directory;
    else if (c0 == 'l') e.type = EntryType::Symlink;
    else if (c0 == '-') e.type = EntryType::File;
    else return false;

    // Tokenize; unix lines have >= 9 fields, we stop after 8.
    std::vector<std::string> tok;
    std::size_t i = 0;
    while (i < line.size() && tok.size() < 8) {
        while (i < line.size() && line[i] == ' ') ++i;
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > start) tok.push_back(line.substr(start, i - start));
    }
    if (tok.size() < 8) return false;
    // Advance past the spaces separating field 8 from the name.
    while (i < line.size() && line[i] == ' ') ++i;
    const std::string name = trim(line.substr(i));
    if (name.empty()) return false;
    e.name = name;
    e.size = std::strtoull(tok[4].c_str(), nullptr, 10);
    // Date: tok[5] month, tok[6] day, tok[7] year-or-time. Keep mtime 0:
    // parsing server-local dates without a tz is a trap we avoid; the UI
    // shows it as "-" until MLSD-style data lands.
    return true;
}

bool parseDosListLine(const std::string& line, Entry& e) {
    // "01-12-26  09:33PM                  12345 name"
    if (line.size() < 18) return false;
    const std::string date = line.substr(0, 10);
    if (date[2] != '-' || date[5] != '-') return false;
    for (char c : date)
        if (c != '-' && (c < '0' || c > '9')) return false;
    const std::string rest = trim(line.substr(10));
    std::size_t sp = rest.find(' ');
    if (sp == std::string::npos) return false;
    const std::string name = trim(rest.substr(sp + 1));
    if (name.empty()) return false;
    const std::string sizeTok = rest.substr(0, sp);
    if (sizeTok == "<DIR>") {
        e.type = EntryType::Directory;
        e.size = 0;
    } else {
        e.type = EntryType::File;
        e.size = std::strtoull(sizeTok.c_str(), nullptr, 10);
    }
    e.name = name;
    return true;
}

bool parseListLine(const std::string& line, Entry& e) {
    if (line.empty()) return false;
    if (line == "." || line == "..") return false;
    if (parseUnixListLine(line, e)) return true;
    return parseDosListLine(line, e);
}

// ---- libcurl callbacks -------------------------------------------------------

std::size_t appendToString(char* ptr, std::size_t sz, std::size_t nm,
                           void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, sz * nm);
    return sz * nm;
}

std::size_t writeToFile(char* ptr, std::size_t sz, std::size_t nm,
                        void* userdata) {
    std::FILE* f = static_cast<std::FILE*>(userdata);
    return std::fwrite(ptr, 1, sz * nm, f);
}

std::size_t readFromFile(char* ptr, std::size_t sz, std::size_t nm,
                         void* userdata) {
    std::FILE* f = static_cast<std::FILE*>(userdata);
    return std::fread(ptr, 1, sz * nm, f);
}

// Uploads must not inherit a WRITEDATA pointing at a dead stack buffer from
// a previous call (open()'s PWD sink); route any stray body into the void.
std::size_t discardData(char*, std::size_t sz, std::size_t nm, void*) {
    return sz * nm;
}

// FTPFLOW_FTP_VERBOSE=1 -> CURLOPT_VERBOSE trace on stderr. Off by default;
// this is a developer escape hatch, not a logging framework.
int traceDump(CURL*, curl_infotype type, char* data, std::size_t size, void*) {
    if (type == CURLINFO_TEXT || type == CURLINFO_HEADER_IN ||
        type == CURLINFO_HEADER_OUT || type == CURLINFO_DATA_IN ||
        type == CURLINFO_DATA_OUT) {
        std::fwrite(data, 1, size, stderr);
        if (size && data[size - 1] != '\n') std::fputc('\n', stderr);
    }
    return 0;
}

bool wantVerbose() {
    const char* v = std::getenv("FTPFLOW_FTP_VERBOSE");
    return v && *v && *v != '0';
}

// Progress/control context handed to libcurl's XFERINFO hook. A plain
// file-local struct (not FtpSession::Impl, which is private) so the
// callback in this namespace can touch it.
struct XferCtx {
    std::function<void(std::uint64_t, std::uint64_t)> onProgress;
    const TransferControl* ctl = nullptr;   // set per transfer call
    volatile bool isUpload = false;
};

// libcurl progress hook: parks while paused, returns nonzero to abort on
// cancel (curl maps that to CURLE_ABORTED_BY_CALLBACK). NOTE: the client
// pointer is the FIRST argument (curl_xferinfo_callback layout).
int xferInfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
             curl_off_t ultotal, curl_off_t ulnow) {
    auto* ctx = static_cast<XferCtx*>(clientp);
    if (ctx && ctx->ctl) {
        while (ctx->ctl->pause && !ctx->ctl->cancel) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (ctx->ctl->cancel) return 1;
    }
    if (ctx && ctx->onProgress) {
        const bool up = ctx->isUpload;
        const std::uint64_t total = up ? (std::uint64_t)ultotal
                                       : (std::uint64_t)dltotal;
        const std::uint64_t now = up ? (std::uint64_t)ulnow
                                     : (std::uint64_t)dlnow;
        ctx->onProgress(now, total);
    }
    return 0;
}

// Builds "scheme://host[:port] + normalized path". The path always gets a
// leading '/'; `trailingSlash` appends one for directory listings.
std::string makeUrl(const FtpOptions& opt, const std::string& path,
                    bool trailingSlash) {
    const std::string scheme = opt.implicitTls ? "ftps" : "ftp";
    std::string url = scheme + "://" + opt.host;
    if (opt.port != 0 && opt.port != 21 && opt.port != 990)
        url += ":" + std::to_string(opt.port);
    std::string p = path;
    if (p.empty() || p[0] != '/') p = "/" + p;
    if (trailingSlash && p.back() != '/') p += "/";
    return url + p;
}

}  // namespace

struct FtpSession::Impl {
    CURL* easy = nullptr;
    char errBuf[CURL_ERROR_SIZE] = {0};
    std::string lastErr;
    std::string listBuf;
    XferCtx xfer;   // progress/control state shared with the XFERINFO hook
};

FtpSession::FtpSession() {
    static CurlGlobal g;
    if (g.ok) impl_ = new Impl();
}

FtpSession::~FtpSession() {
    close();
    delete impl_;
}

void FtpSession::close() {
    if (impl_ && impl_->easy) {
        ::curl_easy_cleanup(impl_->easy);
        impl_->easy = nullptr;
    }
    opened_ = false;
}

FtpResult FtpSession::open(const FtpOptions& opt) {
    if (impl_ == nullptr)
        return {FtpStatus::EngineUnavailable,
                "built without libcurl (build with FTPFLOW_WITH_CURL)"};
    opt_ = opt;

    if (impl_->easy) ::curl_easy_cleanup(impl_->easy);
    impl_->easy = ::curl_easy_init();
    if (!impl_->easy) return {FtpStatus::IoError, "curl_easy_init failed"};
    ::curl_easy_setopt(impl_->easy, CURLOPT_ERRORBUFFER, impl_->errBuf);
    // Progress + control hook (needed for pause/cancel and progress events).
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOPROGRESS, 0L);
    ::curl_easy_setopt(impl_->easy, CURLOPT_XFERINFOFUNCTION, xferInfo);
    ::curl_easy_setopt(impl_->easy, CURLOPT_XFERINFODATA, &impl_->xfer);
    if (wantVerbose()) {
        ::curl_easy_setopt(impl_->easy, CURLOPT_VERBOSE, 1L);
        ::curl_easy_setopt(impl_->easy, CURLOPT_DEBUGFUNCTION, traceDump);
    }

    // Base URL: scheme://host[:port]
    std::string scheme = opt.implicitTls ? "ftps" : "ftp";
    std::string url = scheme + "://" + opt.host;
    if (opt.port != 0 && opt.port != 21 && opt.port != 990)
        url += ":" + std::to_string(opt.port);

    ::curl_easy_setopt(impl_->easy, CURLOPT_URL, (url + "/").c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_USERPWD,
                       (opt.user + ":" + opt.password).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_CONNECTTIMEOUT_MS,
                       static_cast<long>(opt.connectTimeoutMs));
    ::curl_easy_setopt(impl_->easy, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L);
    if (opt.tls && !opt.implicitTls) {
        ::curl_easy_setopt(impl_->easy, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
        ::curl_easy_setopt(impl_->easy, CURLOPT_FTPSSLAUTH,
                           (long)CURLFTPAUTH_TLS);
    }
    // Schannel: verify by default (CURLSSLOPT_NATIVE_CA on 7.77+).
#if LIBCURL_VERSION_NUM >= 0x074d00
    ::curl_easy_setopt(impl_->easy, CURLOPT_SSL_OPTIONS,
                       (long)CURLSSLOPT_NATIVE_CA);
#endif

    // Cheap auth/connect check: PWD via a NOBODY request on the root.
    std::string pwdBuf;
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEFUNCTION, appendToString);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEDATA, &pwdBuf);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 1L);
    CURLcode rc = ::curl_easy_perform(impl_->easy);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 0L);
    if (rc != CURLE_OK) {
        impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
        impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                        : ::curl_easy_strerror(rc);
        const std::string& msg = impl_->lastErr;
        FtpStatus st = rc == CURLE_COULDNT_CONNECT ? FtpStatus::ConnectFailed
                     : rc == CURLE_LOGIN_DENIED ? FtpStatus::AuthFailed
                     : FtpStatus::ConnectFailed;
        close();
        return {st, "connect/auth check (PWD): " + msg, rc};
    }
    opened_ = true;
    return {};
}

Capabilities FtpSession::capabilities() const {
    Capabilities c;
    c.structuredListing = false;   // LIST text parse; MLSD lands later
    c.utf8Paths = true;
    c.resume = true;               // REST / APPE
    c.chmod = true;                // SITE CHMOD (server dependent)
    c.symlinkFollow = true;
    c.execChannel = false;         // FTP has no exec semantics
    c.timestampSubSecond = false;
    return c;
}

ListingPage FtpSession::list(const std::string& dir) {
    ListingPage page;
    if (!opened_ || !impl_ || !impl_->easy)
        return page;  // caller checks isOpen()

    std::string d = dir.empty() ? opt_.initialPath : dir;
    impl_->listBuf.clear();
    impl_->lastErr.clear();
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL, makeUrl(opt_, d, true).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEFUNCTION, appendToString);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEDATA, &impl_->listBuf);
    // Default request for a trailing-slash URL is LIST; overriding via
    // CUSTOMREQUEST breaks curl's internal FTP state machine.
    ::curl_easy_setopt(impl_->easy, CURLOPT_DIRLISTONLY, 0L);
    CURLcode rc = ::curl_easy_perform(impl_->easy);
    if (rc != CURLE_OK) {
        impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
        impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                        : ::curl_easy_strerror(rc);
        impl_->lastErr = "list: " + impl_->lastErr;
        return page;  // caller sees an empty page; lastErr holds the cause
    }

    // Split lines and parse.
    std::size_t pos = 0;
    while (pos < impl_->listBuf.size()) {
        std::size_t nl = impl_->listBuf.find('\n', pos);
        std::string line = impl_->listBuf.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? impl_->listBuf.size() : nl + 1;
        Entry e;
        if (parseListLine(trim(line), e)) page.entries.push_back(e);
    }
    page.hasMore = false;  // whole listing in one page
    return page;
}

FtpResult FtpSession::size(const std::string& remotePath,
                           std::uint64_t* out) {
    if (out) *out = 0;
    if (!opened_ || !impl_ || !impl_->easy)
        return {FtpStatus::IoError, "session not open"};

    std::string sink;
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL, makeUrl(opt_, remotePath, false).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEFUNCTION, appendToString);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEDATA, &sink);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 1L);
    CURLcode rc = ::curl_easy_perform(impl_->easy);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 0L);
    if (rc != CURLE_OK)
        return {FtpStatus::DownloadFailed,
                std::string("SIZE: ") + ::curl_easy_strerror(rc), rc};
    double len = 0.0;
    if (::curl_easy_getinfo(impl_->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD,
                            &len) != CURLE_OK || len < 0)
        return {FtpStatus::DownloadFailed, "no content length"};
    if (out) *out = static_cast<std::uint64_t>(len);
    return {};
}

FtpResult FtpSession::download(const std::string& remotePath,
                               const std::string& localPath,
                               std::uint64_t resumeBytes,
                               std::uint64_t* bytesMoved,
                               const TransferControl* ctl) {
    if (bytesMoved) *bytesMoved = 0;
    if (!opened_ || !impl_ || !impl_->easy)
        return {FtpStatus::IoError, "session not open"};

    const char* mode = resumeBytes > 0 ? "ab" : "wb";
    std::FILE* f = std::fopen(localPath.c_str(), mode);
    if (!f) return {FtpStatus::IoError, "cannot open local " + localPath};

    impl_->xfer.ctl = ctl;
    impl_->xfer.isUpload = false;
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL, makeUrl(opt_, remotePath, false).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEFUNCTION, writeToFile);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEDATA, f);
    if (resumeBytes > 0)
        ::curl_easy_setopt(impl_->easy, CURLOPT_RESUME_FROM_LARGE,
                           static_cast<curl_off_t>(resumeBytes));
    CURLcode rc = ::curl_easy_perform(impl_->easy);
    std::fclose(f);
    impl_->xfer.ctl = nullptr;
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        if (resumeBytes > 0)
            ::curl_easy_setopt(impl_->easy, CURLOPT_RESUME_FROM_LARGE,
                               static_cast<curl_off_t>(0));
        return {FtpStatus::Canceled, "canceled by caller", rc};
    }
    if (rc != CURLE_PARTIAL_FILE && rc != CURLE_OK) {
        return {FtpStatus::DownloadFailed,
                std::string("download: ") + ::curl_easy_strerror(rc), rc};
    }
    if (bytesMoved) {
        double len = 0.0;
        ::curl_easy_getinfo(impl_->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD,
                            &len);
        if (len > 0) *bytesMoved = static_cast<std::uint64_t>(len);
    }
    if (resumeBytes > 0)
        ::curl_easy_setopt(impl_->easy, CURLOPT_RESUME_FROM_LARGE,
                           static_cast<curl_off_t>(0));
    return {};
}

FtpResult FtpSession::upload(const std::string& localPath,
                             const std::string& remotePath,
                             std::uint64_t resumeBytes,
                             std::uint64_t* bytesMoved,
                             const TransferControl* ctl) {
    if (bytesMoved) *bytesMoved = 0;
    if (!opened_ || !impl_ || !impl_->easy)
        return {FtpStatus::IoError, "session not open"};

    std::FILE* f = std::fopen(localPath.c_str(), "rb");
    if (!f) return {FtpStatus::IoError, "cannot open local " + localPath};
    std::fseek(f, 0, SEEK_END);
    const std::uint64_t fsize = static_cast<std::uint64_t>(std::ftell(f));
    std::fseek(f, static_cast<long>(resumeBytes), SEEK_SET);

    impl_->xfer.ctl = ctl;
    impl_->xfer.isUpload = true;
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL, makeUrl(opt_, remotePath, false).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_UPLOAD, 1L);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEFUNCTION, discardData);
    ::curl_easy_setopt(impl_->easy, CURLOPT_READFUNCTION, readFromFile);
    ::curl_easy_setopt(impl_->easy, CURLOPT_READDATA, f);
    ::curl_easy_setopt(impl_->easy, CURLOPT_INFILESIZE_LARGE,
                       static_cast<curl_off_t>(
                           resumeBytes > 0 ? (fsize - resumeBytes) : fsize));
    if (resumeBytes > 0)
        ::curl_easy_setopt(impl_->easy, CURLOPT_APPEND, 1L);

    CURLcode rc = ::curl_easy_perform(impl_->easy);
    std::fclose(f);
    impl_->xfer.ctl = nullptr;
    ::curl_easy_setopt(impl_->easy, CURLOPT_UPLOAD, 0L);
    if (resumeBytes > 0)
        ::curl_easy_setopt(impl_->easy, CURLOPT_APPEND, 0L);
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        return {FtpStatus::Canceled, "canceled by caller", rc};
    }
    if (rc != CURLE_PARTIAL_FILE && rc != CURLE_OK) {
        return {FtpStatus::UploadFailed,
                std::string("upload: ") + ::curl_easy_strerror(rc), rc};
    }
    if (bytesMoved)
        *bytesMoved = resumeBytes > 0 ? (fsize - resumeBytes) : fsize;
    return {};
}

void FtpSession::setProgressCallback(
        std::function<void(std::uint64_t, std::uint64_t)> fn) {
    if (impl_) impl_->xfer.onProgress = std::move(fn);
}

// ---- metadata ops (MKD / DELE / RMD / RNFR+RNTO) -------------------------
//
// libcurl has no first-class verbs for these, so we drive them through
// CURLOPT_QUOTE (commands issued verbatim after login/CWD, before the
// transfer). Empirically (pyftpdlib wire log): a NOBODY transfer on the
// root URL + one QUOTE command runs exactly our verb and nothing else;
// CURLOPT_CUSTOMREQUEST is NOT usable here -- curl CWDs into the URL
// path first and aborts before the custom request when the path does
// not exist yet (which is precisely the MKD case).

#if defined(FTPFLOW_HAVE_CURL)
FtpResult FtpSession::quoteCommand(const std::string& cmd) {
    if (!impl_ || !impl_->easy) return {FtpStatus::IoError, "not open"};
    impl_->lastErr.clear();
    struct curl_slist* quote = nullptr;
    quote = ::curl_slist_append(quote, cmd.c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL,
                       makeUrl(opt_, "/", true).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_QUOTE, quote);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 1L);
    CURLcode rc = ::curl_easy_perform(impl_->easy);
    ::curl_slist_free_all(quote);
    ::curl_easy_setopt(impl_->easy, CURLOPT_QUOTE, nullptr);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 0L);
    if (rc != CURLE_OK) {
        impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
        impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                        : ::curl_easy_strerror(rc);
        impl_->lastErr = cmd.substr(0, cmd.find(' ')) + ": " + impl_->lastErr;
        return {FtpStatus::IoError, impl_->lastErr, rc};
    }
    return {};
}
#endif

FtpResult FtpSession::mkdir(const std::string& remotePath) {
#if defined(FTPFLOW_HAVE_CURL)
    return quoteCommand("MKD " + remotePath);
#else
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
#endif
}

FtpResult FtpSession::removeFile(const std::string& remotePath) {
#if defined(FTPFLOW_HAVE_CURL)
    return quoteCommand("DELE " + remotePath);
#else
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
#endif
}

FtpResult FtpSession::removeDir(const std::string& remotePath) {
#if defined(FTPFLOW_HAVE_CURL)
    return quoteCommand("RMD " + remotePath);
#else
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
#endif
}

FtpResult FtpSession::rename(const std::string& from, const std::string& to) {
#if defined(FTPFLOW_HAVE_CURL)
    if (!impl_ || !impl_->easy) return {FtpStatus::IoError, "not open"};
    impl_->lastErr.clear();
    // RNFR + RNTO must run as one sequence, so both commands share one
    // QUOTE list (curl issues them in order and aborts on the first
    // failure).
    struct curl_slist* quote = nullptr;
    quote = ::curl_slist_append(quote, ("RNFR " + from).c_str());
    quote = ::curl_slist_append(quote, ("RNTO " + to).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL,
                       makeUrl(opt_, "/", true).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_QUOTE, quote);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 1L);
    CURLcode rc = ::curl_easy_perform(impl_->easy);
    ::curl_slist_free_all(quote);
    ::curl_easy_setopt(impl_->easy, CURLOPT_QUOTE, nullptr);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 0L);
    if (rc != CURLE_OK) {
        impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
        impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                        : ::curl_easy_strerror(rc);
        impl_->lastErr = "rename: " + impl_->lastErr;
        return {FtpStatus::IoError, impl_->lastErr, rc};
    }
    return {};
#else
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
#endif
}

std::string FtpSession::lastError() const {
    return impl_ ? impl_->lastErr : std::string();
}

#else  // !FTPFLOW_HAVE_CURL

struct FtpSession::Impl {};

FtpSession::FtpSession() {}
FtpSession::~FtpSession() { delete impl_; }
void FtpSession::close() { opened_ = false; }

FtpResult FtpSession::open(const FtpOptions&) {
    return {FtpStatus::EngineUnavailable,
            "built without libcurl (build with FTPFLOW_WITH_CURL / --full)"};
}

Capabilities FtpSession::capabilities() const {
    Capabilities c;
    return c;
}

ListingPage FtpSession::list(const std::string&) { return {}; }

FtpResult FtpSession::size(const std::string&, std::uint64_t*) {
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
}

FtpResult FtpSession::download(const std::string&, const std::string&,
                               std::uint64_t, std::uint64_t*,
                               const TransferControl*) {
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
}

FtpResult FtpSession::upload(const std::string&, const std::string&,
                             std::uint64_t, std::uint64_t*,
                             const TransferControl*) {
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
}

void FtpSession::setProgressCallback(
        std::function<void(std::uint64_t, std::uint64_t)>) {}
std::string FtpSession::lastError() const { return {}; }

#endif

}  // namespace ftpflow
