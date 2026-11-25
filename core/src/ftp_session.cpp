// ftpflow -- FTP/FTPS session implementation (libcurl, Schannel).
#include "ftpflow/ftp_session.hpp"
#include "ftpflow/text_codec.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

// Parse the "Jan 12 09:33" / "Jan 12 2024" tail of a Unix LIST line into an
// epoch timestamp (seconds, local time). Returns 0 on any parse error. This is
// a pragmatic best-effort: LIST carries no timezone, so we interpret the
// server-local stamp in the client's local zone (right for most self-hosted
// / LAN servers). MLSD is still preferred when available.
std::int64_t parseUnixListTime(const std::string& mon, const std::string& day,
                               const std::string& yearOrTime) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int m = -1;
    for (int k = 0; k < 12; ++k)
        if (mon.size() == 3 && mon[0] == months[k][0] &&
            mon[1] == months[k][1] && mon[2] == months[k][2]) { m = k + 1; break; }
    if (m < 0) return 0;
    int d = 0;
    for (char c : day) {
        if (c < '0' || c > '9') return 0;
        d = d * 10 + (c - '0');
    }
    if (d < 1 || d > 31) return 0;

    int year = 0, hh = 0, mm = 0;
    const bool hasColon = yearOrTime.find(':') != std::string::npos;
    if (hasColon) {
        // "HH:MM" -> current year.
        if (yearOrTime.size() < 4 || yearOrTime[2] != ':') return 0;
        hh = (yearOrTime[0] - '0') * 10 + (yearOrTime[1] - '0');
        mm = (yearOrTime[3] - '0') * 10 + (yearOrTime[4] - '0');
        const std::time_t now = std::time(nullptr);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        year = lt.tm_year + 1900;
    } else {
        // "YYYY".
        if (yearOrTime.size() != 4) return 0;
        for (char c : yearOrTime)
            if (c < '0' || c > '9') return 0;
        year = (yearOrTime[0] - '0') * 1000 + (yearOrTime[1] - '0') * 100 +
               (yearOrTime[2] - '0') * 10 + (yearOrTime[3] - '0');
    }
    if (hh > 23 || mm > 59) return 0;

    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = hh;
    t.tm_min = mm;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    const std::time_t ts = std::mktime(&t);
    return ts == (std::time_t)-1 ? 0 : (std::int64_t)ts;
}

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
    // Date: tok[5] month, tok[6] day, tok[7] year-or-time. Best-effort local
    // parse (LIST has no timezone); MLSD is preferred and carries real UTC.
    e.mtimeUnix = parseUnixListTime(tok[5], tok[6], tok[7]);
    return true;
}

bool parseDosListLine(const std::string& line, Entry& e) {
    // IIS / DOS FTP listing, four whitespace-separated fields followed by a
    // name that may contain spaces:
    //   "08-26-26  11:51AM       <DIR>          Areas"
    //   "08-26-26  11:50AM                   49 PrecompiledApp.config"
    //   "01-12-26  09:33PM                 12345 name with spaces"
    if (line.size() < 18) return false;
    const std::string date = line.substr(0, 8);
    if (date.size() < 8 || date[2] != '-' || date[5] != '-') return false;
    for (char c : date)
        if (c != '-' && (c < '0' || c > '9')) return false;

    // Tokenize the remainder after the fixed date field.
    const std::string rest = trim(line.substr(8));
    std::vector<std::string> tok;
    std::size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && rest[i] == ' ') ++i;
        std::size_t start = i;
        while (i < rest.size() && rest[i] != ' ') ++i;
        if (i > start) tok.push_back(rest.substr(start, i - start));
    }
    // Need at least: [time, size-or-<DIR>, name...]
    if (tok.size() < 3) return false;

    // tok[0] is the time ("11:51AM" / "09:33PM"); tok[1] is "<DIR>" or a
    // decimal size; tok[2..] join into the name (may contain spaces).
    const std::string& sizeTok = tok[1];
    if (sizeTok == "<DIR>") {
        e.type = EntryType::Directory;
        e.size = 0;
    } else {
        // Reject a non-numeric token (e.g. a stray listing line).
        for (char c : sizeTok)
            if (c < '0' || c > '9') return false;
        e.type = EntryType::File;
        e.size = std::strtoull(sizeTok.c_str(), nullptr, 10);
    }
    std::string name;
    for (std::size_t k = 2; k < tok.size(); ++k) {
        if (!name.empty()) name += ' ';
        name += tok[k];
    }
    if (name.empty()) return false;
    e.name = name;
    // Best-effort mtime from the DOS date/time fields (local time, no tz).
    // date = "MM-DD-YY"; tok[0] = "HH:MM" + optional "AM"/"PM".
    {
        auto d2 = [&](std::size_t i) { return (date[i] - '0') * 10 + (date[i + 1] - '0'); };
        const int mo = d2(0), dd = d2(3), yy = d2(6);
        const std::string& t = tok[0];
        if (mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && t.size() >= 4 &&
            t[2] == ':') {
            int hh = (t[0] - '0') * 10 + (t[1] - '0');
            int mm = (t[3] - '0') * 10 + (t[4] - '0');
            bool pm = false;
            for (char c : t)
                if (c == 'P' || c == 'p') pm = true;
            if (pm && hh < 12) hh += 12;
            if (!pm && hh == 12) hh = 0;
            if (hh <= 23 && mm <= 59) {
                const int year = 2000 + yy;
                std::tm tm{};
                tm.tm_year = year - 1900;
                tm.tm_mon = mo - 1;
                tm.tm_mday = dd;
                tm.tm_hour = hh;
                tm.tm_min = mm;
                tm.tm_isdst = -1;
                const std::time_t ts = std::mktime(&tm);
                if (ts != (std::time_t)-1) e.mtimeUnix = (std::int64_t)ts;
            }
        }
    }
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

