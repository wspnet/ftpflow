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
// The file is an APPEND LOG: `add()` appends one length-prefixed record
// (O(1)); `remove()` appends a tombstone record (magic "JFTX" + jobId)
// and periodically compacts the file back to the live set. `load()`
// replays adds and removes in order. A record truncated at the tail (a
// crash mid-append) is dropped silently; corrupt records in the middle
// are an error.
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
    // + rename). Returns false + *error on IO failure. Also used to
    // compact the append log back to the live set.
    bool save(const std::vector<JournalRecord>& records,
              std::string* error) const;

    // Appends one record to the journal (O(1); no rewrite).
    bool add(const JournalRecord& rec, std::string* error) const;

    // Appends many records in a single open-append-flush-close, so bulk
    // enqueues (a drag of thousands of files) cost one disk round-trip
    // instead of one per record.
    bool addBatch(const std::vector<JournalRecord>& recs,
                  std::string* error) const;

    // Removes one record by jobId by appending a tombstone (O(1)); the
    // log is compacted back to the live set after enough tombstones
    // accumulate. Missing id is a no-op success.
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
    // Tombstones appended since the last compaction (remove() rewrites the
    // log back to the live set once this crosses kCompactThreshold).
    mutable std::uint32_t tombstonesSinceCompact_ = 0;
    static constexpr std::uint32_t kCompactThreshold = 512;
};

}  // namespace ftpflow
