// ftpflow -- durable transfer journal implementation.
#include "ftpflow/transfer_journal.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace ftpflow {

namespace {

constexpr std::uint32_t kMagic = 0x4654464A;   // "JFTF" little-endian
constexpr std::uint32_t kVersion = 1;

// ---- binary field helpers -------------------------------------------------

void writeU32(std::string& out, std::uint32_t v) {
    out.push_back((char)(v & 0xFF));
    out.push_back((char)((v >> 8) & 0xFF));
    out.push_back((char)((v >> 16) & 0xFF));
    out.push_back((char)((v >> 24) & 0xFF));
}

void writeStr(std::string& out, const std::string& s) {
    writeU32(out, (std::uint32_t)s.size());
    out.append(s);
}

struct Reader {
    const std::string& buf;
    std::size_t pos = 0;
    bool ok = true;

    std::uint32_t u32() {
        if (pos + 4 > buf.size()) { ok = false; return 0; }
        const unsigned char* p = (const unsigned char*)buf.data() + pos;
        std::uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16) |
                          ((std::uint32_t)p[3] << 24);
        pos += 4;
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok || pos + n > buf.size()) { ok = false; return {}; }
        std::string s = buf.substr(pos, n);
        pos += n;
        return s;
    }
};

// ---- Win32 helpers --------------------------------------------------------

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0);
    std::wstring w((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string s((std::size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                        nullptr, nullptr);
    return s;
}

std::string appDataDir() {
    wchar_t buf[MAX_PATH];
    const HRESULT hr = SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                        SHGFP_TYPE_CURRENT, buf);
    if (FAILED(hr)) return {};
    return toUtf8(buf);
}

}  // namespace

// ---- serialization --------------------------------------------------------

JournalRecord TransferJournal::fromRequest(std::uint64_t jobId,
                                          const TransferRequest& req) {
    JournalRecord rec;
    rec.jobId = jobId;
    rec.direction = req.direction;
    rec.protocol = req.protocol;
    rec.localPath = req.localPath;
    rec.remotePath = req.remotePath;
    rec.ftp = req.ftp;
    rec.ftp.password.clear();                 // never persist credentials
    rec.ssh = req.ssh;
    rec.ssh.password.clear();
    rec.ssh.privateKeyPem.clear();
    rec.ssh.privateKeyPassphrase.clear();
    rec.autoDecide = req.autoDecide;
    rec.mode = req.mode;
    rec.zstdLevel = req.zstdLevel;
    rec.pushCommandTemplate = req.pushCommandTemplate;
    rec.resume = req.resume;
    rec.ftzArchive = req.ftzArchive;
    rec.ensureRemoteDir = req.ensureRemoteDir;
    rec.siteName = req.siteName;
    return rec;
}

TransferRequest TransferJournal::toRequest(const JournalRecord& rec) {
    TransferRequest req;
    req.direction = rec.direction;
    req.protocol = rec.protocol;
    req.localPath = rec.localPath;
    req.remotePath = rec.remotePath;
    req.ftp = rec.ftp;
    req.ssh = rec.ssh;
    req.autoDecide = rec.autoDecide;
    req.mode = rec.mode;
    req.zstdLevel = rec.zstdLevel;
    req.pushCommandTemplate = rec.pushCommandTemplate;
    req.resume = rec.resume;
    req.ftzArchive = rec.ftzArchive;
    req.ensureRemoteDir = rec.ensureRemoteDir;
    req.siteName = rec.siteName;
    return req;
}

static std::string encodeRecord(const JournalRecord& rec) {
    std::string out;
    writeU32(out, kMagic);
    writeU32(out, kVersion);
    writeU32(out, (std::uint32_t)(rec.jobId & 0xFFFFFFFF));
    writeU32(out, (std::uint32_t)(rec.jobId >> 32));
    out.push_back((char)rec.direction);
    out.push_back((char)rec.protocol);
    writeStr(out, rec.localPath);
    writeStr(out, rec.remotePath);
    // FTP endpoint.
    writeStr(out, rec.ftp.host);
    writeU32(out, rec.ftp.port);
    writeStr(out, rec.ftp.user);
    writeStr(out, rec.ftp.initialPath);
    out.push_back(rec.ftp.tls ? 1 : 0);
    out.push_back(rec.ftp.implicitTls ? 1 : 0);
    writeU32(out, (std::uint32_t)rec.ftp.connectTimeoutMs);
    // SSH endpoint (no secrets).
    writeStr(out, rec.ssh.host);
    writeU32(out, rec.ssh.port);
    writeStr(out, rec.ssh.user);
    // Options.
    out.push_back(rec.autoDecide ? 1 : 0);
    out.push_back((char)rec.mode);
    writeU32(out, (std::uint32_t)rec.zstdLevel);
    writeStr(out, rec.pushCommandTemplate);
    out.push_back(rec.resume ? 1 : 0);
    out.push_back(rec.ftzArchive ? 1 : 0);
    out.push_back(rec.ensureRemoteDir ? 1 : 0);
    writeStr(out, rec.siteName);
    return out;
}