bool wantVerbose();   // defined just below

// During the login probe we collect the server's control-channel responses
// (the 220/230 greeting). A Chinese IIS serves that greeting in GBK, which
// is a reliable signal that path bytes on this server must be GBK, not
// UTF-8. Used only while FtpOptions::encoding == "auto" is still unresolved:
// list() can only lock GBK after seeing a Chinese *filename*, so a directory
// containing only ASCII names would otherwise leave "auto" in place and a
// brand-new Chinese-named upload would send UTF-8 bytes that the server
// rejects at STOR (CURLE_UPLOAD_FAILED, "Upload failed ... before it took off").
int collectCtrlResponse(CURL*, curl_infotype type, char* data,
                        std::size_t size, void* userp) {
    if (type == CURLINFO_HEADER_IN && data && size && userp)
        static_cast<std::string*>(userp)->append(data, size);
    if (wantVerbose()) return traceDump(nullptr, type, data, size, nullptr);
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
    std::uint64_t lastBytes = 0;   // for chunk-delta rate limiting
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
        // Chunk-delta rate limiting: throttle on the bytes moved since the
        // last callback so curl's transfer obeys the global limit.
        if (ctx->ctl && now > ctx->lastBytes) {
            throttle(ctx->ctl, now - ctx->lastBytes);
        }
        ctx->lastBytes = now;
        ctx->onProgress(now, total);
    }
    return 0;
}

// Builds "scheme://host[:port] + normalized path". The path always gets a
// leading '/'; `trailingSlash` appends one for directory listings.
//
// The path arrives as UTF-8 (what the UI holds and what list() produced).
// FTP has no mandated filename encoding, so the bytes curl puts on the wire
// for CWD/LIST/DELE/etc. must match what the server expects: on a GBK box
// (Chinese IIS) that is GBK bytes, not UTF-8. `encoding` (from FtpOptions)
// drives the reverse conversion via fromUtf8().
std::string makeUrl(const FtpOptions& opt, const std::string& path,
                    bool trailingSlash) {
    const std::string scheme = opt.implicitTls ? "ftps" : "ftp";
    std::string url = scheme + "://" + opt.host;
    if (opt.port != 0 && opt.port != 21 && opt.port != 990)
        url += ":" + std::to_string(opt.port);
    // UTF-8 path -> server-encoding bytes, then percent-encode anything that
    // is not a URL-safe path byte. Curl decodes %XX back to raw bytes before
    // issuing CWD, so a GBK name survives the URL round-trip intact. Bytes
    // that are already ASCII (e.g. '/', alphanumerics) pass through as-is.
    const std::string p = fromUtf8(path, opt.encoding);
    std::string esc;
    esc.reserve(p.size() * 3);
    for (unsigned char c : p) {
        if (c == '/' || c == '.') {
            esc.push_back((char)c);
        } else if (std::isalnum(c) || c == '-' || c == '_' || c == '~') {
            esc.push_back((char)c);
        } else {
            static const char* hex = "0123456789ABCDEF";
            esc.push_back('%');
            esc.push_back(hex[c >> 4]);
            esc.push_back(hex[c & 0xF]);
        }
    }
    std::string out = url;
    if (esc.empty() || esc[0] != '/') out += "/";
    out += esc;
    if (trailingSlash && (out.empty() || out.back() != '/')) out += "/";
    return out;
}

}  // namespace

// ---- MLSD (RFC 3659) public parsing helpers --------------------------------

// Days from civil date (Y/M/D) to days since epoch (Howard Hinnant's
// days_from_civil), valid for the full int32 range around the epoch. MLSD
// timestamps are UTC, so no local-time adjustment.
namespace {
std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                 // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     // [0, 146096]
    return (std::int64_t)era * 146097 + (std::int64_t)doe - 719468;
}
}  // namespace

