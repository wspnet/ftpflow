// ftpflow -- filename byte encoding helpers.
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// Remote filesystems do not agree on a filename encoding. SFTP is UTF-8 by
// the spec, but FTP has no such rule: IIS (Windows) serves names in the
// server's ANSI code page (GBK on a Chinese box), while vsftpd/proftpd serve
// UTF-8. The UI assumes UTF-8 everywhere, so backends convert name bytes to
// UTF-8 at the protocol boundary via toUtf8().
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace ftpflow {

// Normalize an encoding label to a canonical token the codec understands.
// Returns "utf-8", "gbk", or "auto" (default when the label is empty or
// unrecognized). Case- and separator-insensitive ("GB2312" -> "gbk",
// "cp936"/"936" -> "gbk", "utf8" -> "utf-8").
std::string normalizeEncoding(const std::string& label);

// Convert raw filename bytes to UTF-8.
//   encoding == "utf-8"  -> bytes are already UTF-8 (validated; invalid
//                           sequences are replaced with U+FFFD).
//   encoding == "gbk"    -> bytes are GBK/GB2312/GB18030 (Windows code page
//                           936); decoded and re-encoded as UTF-8.
//   encoding == "auto"   -> if the bytes are valid UTF-8 keep them as-is,
//                           otherwise treat them as GBK (covers Chinese IIS
//                           vs. UTF-8 servers without configuration).
// Unconvertible bytes are replaced with U+FFFD rather than dropping the
// whole name, so a mixed listing never disappears.
std::string toUtf8(const std::string& bytes, const std::string& encoding);

// True when `bytes` is a valid, complete UTF-8 sequence (no lone lead/trail
// bytes, no overlong forms, no surrogates). Used by the "auto" mode probe.
bool isValidUtf8(const std::string& bytes);

// Convert a UTF-8 path (what the UI holds, and what toUtf8 produced) back
// to the bytes the server expects before it is sent on the wire (CWD/URL/
// quote commands). This is the inverse of toUtf8:
//   encoding == "utf-8"  -> unchanged.
//   encoding == "gbk"    -> UTF-8 -> Windows code page 936 bytes.
//   encoding == "auto"   -> unchanged (the server was assumed UTF-8 when
//                           names decoded as valid UTF-8); note that a real
//                           GBK server whose names are pure ASCII is
//                           indistinguishable and needs "gbk" set explicitly.
std::string fromUtf8(const std::string& utf8, const std::string& encoding);

// Open a LOCAL file whose path is UTF-8 (what the UI and the rest of core
// hold). Plain std::fopen uses the ANSI code page (GBK on a Chinese box), so
// a UTF-8 Chinese path would fail to open. This converts to a wide string and
// uses _wfopen so any Unicode local path opens correctly.
std::FILE* openLocalFile(const std::string& utf8Path, const char* mode);

// Read a whole LOCAL file (UTF-8 path) into a string; empty on failure.
// One shared implementation for key files, probes and similar small reads.
std::string readWholeFileUtf8(const std::string& utf8Path);

// Size in bytes of a LOCAL file whose path is UTF-8, or 0 when it cannot be
// stat'd (missing file, or a non-ASCII path). Mirrors openLocalFile's wide
// conversion so Chinese paths work.
std::uint64_t localFileSizeUtf8(const std::string& utf8Path);

// Create a LOCAL directory whose path is UTF-8 (recursive single segment;
// callers create the parent chain first). Returns true on success or when
// the directory already exists. Uses the wide Win32 API so Chinese paths
// work.
bool createDirUtf8(const std::string& utf8Path);

}  // namespace ftpflow
