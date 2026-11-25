// ftpflow -- filename byte encoding helpers (Win32).
#include "ftpflow/text_codec.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <sys/stat.h>

namespace ftpflow {

namespace {

// Case-insensitive, separator-insensitive comparison helper.
std::string lowerKey(const std::string& s) {
    std::string k;
    k.reserve(s.size());
    for (char c : s) {
        if (c == '-' || c == '_' || c == ' ') continue;
        k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return k;
}

// Decode a byte string using a Windows code page, then re-encode as UTF-8.
// On any failure (bad sequence) the offending bytes map to U+FFFD via
// MB_ERR_INVALID_CHARS disabled + WC_NO_BEST_FIT_CHARS, so names survive.
std::string transcode(const std::string& in, UINT codepage) {
    if (in.empty()) return {};
    const int wn = MultiByteToWideChar(codepage, 0, in.data(), (int)in.size(),
                                       nullptr, 0);
    if (wn <= 0) return in;  // cannot decode at all; return raw bytes
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(codepage, 0, in.data(), (int)in.size(), &w[0], wn);
    const int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                       nullptr, 0, nullptr, nullptr);
    if (un <= 0) return in;
    std::string out((size_t)un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], un,
                        nullptr, nullptr);
    return out;
}

}  // namespace

std::string normalizeEncoding(const std::string& label) {
    const std::string k = lowerKey(label);
    if (k == "utf8" || k == "utf") return "utf-8";
    if (k == "gbk" || k == "gb2312" || k == "gb18030" || k == "cp936" ||
        k == "936" || k == "ansi" || k == "chinese") {
        return "gbk";
    }
    return "auto";
}

bool isValidUtf8(const std::string& bytes) {
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {                      // ASCII
            ++i;
        } else if ((c & 0xE0) == 0xC0) {     // 2-byte lead
            if (i + 1 >= n) return false;
            const unsigned char c1 = p[i + 1];
            if ((c1 & 0xC0) != 0x80) return false;
            const unsigned cp = ((c & 0x1F) << 6) | (c1 & 0x3F);
            if (cp < 0x80) return false;     // overlong
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {     // 3-byte lead
            if (i + 2 >= n) return false;
            const unsigned char c1 = p[i + 1], c2 = p[i + 2];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
            const unsigned cp =
                ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            if (cp < 0x800) return false;    // overlong
            if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // surrogate
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {     // 4-byte lead
            if (i + 3 >= n) return false;
            const unsigned char c1 = p[i + 1], c2 = p[i + 2], c3 = p[i + 3];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 ||
                (c3 & 0xC0) != 0x80)
                return false;
            const unsigned cp = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) |
                                ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return false;  // overlong/out
            i += 4;
        } else {
            return false;  // stray continuation or invalid lead
        }
    }
    return true;
}

std::string toUtf8(const std::string& bytes, const std::string& encoding) {
    if (bytes.empty()) return bytes;
    const std::string enc = normalizeEncoding(encoding);
    if (enc == "utf-8") {
        // Validate; replace invalid runs so the name still shows.
        if (isValidUtf8(bytes)) return bytes;
        return transcode(bytes, CP_UTF8);  // MB->W (lenient) ->UTF-8
    }
    if (enc == "gbk") {
        return transcode(bytes, 936);
    }
    // "auto": keep UTF-8 as-is, otherwise assume GBK.
    if (isValidUtf8(bytes)) return bytes;
    return transcode(bytes, 936);
}

std::string fromUtf8(const std::string& utf8, const std::string& encoding) {
    if (utf8.empty()) return utf8;
    const std::string enc = normalizeEncoding(encoding);
    if (enc == "gbk") {
        // UTF-8 -> wide -> Windows code page 936 (GBK) bytes. Characters
        // outside GBK map to '?' via the default replacement, matching what
        // a GBK-only server can represent.
        const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           (int)utf8.size(), nullptr, 0);
        if (wn <= 0) return utf8;
        std::wstring w((size_t)wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0],
                            wn);
        const int bn = WideCharToMultiByte(936, 0, w.data(), (int)w.size(),
                                           nullptr, 0, nullptr, nullptr);
        if (bn <= 0) return utf8;
        std::string out((size_t)bn, '\0');
        WideCharToMultiByte(936, 0, w.data(), (int)w.size(), &out[0], bn,
                            nullptr, nullptr);
        return out;
    }
    // "utf-8" and "auto": paths stay UTF-8 on the wire.
    return utf8;
}

std::FILE* openLocalFile(const std::string& utf8Path, const char* mode) {
    if (utf8Path.empty()) return nullptr;
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(),
                                       (int)utf8Path.size(), nullptr, 0);
    if (wn <= 0) return nullptr;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(), (int)utf8Path.size(),
                        &w[0], wn);
    const int mn = MultiByteToWideChar(CP_UTF8, 0, mode, -1, nullptr, 0);
    std::wstring wm((size_t)mn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, &wm[0], mn);
    return _wfopen(w.c_str(), wm.c_str());
}

std::string readWholeFileUtf8(const std::string& utf8Path) {
    std::string out;
    std::FILE* f = openLocalFile(utf8Path, "rb");
    if (!f) return out;
    _fseeki64(f, 0, SEEK_END);
    const __int64 n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<std::size_t>(n));
        const std::size_t got =
            std::fread(&out[0], 1, static_cast<std::size_t>(n), f);
        out.resize(got);
    }
    std::fclose(f);
    return out;
}

std::uint64_t localFileSizeUtf8(const std::string& utf8Path) {
    if (utf8Path.empty()) return 0;
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(),
                                       (int)utf8Path.size(), nullptr, 0);
    if (wn <= 0) return 0;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(), (int)utf8Path.size(),
                        &w[0], wn);
    struct _stat64 st{};
    return _wstat64(w.c_str(), &st) == 0 ? (std::uint64_t)st.st_size : 0;
}

bool createDirUtf8(const std::string& utf8Path) {
    if (utf8Path.empty()) return false;
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(),
                                       (int)utf8Path.size(), nullptr, 0);
    if (wn <= 0) return false;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(), (int)utf8Path.size(),
                        &w[0], wn);
    if (CreateDirectoryW(w.c_str(), nullptr)) return true;
    // Already exists (as a directory) is success.
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace ftpflow