std::int64_t parseMlsdTimestamp(const std::string& s) {
    if (s.size() < 14) return 0;
    auto d2 = [&](std::size_t i) { return (s[i] - '0') * 10 + (s[i + 1] - '0'); };
    for (std::size_t i = 0; i < 14; ++i)
        if (s[i] < '0' || s[i] > '9') return 0;
    const int year = d2(0) * 100 + d2(2);
    const int mon = d2(4);
    const int day = d2(6);
    const int hh = d2(8);
    const int mm = d2(10);
    const int ss = d2(12);
    if (mon < 1 || mon > 12 || day < 1 || day > 31) return 0;
    if (hh > 23 || mm > 59 || ss > 60) return 0;
    const std::int64_t days = daysFromCivil(year, (unsigned)mon, (unsigned)day);
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

bool parseMlsdLine(const std::string& line, Entry& e) {
    // The separator is ";" + a space: MLSD facts are always ';'-joined and
    // the name begins after the first "; " sequence. This distinguishes the
    // fact delimiter from a name that contains ';'.
    const std::size_t sep = line.find("; ");
    if (sep == std::string::npos) return false;
    const std::string facts = line.substr(0, sep);
    const std::string name = trim(line.substr(sep + 2));
    if (name.empty() || name == "." || name == "..") return false;

    e = Entry{};
    e.name = name;
    // Walk the ';'-separated facts.
    std::size_t i = 0;
    while (i < facts.size()) {
        std::size_t semi = facts.find(';', i);
        const std::string fact = facts.substr(
            i, semi == std::string::npos ? std::string::npos : semi - i);
        const std::size_t eq = fact.find('=');
        if (eq != std::string::npos) {
            const std::string key = fact.substr(0, eq);
            const std::string val = fact.substr(eq + 1);
            if (key == "type") {
                if (val == "file") e.type = EntryType::File;
                else if (val == "dir") e.type = EntryType::Directory;
                else if (val == "cdir" || val == "pdir") e.type = EntryType::Directory;
                else if (val.find("link") != std::string::npos ||
                         val == "OS.unix=symlink") e.type = EntryType::Symlink;
                else e.type = EntryType::Other;
            } else if (key == "size") {
                e.size = std::strtoull(val.c_str(), nullptr, 10);
            } else if (key == "modify") {
                e.mtimeUnix = parseMlsdTimestamp(val);
            } else if (key == "UNIX.mode") {
                e.permissions = (std::uint32_t)std::strtoul(val.c_str(), nullptr, 8);
            }
        }
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return true;
}

struct FtpSession::Impl {
    CURL* easy = nullptr;
    char errBuf[CURL_ERROR_SIZE] = {0};
    std::string lastErr;
    std::string listBuf;
    int lastCurlErr = CURLE_OK;   // most recent curl result code
    bool mlsdOk = false;          // server accepted MLSD (probed on first list)
    // Physical root the server drops us into after login (CURLINFO_FTP_ENTRY_PATH).
    // On IIS user isolation this is the user's virtual root (e.g. /zmailvip163com8),
    // NOT "/". We prefix it to every absolute path we hand to the server so that
    // `CWD /wwwroot` (which the server resolves against the *physical* root and
    // would reject with 550) becomes `CWD /zmailvip163com8/wwwroot`.
    std::string homeDir;
    // Scratch buffer used only during open()'s login probe to capture the
    // control-channel greeting for auto encoding detection.
    std::string ctrlGreeting;
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
    // When the encoding is still "auto", inspect the login greeting to
    // resolve it up front. Otherwise a Chinese upload into an ASCII-only
    // directory sends UTF-8 bytes to a GBK server and fails at STOR.
    const bool probeGreeting = (opt.encoding == "auto");
    if (probeGreeting) impl_->ctrlGreeting.clear();
    if (probeGreeting || wantVerbose()) {
        ::curl_easy_setopt(impl_->easy, CURLOPT_VERBOSE, 1L);
        ::curl_easy_setopt(impl_->easy, CURLOPT_DEBUGFUNCTION,
                           probeGreeting ? collectCtrlResponse : traceDump);
        ::curl_easy_setopt(impl_->easy, CURLOPT_DEBUGDATA,
                           probeGreeting ? &impl_->ctrlGreeting : nullptr);
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
    // Hard ceiling on the whole connect/auth handshake (DNS + TCP + PWD).
    // CONNECTTIMEOUT_MS does not cover DNS resolution, and a misbehaving
    // server that accepts the socket but stalls the login would otherwise
    // leave the UI stuck on "connecting" indefinitely.
    ::curl_easy_setopt(impl_->easy, CURLOPT_TIMEOUT_MS,
                       static_cast<long>(opt.connectTimeoutMs) * 3L);
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
    if (probeGreeting) {
        // Stop collecting and restore the ordinary verbose hook (if any).
        ::curl_easy_setopt(impl_->easy, CURLOPT_VERBOSE, 0L);
        ::curl_easy_setopt(impl_->easy, CURLOPT_DEBUGFUNCTION, nullptr);
        ::curl_easy_setopt(impl_->easy, CURLOPT_DEBUGDATA, nullptr);
        if (wantVerbose()) {
            ::curl_easy_setopt(impl_->easy, CURLOPT_VERBOSE, 1L);
            ::curl_easy_setopt(impl_->easy, CURLOPT_DEBUGFUNCTION, traceDump);
        }
        // A greeting that is not valid UTF-8 (a Chinese IIS sends GBK) means
        // the server expects GBK path bytes. Lock it so every CWD/STOR/MKD
        // from this session -- including the transfer queue's own independent
        // FtpSession -- uses the correct encoding.
        if (!impl_->ctrlGreeting.empty() &&
            !isValidUtf8(impl_->ctrlGreeting))
            opt_.encoding = "gbk";
        impl_->ctrlGreeting.clear();
    }
    // Clear the whole-transfer ceiling set above so later transfers (large
    // uploads/downloads) are not cut off at 30s.
    ::curl_easy_setopt(impl_->easy, CURLOPT_TIMEOUT_MS, 0L);
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
    // Record the physical directory the server logged us into. On an ordinary
    // server this is "/"; on IIS user isolation it is the user's virtual root
    // (e.g. "/zmailvip163com8"). Every absolute path we later send over the
    // control channel (CWD in quote commands) must be prefixed with it.
    {
        char* entryPath = nullptr;
        if (::curl_easy_getinfo(impl_->easy, CURLINFO_FTP_ENTRY_PATH,
                                &entryPath) == CURLE_OK && entryPath)
            impl_->homeDir = entryPath;
    }
    if (impl_->homeDir.empty() || impl_->homeDir == "/") impl_->homeDir.clear();
    opened_ = true;
    return {};
}

Capabilities FtpSession::capabilities() const {
    Capabilities c;
    c.structuredListing = impl_ && impl_->mlsdOk;  // MLSD probed on list
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

    const std::string d = dir.empty() ? opt_.initialPath : dir;

    // Prefer MLSD (RFC 3659): structured facts (size/type/mtime) instead of
    // scraping LIST text. On a server that rejects MLSD, fall back to LIST.
    if (listViaMlsd(d, page.entries)) {
        impl_->mlsdOk = true;
    } else {
        // Fallback: classic LIST text parse.
        impl_->lastErr.clear();
        impl_->listBuf.clear();
        ::curl_easy_setopt(impl_->easy, CURLOPT_URL,
                           makeUrl(opt_, d, true).c_str());
        ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEFUNCTION, appendToString);
        ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEDATA, &impl_->listBuf);
        // Default request for a trailing-slash URL is LIST; overriding via
        // CUSTOMREQUEST breaks curl's internal FTP state machine.
        ::curl_easy_setopt(impl_->easy, CURLOPT_DIRLISTONLY, 0L);
        CURLcode rc = ::curl_easy_perform(impl_->easy);
        if (rc != CURLE_OK) {
            impl_->lastCurlErr = (int)rc;
            impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
            impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                            : ::curl_easy_strerror(rc);
            impl_->lastErr = "list: " + impl_->lastErr;
            return page;  // caller sees an empty page; lastErr holds the cause
        }
        std::size_t pos = 0;
        while (pos < impl_->listBuf.size()) {
            std::size_t nl = impl_->listBuf.find('\n', pos);
            std::string line = impl_->listBuf.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? impl_->listBuf.size() : nl + 1;
            Entry e;
            if (parseListLine(trim(line), e)) page.entries.push_back(e);
        }
    }
    page.hasMore = false;  // whole listing in one page
    // Convert filename bytes from the server's encoding to UTF-8 (FTP has
    // no mandated encoding; Chinese IIS serves GBK while Linux servers
    // serve UTF-8). Done once here so both MLSD and LIST paths are covered.
    //
    // In "auto" mode we additionally *learn* the server's encoding from this
    // listing: if any name was not valid UTF-8 (i.e. had to be decoded as
    // GBK), lock the session to GBK so that the reverse conversion in
    // makeUrl()/quote commands (CWD into a Chinese directory, MKD/DELE/RMD,
    // rename) sends GBK bytes instead of round-tripping UTF-8. Without this,
    // a Chinese directory would list correctly but refuse to open with
    // "Server denied you to change to the given directory".
    const std::string userEncoding = opt_.encoding;
    for (Entry& e : page.entries) {
        if (userEncoding == "auto" && !isValidUtf8(e.name))
            opt_.encoding = "gbk";  // lock before converting
        e.name = toUtf8(e.name, userEncoding);
    }
    // A listing that produced no entries can hide two very different things:
    // a genuinely empty directory, or a server whose MLSD/LIST payload the
    // parsers could not recognize (a non-Unix/non-DOS format). Surface the
    // latter so the UI shows a diagnostic instead of silently blank.
    if (page.entries.empty() && !impl_->listBuf.empty()) {
        std::string head = toUtf8(trim(impl_->listBuf), opt_.encoding);
        if (head.size() > 160) head = head.substr(0, 160) + "...";
        impl_->lastErr = "无法解析服务器目录列表: [" + head + "]";
    }
    return page;
}

ListingPage FtpSession::listAll(const std::string& dir) {
    ListingPage page;
    if (!opened_ || !impl_ || !impl_->easy) return page;

    const std::string d = dir.empty() ? opt_.initialPath : dir;
    // Force `LIST -a` (show hidden entries). Servers that hide dot-files /
    // system files from the default listing make a directory look empty while
    // RMD still fails with 550 ("not empty"); this surfaces those entries so
    // recursive deletion can remove them. If the server rejects `-a` (or
    // treats it as plain LIST), the result is identical to a normal LIST.
    impl_->lastErr.clear();
    impl_->listBuf.clear();
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL, makeUrl(opt_, d, true).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_CUSTOMREQUEST, "LIST -a");
    ::curl_easy_setopt(impl_->easy, CURLOPT_DIRLISTONLY, 1L);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEFUNCTION, appendToString);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEDATA, &impl_->listBuf);
    CURLcode rc = ::curl_easy_perform(impl_->easy);
    ::curl_easy_setopt(impl_->easy, CURLOPT_CUSTOMREQUEST, nullptr);
    ::curl_easy_setopt(impl_->easy, CURLOPT_DIRLISTONLY, 0L);
    if (rc != CURLE_OK) {
        impl_->lastCurlErr = (int)rc;
        impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
        impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                        : ::curl_easy_strerror(rc);
        impl_->lastErr = "list -a: " + impl_->lastErr;
        return page;
    }
    std::size_t pos = 0;
    while (pos < impl_->listBuf.size()) {
        std::size_t nl = impl_->listBuf.find('\n', pos);
        std::string line = impl_->listBuf.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? impl_->listBuf.size() : nl + 1;
        Entry e;
        if (parseListLine(trim(line), e)) page.entries.push_back(e);
    }
    page.hasMore = false;
    // Same encoding conversion as list() (learn GBK in auto mode).
    const std::string userEncoding = opt_.encoding;
    for (Entry& e : page.entries) {
        if (userEncoding == "auto" && !isValidUtf8(e.name))
            opt_.encoding = "gbk";
        e.name = toUtf8(e.name, userEncoding);
    }
    return page;
}

