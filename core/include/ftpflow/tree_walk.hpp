// ftpflow -- directory tree walking for recursive transfers.
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// Produces a flat, depth-first list of entries with paths relative to the
// walk root (forward slashes on every platform, remote included):
//
//   ""            <- never emitted; files land at "name",
//                   nested files at "dir/name", "a/b/c.txt", ...
//
// Local walks use Win32 directly (the vfs_local backend is page-oriented
// for UI; recursive transfer wants the whole tree at once). Remote walks
// drive an open session (SFTP readdir / FTP LIST) and do NOT follow
// directory symlinks -- a link lands in the list as a plain entry so the
// transfer layer can decide (SFTP reads follow the link server-side).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ftpflow/ftp_session.hpp"
#include "ftpflow/sftp_session.hpp"

namespace ftpflow {

struct TreeEntry {
    std::string relPath;      // relative to the walk root, '/'-separated
    bool isDir = false;
    std::uint64_t size = 0;   // files; symlinks may report 0
};

// Walks a local directory tree (Win32). Returns false on a hard failure
// (root missing / unreadable); per-entry errors are reported through
// `error` but the walk continues (best effort, like robocopy).
bool walkLocalTree(const std::string& rootDir, std::vector<TreeEntry>& out,
                   std::string* error = nullptr);

// Walks a remote tree over an open SFTP session.
bool walkRemoteTree(SftpSession& sftp, const std::string& rootDir,
                    std::vector<TreeEntry>& out, std::string* error = nullptr);

// Walks a remote tree over an open FTP session (LIST-based).
bool walkRemoteTree(FtpSession& ftp, const std::string& rootDir,
                    std::vector<TreeEntry>& out, std::string* error = nullptr);

// Joins a walk root and a relative path with '/' (root keeps its own
// style: "C:/x/y" or "/remote/root"). Empty rel -> root unchanged.
std::string joinTreePath(const std::string& root, const std::string& rel);

}  // namespace ftpflow
