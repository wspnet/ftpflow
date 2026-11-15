// ftpflow -- directory tree walking implementation.
#include "ftpflow/tree_walk.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace ftpflow {

namespace {

// UTF-8 <-> wide helpers (same policy as vfs_local: the process runs in
// the system code page, we widen at the Win32 boundary).
std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0);
    std::wstring w((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string toUtf8(const wchar_t* w, int n) {
    if (n <= 0) return std::string();
    const int m = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0,
                                      nullptr, nullptr);
    std::string s((std::size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, &s[0], m, nullptr, nullptr);
    return s;
}

void walkLocalDir(const std::string& dirRel, const std::wstring& dirWide,
                  std::vector<TreeEntry>& out, std::string* error) {
    const std::wstring pattern = dirWide + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot enumerate " + dirRel;
        return;
    }
    do {
        const std::string name = toUtf8(fd.cFileName, lstrlenW(fd.cFileName));
        if (name == "." || name == "..") continue;
        const std::string rel =
            dirRel.empty() ? name : dirRel + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Junctions/reparse points: skip to avoid loops.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            TreeEntry e;
            e.relPath = rel;
            e.isDir = true;
            out.push_back(std::move(e));
            walkLocalDir(rel, dirWide + L"\\" + fd.cFileName, out, error);
        } else {
            TreeEntry e;
            e.relPath = rel;
            e.isDir = false;
            e.size = ((std::uint64_t)fd.nFileSizeHigh << 32) |
                     (std::uint64_t)fd.nFileSizeLow;
            out.push_back(std::move(e));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

bool walkLocalTree(const std::string& rootDir, std::vector<TreeEntry>& out,
                   std::string* error) {
    out.clear();
    const std::wstring root = toWide(rootDir);
    const DWORD attrs = GetFileAttributesW(root.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error) *error = "not a directory: " + rootDir;
        return false;
    }
    walkLocalDir("", root, out, error);
    std::sort(out.begin(), out.end(),
              [](const TreeEntry& a, const TreeEntry& b) {
                  return a.relPath < b.relPath;
              });
    return true;
}

namespace {

// Returns false when this directory could not be listed at all (root
// failures abort the walk; nested failures are recorded and skipped).
bool walkSftpDir(SftpSession& sftp, const std::string& dirAbs,
                 const std::string& dirRel, std::vector<TreeEntry>& out,
                 std::string* error) {
    ListingPage page = sftp.list(dirAbs);
    // Failure contract: empty entries + non-empty cursor carrying the
    // message; an empty directory yields empty entries + empty cursor.
    if (page.entries.empty() && !page.cursor.empty()) {
        if (error) *error = "list " + dirAbs + ": " + page.cursor;
        return false;
    }
    for (const Entry& e : page.entries) {
        if (e.name == "." || e.name == "..") continue;
        const std::string rel =
            dirRel.empty() ? e.name : dirRel + "/" + e.name;
        if (e.type == EntryType::Directory) {
            TreeEntry t;
            t.relPath = rel;
            t.isDir = true;
            out.push_back(std::move(t));
            walkSftpDir(sftp, joinTreePath(dirAbs, e.name), rel, out, error);
        } else {
            // Files and symlinks both land as entries; SFTP reads follow
            // links server-side, which is what a transfer wants.
            TreeEntry t;
            t.relPath = rel;
            t.isDir = false;
            t.size = e.size;
            out.push_back(std::move(t));
        }
    }
    return true;
}

bool walkFtpDir(FtpSession& ftp, const std::string& dirAbs,
                const std::string& dirRel, std::vector<TreeEntry>& out,
                std::string* error) {
    ListingPage page = ftp.list(dirAbs);
    if (page.entries.empty() && !ftp.lastError().empty()) {
        if (error) *error = "list " + dirAbs + ": " + ftp.lastError();
        return false;
    }
    for (const Entry& e : page.entries) {
        if (e.name == "." || e.name == "..") continue;
        const std::string rel =
            dirRel.empty() ? e.name : dirRel + "/" + e.name;
        if (e.type == EntryType::Directory) {
            TreeEntry t;
            t.relPath = rel;
            t.isDir = true;
            out.push_back(std::move(t));
            walkFtpDir(ftp, joinTreePath(dirAbs, e.name), rel, out, error);
        } else {
            TreeEntry t;
            t.relPath = rel;
            t.isDir = false;
            t.size = e.size;
            out.push_back(std::move(t));
        }
    }
    return true;
}

}  // namespace

bool walkRemoteTree(SftpSession& sftp, const std::string& rootDir,
                    std::vector<TreeEntry>& out, std::string* error) {
    out.clear();
    if (!sftp.isOpen()) {
        if (error) *error = "sftp session not open";
        return false;
    }
    const bool rootOk = walkSftpDir(sftp, rootDir, "", out, error);
    std::sort(out.begin(), out.end(),
              [](const TreeEntry& a, const TreeEntry& b) {
                  return a.relPath < b.relPath;
              });
    return rootOk;
}

bool walkRemoteTree(FtpSession& ftp, const std::string& rootDir,
                    std::vector<TreeEntry>& out, std::string* error) {
    out.clear();
    if (!ftp.isOpen()) {
        if (error) *error = "ftp session not open";
        return false;
    }
    const bool rootOk = walkFtpDir(ftp, rootDir, "", out, error);
    std::sort(out.begin(), out.end(),
              [](const TreeEntry& a, const TreeEntry& b) {
                  return a.relPath < b.relPath;
              });
    return rootOk;
}

std::string joinTreePath(const std::string& root, const std::string& rel) {
    if (rel.empty()) return root;
    std::string r = root;
    while (!r.empty() && r.back() == '/') r.pop_back();
    if (r.empty()) return "/" + rel;
    return r + "/" + rel;
}

}  // namespace ftpflow