bool FtpSession::listViaMlsd(const std::string& dir, std::vector<Entry>& out) {
    impl_->lastErr.clear();
    impl_->listBuf.clear();
    // MLSD over the data connection; curl issues the MLSD verb for a
    // trailing-slash URL when CUSTOMREQUEST is set. DIRLISTONLY keeps curl
    // from rewriting it into LIST.
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL,
                       makeUrl(opt_, dir, true).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_CUSTOMREQUEST, "MLSD");
    ::curl_easy_setopt(impl_->easy, CURLOPT_DIRLISTONLY, 1L);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEFUNCTION, appendToString);
    ::curl_easy_setopt(impl_->easy, CURLOPT_WRITEDATA, &impl_->listBuf);
    const CURLcode rc = ::curl_easy_perform(impl_->easy);
    // Restore the default request verb for subsequent LIST/other calls.
    ::curl_easy_setopt(impl_->easy, CURLOPT_CUSTOMREQUEST, nullptr);
    ::curl_easy_setopt(impl_->easy, CURLOPT_DIRLISTONLY, 0L);
    if (rc != CURLE_OK) {
        impl_->lastCurlErr = (int)rc;
        impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
        impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                        : ::curl_easy_strerror(rc);
        return false;
    }
    // A server without MLSD answers 500/502 and curl surfaces it as a
    // QUOTE/response error (non-CURLE_OK) above; if we got here the data
    // connection delivered MLSD lines. Parse them.
    std::size_t pos = 0;
    while (pos < impl_->listBuf.size()) {
        std::size_t nl = impl_->listBuf.find('\n', pos);
        std::string line = impl_->listBuf.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? impl_->listBuf.size() : nl + 1;
        line = trim(line);
        if (line.empty()) continue;
        Entry e;
        if (parseMlsdLine(line, e)) {
            out.push_back(std::move(e));
        }
    }
    // MLSD delivered zero entries. Two cases are indistinguishable here: a
    // genuinely empty directory, or a server that "accepted" MLSD but sent
    // an empty/foreign (LIST-style) payload. Fall back to the classic LIST
    // parse to be safe -- a truly empty directory yields an empty LIST too,
    // so the result stays correct, and a server without real MLSD gets its
    // listing recovered.
    if (out.empty()) {
        impl_->lastErr = "MLSD returned no entries; retrying with LIST";
        return false;
    }
    impl_->lastErr.clear();
    return true;
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