static bool decodeRecord(const std::string& line, JournalRecord& out) {
    Reader r{line};
    if (r.u32() != kMagic) return false;
    if (r.u32() != kVersion) return false;
    const std::uint64_t lo = r.u32();
    const std::uint64_t hi = r.u32();
    out.jobId = lo | (hi << 32);
    out.direction = (TransferDirection)(unsigned char)line[r.pos];
    r.pos += 1;
    out.protocol = (BackendKind)(unsigned char)line[r.pos];
    r.pos += 1;
    out.localPath = r.str();
    out.remotePath = r.str();
    out.ftp.host = r.str();
    out.ftp.port = (std::uint16_t)r.u32();
    out.ftp.user = r.str();
    out.ftp.initialPath = r.str();
    out.ftp.tls = line[r.pos] != 0;
    r.pos += 1;
    out.ftp.implicitTls = line[r.pos] != 0;
    r.pos += 1;
    out.ftp.connectTimeoutMs = (int)r.u32();
    out.ssh.host = r.str();
    out.ssh.port = (std::uint16_t)r.u32();
    out.ssh.user = r.str();
    out.autoDecide = line[r.pos] != 0;
    r.pos += 1;
    out.mode = (TransferMode)(unsigned char)line[r.pos];
    r.pos += 1;
    out.zstdLevel = (int)r.u32();
    out.pushCommandTemplate = r.str();
    out.resume = line[r.pos] != 0;
    r.pos += 1;
    out.ftzArchive = line[r.pos] != 0;
    r.pos += 1;
    out.ensureRemoteDir = line[r.pos] != 0;
    r.pos += 1;
    out.siteName = r.str();
    return r.ok;
}

// ---- TransferJournal ------------------------------------------------------

TransferJournal::TransferJournal(std::string path) : path_(std::move(path)) {}

std::string TransferJournal::defaultPath() {
    return appDataDir() + "\\ftpflow\\transfers.dat";
}

bool TransferJournal::load(std::vector<JournalRecord>& out,
                           std::string* error) const {
    out.clear();
    if (!enabled()) return true;

    std::ifstream f(path_, std::ios::binary);
    if (!f) {
        // Missing file is a clean "empty journal".
        return true;
    }

    // Length-prefixed records: u32 LE record length + record body. Reading
    // by length (not by '\n') keeps binary field bytes from being mistaken
    // for a delimiter.
    for (;;) {
        unsigned char lenBuf[4];
        f.read((char*)lenBuf, 4);
        if (f.gcount() == 0) break;             // clean EOF
        if (f.gcount() != 4) {                   // truncated header
            if (error) *error = "truncated journal header in " + path_;
            return false;
        }
        const std::uint32_t len = lenBuf[0] | (lenBuf[1] << 8) |
                                  (lenBuf[2] << 16) |
                                  ((std::uint32_t)lenBuf[3] << 24);
        std::string body(len, '\0');
        f.read(&body[0], len);
        if (f.gcount() != (std::streamsize)len) {
            if (error) *error = "truncated journal record in " + path_;
            return false;
        }
        JournalRecord rec;
        if (!decodeRecord(body, rec)) {
            if (error) *error = "corrupt journal record in " + path_;
            return false;
        }
        out.push_back(std::move(rec));
    }
    return true;
}

bool TransferJournal::save(const std::vector<JournalRecord>& records,
                           std::string* error) const {
    if (!enabled()) return true;

    // Ensure the parent directory exists.
    const std::size_t slash = path_.find_last_of("\\/");
    if (slash != std::string::npos) {
        const std::string dir = path_.substr(0, slash);
        CreateDirectoryW(toWide(dir).c_str(), nullptr);
    }

    std::string body;
    for (const JournalRecord& r : records) {
        const std::string enc = encodeRecord(r);
        // Length prefix (u32 LE) + record body.
        body.push_back((char)(enc.size() & 0xFF));
        body.push_back((char)((enc.size() >> 8) & 0xFF));
        body.push_back((char)((enc.size() >> 16) & 0xFF));
        body.push_back((char)((enc.size() >> 24) & 0xFF));
        body += enc;
    }

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot open " + tmp;
            return false;
        }
        f.write(body.data(), (std::streamsize)body.size());
        f.flush();
        if (!f) {
            if (error) *error = "write failed for " + tmp;
            return false;
        }
    }

    // Atomic swap.
    if (!MoveFileExW(toWide(tmp).c_str(), toWide(path_).c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        if (error) *error = "rename failed for journal";
        DeleteFileW(toWide(tmp).c_str());
        return false;
    }
    return true;
}

bool TransferJournal::remove(std::uint64_t jobId, std::string* error) const {
    if (!enabled()) return true;
    std::vector<JournalRecord> recs;
    if (!load(recs, error)) return false;
    std::vector<JournalRecord> kept;
    kept.reserve(recs.size());
    for (auto& r : recs) {
        if (r.jobId != jobId) kept.push_back(std::move(r));
    }
    return save(kept, error);
}

}  // namespace ftpflow
