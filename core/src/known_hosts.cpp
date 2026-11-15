// ftpflow -- OpenSSH-compatible known_hosts store implementation.
#include "ftpflow/known_hosts.hpp"

#include "ftpflow/sha256.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace ftpflow {

namespace {

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

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

// Parse "[host]:port" or "host" (default port). Returns host (without
// brackets) and fills port; returns false on malformed input.
bool parseHostPort(const std::string& token, std::string& host,
                   std::uint16_t& port) {
    std::string t = trim(token);
    if (t.empty()) return false;
    if (t.front() == '[') {
        const std::size_t close = t.find(']');
        if (close == std::string::npos) return false;
        host = t.substr(1, close - 1);
        if (close + 1 < t.size()) {
            if (t[close + 1] != ':') return false;
            const std::string ps = t.substr(close + 2);
            char* end = nullptr;
            const long p = std::strtol(ps.c_str(), &end, 10);
            if (!end || *end != '\0' || p < 1 || p > 65535) return false;
            port = (std::uint16_t)p;
        }
        return true;
    }
    // Plain "host" or "host:port".
    const std::size_t colon = t.rfind(':');
    if (colon != std::string::npos) {
        // Could be an IPv6 literal without brackets — OpenSSH requires
        // brackets for IPv6 with a port; treat a trailing ":digits" as a
        // port only when the suffix is numeric.
        const std::string tail = t.substr(colon + 1);
        bool numeric = !tail.empty();
        for (char c : tail) numeric = numeric && (c >= '0' && c <= '9');
        if (numeric) {
            host = t.substr(0, colon);
            const long p = std::strtol(tail.c_str(), nullptr, 10);
            if (p < 1 || p > 65535) return false;
            port = (std::uint16_t)p;
            return true;
        }
    }
    host = t;
    return true;
}

std::string formatHostPort(const std::string& host, std::uint16_t port) {
    return "[" + host + "]:" + std::to_string(port);
}

}  // namespace

// ---- base64 ---------------------------------------------------------------

std::string KnownHosts::base64Encode(const void* data, std::size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const std::uint32_t a = p[i];
        const std::uint32_t b = (i + 1 < len) ? p[i + 1] : 0;
        const std::uint32_t c = (i + 2 < len) ? p[i + 2] : 0;
        const std::uint32_t v = (a << 16) | (b << 8) | c;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back((i + 1 < len) ? kB64[(v >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? kB64[v & 63] : '=');
    }
    return out;
}

std::string KnownHosts::base64Decode(const std::string& s, bool* ok) {
    auto setOk = [&](bool v) { if (ok) *ok = v; };
    static int rev[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) rev[i] = -1;
        for (int i = 0; i < 64; ++i) rev[(unsigned char)kB64[i]] = i;
        init = true;
    }
    std::string out;
    std::uint32_t buf = 0;
    int bits = 0;
    for (char ch : s) {
        if (ch == '=' || ch == '\n' || ch == '\r') break;
        const int v = rev[(unsigned char)ch];
        if (v < 0) { setOk(false); return {}; }
        buf = (buf << 6) | (std::uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((buf >> bits) & 0xFF));
        }
    }
    setOk(true);
    return out;
}

std::string KnownHosts::hexOf(const void* data, std::size_t len) {
    static const char* kHex = "0123456789abcdef";
    const unsigned char* p = (const unsigned char*)data;
    std::string s;
    s.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(kHex[p[i] >> 4]);
        s.push_back(kHex[p[i] & 0xF]);
    }
    return s;
}

std::string KnownHosts::hexDecode(const std::string& s, bool* ok) {
    auto setOk = [&](bool v) { if (ok) *ok = v; };
    if (s.size() % 2 != 0) { setOk(false); return {}; }
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) { setOk(false); return {}; }
        out.push_back((char)((hi << 4) | lo));
    }
    setOk(true);
    return out;
}

// ---- fingerprints ---------------------------------------------------------

std::string KnownHosts::fingerprintOf(const std::string& blob) {
    const std::vector<std::uint8_t> d =
        Sha256::digest(blob.data(), blob.size());
    return "SHA256:" + base64Encode(d.data(), d.size());
}