FtpResult FtpSession::stat(const std::string& remotePath, RemoteStat* out) {
    if (out) *out = RemoteStat{};
    std::uint64_t size = 0;
    FtpResult r = this->size(remotePath, &size);
    if (!r.ok()) {
        // CURLE_REMOTE_FILE_NOT_FOUND (78) means the file simply does not
        // exist -- a normal "no conflict" answer, not an error.
        if (r.curlCode == 78 /* CURLE_REMOTE_FILE_NOT_FOUND */) {
            if (out) out->exists = false;
            return {};
        }
        return r;
    }
    if (out) {
        out->exists = true;
        out->size = size;
        out->mtimeUnix = 0;   // FTP: mtime not fetched (see header note)
    }
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
    std::FILE* f = openLocalFile(localPath, mode);
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
    // Bytes actually delivered to the write callback in THIS call. Do not use
    // CONTENT_LENGTH (the announced size): it may be absent or stale and says
    // nothing about how much really arrived -- truncation detection depends
    // on the real counter.
    std::uint64_t movedThisCall = 0;
    {
        curl_off_t sz = 0;
        ::curl_easy_getinfo(impl_->easy, CURLINFO_SIZE_DOWNLOAD_T, &sz);
        if (sz > 0) movedThisCall = static_cast<std::uint64_t>(sz);
    }
    if (bytesMoved) *bytesMoved = movedThisCall;
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        if (resumeBytes > 0)
            ::curl_easy_setopt(impl_->easy, CURLOPT_RESUME_FROM_LARGE,
                               static_cast<curl_off_t>(0));
        return {FtpStatus::Canceled, "canceled by caller", rc};
    }
    if (rc != CURLE_OK) {
        // CURLE_PARTIAL_FILE (link dropped mid-transfer) is a FAILURE: the
        // local file is truncated. Reporting success here previously marked
        // incomplete files Done with no way to tell.
        return {FtpStatus::DownloadFailed,
                std::string("download: ") + ::curl_easy_strerror(rc), rc};
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

    std::FILE* f = openLocalFile(localPath, "rb");
    if (!f) return {FtpStatus::IoError, "cannot open local " + localPath};
    // 64-bit file positions: a plain ftell/fseek wraps at 2 GB and corrupts
    // resume offsets for large files.
    _fseeki64(f, 0, SEEK_END);
    const std::uint64_t fsize = static_cast<std::uint64_t>(_ftelli64(f));
    _fseeki64(f, static_cast<__int64>(resumeBytes), SEEK_SET);

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
    // Bytes actually sent to the server in THIS call (not the planned size:
    // a broken link can cut the stream short without curl noticing until the
    // end).
    std::uint64_t movedThisCall = 0;
    {
        curl_off_t sz = 0;
        ::curl_easy_getinfo(impl_->easy, CURLINFO_SIZE_UPLOAD_T, &sz);
        if (sz > 0) movedThisCall = static_cast<std::uint64_t>(sz);
    }
    if (bytesMoved) *bytesMoved = movedThisCall;
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        return {FtpStatus::Canceled, "canceled by caller", rc};
    }
    if (rc != CURLE_OK) {
        // CURLE_PARTIAL_FILE means the server stopped reading early: the
        // remote file is truncated and must not be reported as complete.
        return {FtpStatus::UploadFailed,
                std::string("upload: ") + ::curl_easy_strerror(rc), rc};
    }
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

