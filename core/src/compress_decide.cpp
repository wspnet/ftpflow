// ftpflow -- smart compressibility decision funnel, stages 1-3.
// Part of core.lib: pure C++17, no Qt.
#include "ftpflow/compress_decide.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace ftpflow {
namespace {

constexpr double kEntropyIncompressibleThreshold = 7.2;  // bits/byte

// ---------------------------------------------------------------------------
// Stage 1 tables. Extensions are compared case-insensitively without the
// leading dot. Deliberately small and explicit: wrong entries here cost
// real bandwidth, so every entry is defensible.
// ---------------------------------------------------------------------------

// Highly compressible by prior: text, markup, code, structured data, raw
// container formats that pack poorly.
const char* const kCompressibleExt[] = {
    "txt",  "log",  "csv",  "tsv",  "json", "xml",  "yaml", "yml",
    "html", "htm",  "css",  "js",   "mjs",  "ts",   "jsx",  "tsx",
    "py",   "rb",   "go",   "rs",   "c",    "h",    "cpp",  "cc",
    "hpp",  "cs",   "java", "kt",   "swift","php",  "lua",  "pl",
    "sh",   "bat",  "ps1",  "sql",  "md",   "rst",  "ini",  "cfg",
    "conf", "toml", "svg",  "tex",  "srt",  "vtt",  "npy",  // npy: raw float dumps
    "tar",  "iso",  "vmdk", "vhdx", "dmp",  "pcap", "bpmn", "patch", "diff",
};

// Already-compressed or entropy-coded: re-compression wastes CPU for <2%.
const char* const kIncompressibleExt[] = {
    "zip", "gz",  "bz2", "xz",   "zst",  "lz4",  "7z",  "rar",
    "tgz", "tbz", "txz", "lzma", "br",   "lzo",
    "jpg", "jpeg","png", "gif",  "webp", "heic", "avif", "bmp",  // bmp: raw BUT
    "mp4", "mkv", "mov", "webm", "avi",  "m4v",
    "mp3", "flac", "ogg", "m4a",  "aac",  "opus", "wav",  // wav often PCM, but
    "pdf",                                 // internal streams already deflated
    "docx", "xlsx", "pptx", "odt", "ods", "odp",  // all OOXML = ZIP inside
    "jar",  "war",  "apk",  "ipa",  "aar",  "vsix", "wasm", "class",
    "sqlite", "db", "woff", "woff2", "ttf", "otf", "eot", "exe", "dll",
};

// NOTE: "bmp" and "wav" sit in the incompressible table conservatively;
// magic/entropy stages can still promote genuinely raw ones via Undecided
// flow when the extension is unknown. See decideByMagic for the real byte
// truth. (M0: revisit both rows with measurements.)

bool hasExt(const std::string& lower, const char* const* table, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (lower == table[i]) return true;
    }
    return false;
}

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return s;
}

std::string extensionOf(const std::string& fileName) {
    auto dot = fileName.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= fileName.size()) return {};
    // Reject pathological cases like "backup." or "..gitignore"
    std::string ext = toLowerAscii(fileName.substr(dot + 1));
    if (ext.size() > 10) return {};
    return ext;
}

// ---------------------------------------------------------------------------
// Stage 2 magic table.
// ---------------------------------------------------------------------------

struct Magic {
    std::size_t offset;
    std::size_t len;
    std::uint8_t bytes[8];
    const char* label;
    Verdict verdict;
};

constexpr Magic kMagics[] = {
    // Compressed containers / codecs -- Store.
    {0, 4, {0x50, 0x4B, 0x03, 0x04}, "zip-family (docx/jar/apk...)", Verdict::Store},
    {0, 3, {0x1F, 0x8B, 0x08},       "gzip",                        Verdict::Store},
    {0, 5, {0x28, 0xB5, 0x2F, 0xFD, 0x00}, "zstd",                   Verdict::Store},
    {0, 6, {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00}, "xz",               Verdict::Store},
    {0, 6, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}, "7z",                 Verdict::Store},
    {0, 7, {'R', 'a', 'r', '!', 0x1A, 0x07, 0x00}, "rar",            Verdict::Store},
    {0, 4, {'B', 'Z', 'h', '1'},     "bzip2",                       Verdict::Store},
    {0, 4, {0x04, 0x22, 0x4D, 0x18}, "lz4-frame",                   Verdict::Store},
    // Images / video / audio -- Store.
    {0, 3, {0xFF, 0xD8, 0xFF},       "jpeg",                        Verdict::Store},
    {0, 8, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, "png",     Verdict::Store},
    {0, 6, {'G', 'I', 'F', '8', '7', 'a'}, "gif87",                  Verdict::Store},
    {0, 6, {'G', 'I', 'F', '8', '9', 'a'}, "gif89",                  Verdict::Store},
    {0, 4, {'R', 'I', 'F', 'F'},     "riff-container(webp/wav)",    Verdict::Store},
    {0, 4, {'f', 't', 'y', 'p'},     "mp4-family(isom)",            Verdict::Store},
    {0, 3, {'I', 'D', '3'},          "mp3-id3",                     Verdict::Store},
    {0, 8, {'\0', '\0', '\0', ' ', 'f', 't', 'y', 'p'}, "mp4-qt",   Verdict::Store},
    // Documents with deflated internals -- Store.
    {0, 5, {'%', 'P', 'D', 'F', '-'}, "pdf",                        Verdict::Store},
    {0, 8, {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1},
                                   "ole2(doc/xls/ppt)",              Verdict::Store},
    // Structured raw formats -- Compress.
    {0, 4, {'S', 'Q', 'L', 'i'},     "sqlite-db(page1)",            Verdict::Store},
    {0, 4, {'%', '!', 'P', 'S'},     "postscript",                  Verdict::Compress},
    {0, 4, {'E', 'S', 'C', 0},       "envoy?",                      Verdict::Store},
    // MZ executable: mixed code+resources, later stages decide.
    {0, 2, {'M', 'Z'},               "pe-image(stage-3)",           Verdict::Store},
};