std::string KnownHosts::fingerprintOfHex(const std::string& hex) {
    return fingerprintOf(hexDecode(hex));
}

// ---- store ----------------------------------------------------------------

KnownHosts::KnownHosts(std::string path) : path_(std::move(path)) {}

std::string KnownHosts::defaultPath() {
    return appDataDir() + "\\ftpflow\\known_hosts";
}

bool KnownHosts::load(std::vector<KnownHostEntry>& out,
                      std::string* error) const {
    out.clear();
    if (!enabled()) return true;

    std::ifstream f(path_, std::ios::binary);
    if (!f) return true;   // missing file == empty store

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;

        std::istringstream ss(line);
        std::string hostPort, keyType, b64;
        if (!(ss >> hostPort >> keyType >> b64)) continue;   // skip malformed

        KnownHostEntry e;
        e.keyType = keyType;
        bool ok = false;
        e.blob = base64Decode(b64, &ok);
        if (!ok || e.blob.empty()) continue;   // skip bad base64
        if (!parseHostPort(hostPort, e.host, e.port)) continue;
        out.push_back(std::move(e));
    }
    return true;
}

bool KnownHosts::save(const std::vector<KnownHostEntry>& entries,
                      std::string* error) const {
    if (!enabled()) return true;

    const std::size_t slash = path_.find_last_of("\\/");
    if (slash != std::string::npos) {
        CreateDirectoryW(toWide(path_.substr(0, slash)).c_str(), nullptr);
    }

    std::string body;
    for (const KnownHostEntry& e : entries) {
        body += formatHostPort(e.host, e.port);
        body += ' ';
        body += e.keyType.empty() ? "ssh-rsa" : e.keyType;
        body += ' ';
        body += base64Encode(e.blob.data(), e.blob.size());
        body += '\n';
    }

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (error) *error = "cannot open " + tmp; return false; }
        f.write(body.data(), (std::streamsize)body.size());
        f.flush();
        if (!f) { if (error) *error = "write failed for " + tmp; return false; }
    }

    if (!MoveFileExW(toWide(tmp).c_str(), toWide(path_).c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        if (error) *error = "rename failed for known_hosts";
        DeleteFileW(toWide(tmp).c_str());
        return false;
    }
    return true;
}

bool KnownHosts::contains(const std::string& host, std::uint16_t port,
                          const std::string& blob) const {
    std::vector<KnownHostEntry> entries;
    std::string err;
    if (!load(entries, &err)) return false;
    for (const KnownHostEntry& e : entries) {
        if (e.host == host && e.port == port && e.blob == blob) return true;
    }
    return false;
}

bool KnownHosts::trust(const std::string& host, std::uint16_t port,
                       const std::string& keyType, const std::string& blob,
                       std::string* error) const {
    if (!enabled()) return true;
    std::vector<KnownHostEntry> entries;
    if (!load(entries, error)) return false;

    // Replace any existing entry for this host:port, else append.
    bool replaced = false;
    for (KnownHostEntry& e : entries) {
        if (e.host == host && e.port == port) {
            e.keyType = keyType;
            e.blob = blob;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        KnownHostEntry e;
        e.host = host;
        e.port = port;
        e.keyType = keyType;
        e.blob = blob;
        entries.push_back(std::move(e));
    }
    return save(entries, error);
}

bool KnownHosts::forget(const std::string& host, std::uint16_t port,
                        std::string* error) const {
    if (!enabled()) return true;
    std::vector<KnownHostEntry> entries;
    if (!load(entries, error)) return false;
    std::vector<KnownHostEntry> kept;
    kept.reserve(entries.size());
    for (KnownHostEntry& e : entries) {
        if (!(e.host == host && e.port == port)) kept.push_back(std::move(e));
    }
    return save(kept, error);
}

std::vector<std::string> KnownHosts::trustedHexFor(const std::string& host,
                                                   std::uint16_t port) const {
    std::vector<std::string> out;
    std::vector<KnownHostEntry> entries;
    std::string err;
    if (!load(entries, &err)) return out;
    for (const KnownHostEntry& e : entries) {
        if (e.host == host && e.port == port)
            out.push_back(hexOf(e.blob.data(), e.blob.size()));
    }
    return out;
}

}  // namespace ftpflow