// Splits a remote absolute path into (parentDir, baseName). parentDir keeps
// its trailing slash (or is "/"), so it can be reused as a CWD target.
static void splitRemotePath(const std::string& path, std::string& parent,
                            std::string& base) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        parent = "/";
        base = path;
    } else if (slash == 0) {
        parent = "/";
        base = path.substr(1);
    } else {
        parent = path.substr(0, slash + 1);
        base = path.substr(slash + 1);
    }
}

#if defined(FTPFLOW_HAVE_CURL)
FtpResult FtpSession::quoteCommand(const std::string& cwdDir,
                                   const std::string& cmd) {
    if (!impl_ || !impl_->easy) return {FtpStatus::IoError, "not open"};
    impl_->lastErr.clear();
    // CRITICAL: libcurl issues CURLOPT_QUOTE commands BEFORE its own CWD
    // (see the CURLOPT_QUOTE docs: "even before the CWD command"). So a
    // relative command like `DELE name` would run against whatever directory
    // the connection was left in, NOT `cwdDir`. To make the relative form
    // work on strict servers (IIS / Windows hosting panels) that reject
    // absolute paths in DELE/RMD/MKD/RNFR, we CWD into the parent ourselves
    // as the FIRST quote command, then issue the relative op.
    //
    // The CWD argument must be an *absolute* path (resolved from the physical
    // filesystem root, so it is immune to whatever directory the connection
    // was last left in). On IIS user isolation the login lands on a virtual
    // root (e.g. /zmailvip163com8), so the internal path "/wwwroot" must be
    // sent as "/zmailvip163com8/wwwroot"; an un-prefixed "/wwwroot" resolves
    // against the physical root and is rejected with 550.
    const std::string absCwd = toServerAbsPath(cwdDir);
    struct curl_slist* quote = nullptr;
    quote = ::curl_slist_append(
        quote, ("CWD " + fromUtf8(absCwd, opt_.encoding)).c_str());
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
        impl_->lastCurlErr = (int)rc;
        impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
        impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                        : ::curl_easy_strerror(rc);
        impl_->lastErr = cmd.substr(0, cmd.find(' ')) + ": " + impl_->lastErr;
        return {FtpStatus::IoError, impl_->lastErr, rc};
    }
    impl_->lastCurlErr = CURLE_OK;
    return {};
}
#endif