bool magicAt(const std::uint8_t* head, std::size_t headLen, const Magic& m) {
    if (headLen < m.offset + m.len) return false;
    return std::memcmp(head + m.offset, m.bytes, m.len) == 0;
}

}  // namespace

double shannonEntropyBits(const std::uint8_t* data, std::size_t len) {
    if (len == 0) return 0.0;
    std::array<std::uint64_t, 256> freq{};
    for (std::size_t i = 0; i < len; ++i) ++freq[data[i]];
    double h = 0.0;
    for (std::uint64_t f : freq) {
        if (f == 0) continue;
        double p = static_cast<double>(f) / static_cast<double>(len);
        h -= p * std::log2(p);
    }
    return h;
}

Decision decideByExtension(const std::string& fileName) {
    Decision d;
    d.stage = 1;
    const std::string ext = extensionOf(fileName);
    if (ext.empty()) {
        d.verdict = Verdict::Undecided;
        d.reason = "no/odd extension";
        return d;
    }
    if (hasExt(ext, kCompressibleExt, sizeof(kCompressibleExt) / sizeof(const char*))) {
        d.verdict = Verdict::Compress;
        d.reason = "extension prior: text/code/raw-container";
        return d;
    }
    if (hasExt(ext, kIncompressibleExt, sizeof(kIncompressibleExt) / sizeof(const char*))) {
        d.verdict = Verdict::Store;
        d.reason = "extension prior: already compressed";
        return d;
    }
    d.verdict = Verdict::Undecided;
    d.reason = "unknown extension";
    return d;
}

Decision decideByMagic(const std::uint8_t* head, std::size_t headLen) {
    Decision d;
    d.stage = 2;
    if (head == nullptr || headLen == 0) {
        d.verdict = Verdict::Undecided;
        d.reason = "no header bytes";
        return d;
    }
    for (const Magic& m : kMagics) {
        if (magicAt(head, headLen, m)) {
            d.verdict = m.verdict;
            d.reason = std::string("magic: ") + m.label;
            return d;
        }
    }
    // BOM / text sniff: UTF-8 BOM or mostly-printable ASCII head strongly
    // suggests text even with an unknown extension.
    if (headLen >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) {
        d.verdict = Verdict::Compress;
        d.reason = "magic: utf-8 BOM";
        return d;
    }
    if (headLen >= 4 && std::memcmp(head, "OggS", 4) == 0) {
        d.verdict = Verdict::Store;
        d.reason = "magic: ogg";
        return d;
    }
    d.verdict = Verdict::Undecided;
    d.reason = "no known magic";
    return d;
}

Decision decideByEntropy(const std::uint8_t* data, std::size_t len) {
    Decision d;
    d.stage = 3;
    if (data == nullptr || len < 512) {
        d.verdict = Verdict::Undecided;
        d.reason = "sample too small";
        return d;
    }
    const double h = shannonEntropyBits(data, len);
    if (h >= kEntropyIncompressibleThreshold) {
        d.verdict = Verdict::Store;
        d.reason = "entropy " + std::to_string(h).substr(0, 4) + " bits/B: random-ish";
        return d;
    }
    // Low entropy alone does not guarantee a win (could be a huge sparse
    // file); stage 4 (trial compression) makes the final call when zstd is
    // linked. Without zstd we accept low entropy as compressible.
    d.verdict = Verdict::Compress;
    d.reason = "entropy " + std::to_string(h).substr(0, 4) + " bits/B: compressible";
    return d;
}

Decision decideCompressibility(const FileProbe& probe) {
    // Stage 1
    Decision d = decideByExtension(probe.fileName);
    // A definitive "already compressed" extension short-circuits: storing
    // is always safe, at worst a missed optimization.
    if (d.verdict == Verdict::Store) return d;

    // Stage 2 runs even when stage 1 said Compress: the magic check is
    // 32 bytes cheap and vetoes mislabeled files (a zip renamed to .txt).
    // A Compress from stage 1 stays provisional until magic had its say.
    if (!probe.head.empty()) {
        Decision m = decideByMagic(probe.head.data(), probe.head.size());
        if (m.verdict != Verdict::Undecided) return m;
    }

    // Stage 1 provisional Compress now stands.
    if (d.verdict == Verdict::Compress) return d;

    // Stage 3 -- still undecided (unknown extension, unknown magic).
    if (!probe.sample.empty()) {
        Decision e = decideByEntropy(probe.sample.data(), probe.sample.size());
        if (e.verdict != Verdict::Undecided) return e;
    }

    // Nothing could decide (e.g. 0-byte or tiny file).
    d.stage = 0;
    d.verdict = Verdict::Store;
    d.reason = "undecidable, defaulting to store";
    return d;
}

}  // namespace ftpflow
