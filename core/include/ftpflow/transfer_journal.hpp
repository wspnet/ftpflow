// ftpflow -- durable transfer journal.
// Part of core.lib: pure C++17 + Win32, no Qt, no third-party deps.
//
// Persists in-flight transfer requests so a crash / app restart can resume
// them. Only QUEUED and RUNNING jobs are stored; a job that reaches a
// terminal state (Done / Failed / Canceled) is removed from the journal.
//
// Format: a small versioned binary file. Every record is a list of
// length-prefixed fields (u32 LE length + bytes) separated by a unit
// separator (0x1F), one record per line terminated by '\n'. Strings may
// contain any byte except the separators are escaped; in practice we keep
// it simple and write raw (paths are the only arbitrary text and 0x1F/0x0A
// do not appear in Windows path names).
//
// Credentials are NOT persisted: passwords and private keys are dropped
// on save and left empty on load. The resume path re-supplies them (the
// GUI pulls the password from the Windows Credential Manager via the site
// name; the CLI takes --key/--pw again).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ftpflow/transfer_queue.hpp"

namespace ftpflow {

// Journal records mirror TransferRequest minus secrets, plus the site
// reference that lets the resume path recover credentials.
struct JournalRecord {
    std::uint64_t jobId = 0;
    TransferDirection direction = TransferDirection::Upload;
    BackendKind protocol = BackendKind::Sftp;
    std::string localPath;
    std::string remotePath;
    // FTP endpoint (password always empty here).
    FtpOptions ftp;
    // SSH endpoint (password/privateKeyPem/privateKeyPassphrase empty).
    SshEndpoint ssh;
    bool autoDecide = true;
    TransferMode mode = TransferMode::Compress;
    int zstdLevel = 3;
    std::string pushCommandTemplate;
    bool resume = false;
    bool ftzArchive = false;
    bool ensureRemoteDir = false;
    // Reference for credential recovery (GUI site name); empty when the
    // job was enqueued with inline credentials (CLI).
    std::string siteName;
};

class TransferJournal {
public:
    // `path` is the on-disk journal file. Empty path -> disabled (no-op).
    explicit TransferJournal(std::string path);

    // Reads all records. Returns false + *error when the file is corrupt
    // (the caller may choose to ignore and start a fresh journal).
    bool load(std::vector<JournalRecord>& out, std::string* error) const;

    // Replaces the journal contents with `records` atomically (temp file
    // + rename). Returns false + *error on IO failure.
    bool save(const std::vector<JournalRecord>& records,
              std::string* error) const;

    // Removes one record by jobId (rewrites the file). Missing id is a
    // no-op success.
    bool remove(std::uint64_t jobId, std::string* error) const;

    const std::string& path() const { return path_; }
    bool enabled() const { return !path_.empty(); }

    // Default journal location: %APPDATA%/ftpflow/transfers.dat.
    static std::string defaultPath();

    // Conversion helpers (shared with the queue integration).
    static JournalRecord fromRequest(std::uint64_t jobId,
                                     const TransferRequest& req);
    static TransferRequest toRequest(const JournalRecord& rec);

private:
    std::string path_;
};

}  // namespace ftpflow