std::string FtpSession::toServerAbsPath(const std::string& path) const {
    // Normalize the internal path (strip leading slashes; treat "." / empty
    // as the root).
    std::string rel = path;
    while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    if (rel.empty() || rel == ".") {
        // Root of the user's view -> the login directory itself.
        return impl_ && !impl_->homeDir.empty() ? impl_->homeDir : "/";
    }
    if (!impl_ || impl_->homeDir.empty()) {
        // Ordinary server: internal path is already physical-absolute.
        return "/" + rel;
    }
    // IIS user isolation: prefix the login root so the server resolves the
    // path against the *physical* root (where it actually lives).
    std::string home = impl_->homeDir;
    while (!home.empty() && home.back() == '/') home.pop_back();
    return home + "/" + rel;
}

FtpResult FtpSession::mkdir(const std::string& remotePath) {
#if defined(FTPFLOW_HAVE_CURL)
    std::string parent, base;
    splitRemotePath(remotePath, parent, base);
    return quoteCommand(parent, "MKD " + fromUtf8(base, opt_.encoding));
#else
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
#endif
}

FtpResult FtpSession::removeFile(const std::string& remotePath) {
#if defined(FTPFLOW_HAVE_CURL)
    std::string parent, base;
    splitRemotePath(remotePath, parent, base);
    // Prefer the relative form (CWD to parent + basename), which is the most
    // widely accepted. If the server rejects it, retry with the absolute form
    // (CWD to root + full path) -- some servers only accept one style.
    FtpResult r = quoteCommand(parent, "DELE " + fromUtf8(base, opt_.encoding));
    if (!r.ok() && r.curlCode == CURLE_QUOTE_ERROR) {
        r = quoteCommand("/", "DELE " + fromUtf8(remotePath, opt_.encoding));
    }
    return r;
#else
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
#endif
}

FtpResult FtpSession::removeDir(const std::string& remotePath) {
#if defined(FTPFLOW_HAVE_CURL)
    std::string parent, base;
    splitRemotePath(remotePath, parent, base);
    // Prefer the relative form (CWD to parent + basename). If the server
    // rejects it, retry with the absolute form (CWD to root + full path) --
    // some servers only accept one style. An empty directory can also be
    // refused when the basename lacks a trailing slash on a few strict
    // servers, so try that spelling too before giving up.
    FtpResult r = quoteCommand(parent, "RMD " + fromUtf8(base, opt_.encoding));
    if (!r.ok() && r.curlCode == CURLE_QUOTE_ERROR) {
        r = quoteCommand("/", "RMD " + fromUtf8(remotePath, opt_.encoding));
    }
    if (!r.ok() && r.curlCode == CURLE_QUOTE_ERROR) {
        r = quoteCommand(parent,
                         "RMD " + fromUtf8(base, opt_.encoding) + "/");
    }
    return r;
#else
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
#endif
}

FtpResult FtpSession::rename(const std::string& from, const std::string& to) {
#if defined(FTPFLOW_HAVE_CURL)
    if (!impl_ || !impl_->easy) return {FtpStatus::IoError, "not open"};
    impl_->lastErr.clear();

    std::string fromParent, fromBase, toParent, toBase;
    splitRemotePath(from, fromParent, fromBase);
    splitRemotePath(to, toParent, toBase);

    // IIS (and other strict servers) reject RNFR/RNTO with an absolute
    // *virtual* path: they resolve the argument against their own CWD (the
    // physical login root) and answer 550. When both sides live in the same
    // directory we CWD there once and send relative basenames. A
    // cross-directory rename goes out as two absolute paths, but they must
    // be PHYSICAL (homeDir-prefixed on IIS user isolation) -- verified
    // against a Microsoft FTP Service box: RNFR/RNTO with
    // "/<home>/a/x.txt" -> "/<home>/b/x.txt" answers 350/250, while the
    // virtual "/a/x.txt" form answers 550.
    if (fromParent == toParent) {
        // RNFR + RNTO must run as one sequence, so both commands share one
        // QUOTE list (curl issues them in order and aborts on the first
        // failure). As with quoteCommand(), libcurl sends QUOTE commands
        // BEFORE its own CWD, so we CWD into the parent explicitly first --
        // otherwise the relative basenames would run in the wrong directory
        // and strict servers answer 550. The CWD target is the physical
        // absolute path (homeDir-prefixed on IIS user isolation).
        const std::string absFromParent = toServerAbsPath(fromParent);
        struct curl_slist* quote = nullptr;
        quote = ::curl_slist_append(
            quote, ("CWD " + fromUtf8(absFromParent, opt_.encoding)).c_str());
        quote = ::curl_slist_append(
            quote, ("RNFR " + fromUtf8(fromBase, opt_.encoding)).c_str());
        quote = ::curl_slist_append(
            quote, ("RNTO " + fromUtf8(toBase, opt_.encoding)).c_str());
        ::curl_easy_setopt(impl_->easy, CURLOPT_URL,
                           makeUrl(opt_, "/", true).c_str());
        ::curl_easy_setopt(impl_->easy, CURLOPT_QUOTE, quote);
        ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 1L);
        CURLcode rc = ::curl_easy_perform(impl_->easy);
        ::curl_slist_free_all(quote);
        ::curl_easy_setopt(impl_->easy, CURLOPT_QUOTE, nullptr);
        ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 0L);
        if (rc != CURLE_OK) {
            impl_->lastCurlErr = (int)rc;
            impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
            impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                            : ::curl_easy_strerror(rc);
            impl_->lastErr = "rename: " + impl_->lastErr;
            return {FtpStatus::IoError, impl_->lastErr, rc};
        }
        impl_->lastCurlErr = CURLE_OK;
        return {};
    }

    // Cross-directory rename: two absolute PHYSICAL paths (see above).
    const std::string absFrom = toServerAbsPath(from);
    const std::string absTo = toServerAbsPath(to);
    struct curl_slist* quote = nullptr;
    quote = ::curl_slist_append(
        quote, ("RNFR " + fromUtf8(absFrom, opt_.encoding)).c_str());
    quote = ::curl_slist_append(
        quote, ("RNTO " + fromUtf8(absTo, opt_.encoding)).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_URL,
                       makeUrl(opt_, "/", true).c_str());
    ::curl_easy_setopt(impl_->easy, CURLOPT_QUOTE, quote);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 1L);
    CURLcode rc = ::curl_easy_perform(impl_->easy);
    ::curl_slist_free_all(quote);
    ::curl_easy_setopt(impl_->easy, CURLOPT_QUOTE, nullptr);
    ::curl_easy_setopt(impl_->easy, CURLOPT_NOBODY, 0L);
    if (rc != CURLE_OK) {
        impl_->lastCurlErr = (int)rc;
        impl_->errBuf[sizeof(impl_->errBuf) - 1] = '\0';
        impl_->lastErr = *impl_->errBuf ? impl_->errBuf
                                        : ::curl_easy_strerror(rc);
        impl_->lastErr = "rename: " + impl_->lastErr;
        return {FtpStatus::IoError, impl_->lastErr, rc};
    }
    impl_->lastCurlErr = CURLE_OK;
    return {};
#else
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
#endif
}

std::string FtpSession::lastError() const {
    return impl_ ? impl_->lastErr : std::string();
}

bool FtpSession::connectionLost() const {
    if (!impl_) return false;
    switch (impl_->lastCurlErr) {
        case CURLE_RECV_ERROR:          // 56 - failure receiving network data
        case CURLE_SEND_ERROR:          // 55 - failed sending network data
        case CURLE_OPERATION_TIMEDOUT:  // 28 - transfer stalled
            return true;
        default:
            return false;
    }
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

ListingPage FtpSession::listAll(const std::string&) { return {}; }

bool FtpSession::listViaMlsd(const std::string&, std::vector<Entry>&) {
    return false;
}

FtpResult FtpSession::size(const std::string&, std::uint64_t*) {
    return {FtpStatus::EngineUnavailable, "built without libcurl"};
}
FtpResult FtpSession::stat(const std::string&, RemoteStat*) {
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
bool FtpSession::connectionLost() const { return false; }

#endif

}  // namespace ftpflow
