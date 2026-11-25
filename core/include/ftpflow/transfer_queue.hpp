// ftpflow -- concurrent transfer queue.
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// A small worker pool executing queued transfer jobs. Per job:
//   FTP  -> FtpSession (one session per job; libcurl pools internally)
//   SSH  -> Upload: auto-decide Compress (exec+zstd fast path) vs Store
//           (SFTP) through the 5-stage funnel; Download: always Store.
// Workers cache one SftpSession per SSH endpoint (handshake reuse).
//
// Events (Started / Progress / Finished) fire on WORKER threads; the UI
// layer must marshal them (Qt queued connections). Progress is throttled
// to `progressMinIntervalMs` so a fast link does not flood the caller.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ftpflow/compress_decide.hpp"
#include "ftpflow/ftp_session.hpp"
#include "ftpflow/ftz1.hpp"
#include "ftpflow/sftp_session.hpp"
#include "ftpflow/ssh_exec.hpp"
#include "ftpflow/ssh_transfer.hpp"
#include "ftpflow/transfer.hpp"
#include "ftpflow/vfs.hpp"

namespace ftpflow {

class TransferJournal;   // defined in transfer_journal.hpp (includes us)

enum class TransferState : std::uint8_t {
    Queued, Running, Done, Failed, Canceled, Skipped,
};

// How a transfer behaves when the destination file already exists.
enum class ConflictPolicy : std::uint8_t {
    Overwrite,   // always replace the existing file
    Skip,        // never touch it; mark the job Skipped
    Smart,       // compare size + mtime: identical -> skip, else overwrite
};

// Smart-policy decision: keep the existing destination (skip) vs overwrite.
// Returns true when the destination should be kept. Sizes differ -> overwrite;
// mtimes differ (when both are known) -> overwrite; otherwise keep. A
// directory destination is never "the same file", so it is never skipped.
inline bool smartShouldSkip(const RemoteStat& src, const RemoteStat& dst) {
    if (!dst.exists || dst.isDir) return false;
    if (src.size != dst.size) return false;              // different size
    if (src.mtimeUnix != 0 && dst.mtimeUnix != 0 &&
        src.mtimeUnix != dst.mtimeUnix) return false;     // different time
    return true;   // same size (+ same/unknown time) -> keep existing
}

struct TransferRequest {
    TransferDirection direction = TransferDirection::Upload;
    BackendKind protocol = BackendKind::Sftp;   // Ftp or Sftp(SSH)

    std::string localPath;
    std::string remotePath;

    // protocol == Ftp
    FtpOptions ftp;
    // protocol == Sftp (store) / SSH compress push
    SshEndpoint ssh;

    // SSH uploads only: run the 5-stage funnel on the local file to pick
    // Compress vs Store. The decision is reported in the Started event.
    // When autoDecide is false, `mode` is used as-is.
    bool autoDecide = true;
    TransferMode mode = TransferMode::Compress;
    CostModel costModel;
    int zstdLevel = 3;
    // Remote decompressor template for compress mode; {file} -> "remote".
    // Empty -> sshDefaultPushCommandTemplate() (system zstd).
    std::string pushCommandTemplate;

    // Resume: pick up from the already-transferred prefix (FTP REST/APPE,
    // SFTP seek, SSH append-push). Uploads stat the remote file, downloads
    // stat the local file. With an appending remote agent this yields
    // byte-exact continuation; the queue verifies the final size.
    bool resume = false;

    // Same-name destination handling. Overwrite (default) always replaces;
    // Skip never touches an existing destination; Smart compares size and
    // mtime and only overwrites when they differ (identical -> skip).
    // Applies to plain file transfers (Store / Compress / Ftz1), not to
    // directory-archive jobs.
    ConflictPolicy conflict = ConflictPolicy::Overwrite;

    // Ship as an FTZ1 container (SFTP endpoints only, both directions).
    // Upload: local -> container -> remotePath + ".ftz1".
    // Download: remotePath is the container, localPath the extraction
    // target. Frames are hash-verified on extract; corrupt frames are
    // named in the error.
    bool ftzArchive = false;

    // Recursive-upload helper: make sure the remote parent directory of
    // remotePath exists before transferring (mkdir -p, failures to create
    // an already-existing dir are ignored). Downloads always create the
    // local parent directory chain.
    bool ensureRemoteDir = false;

    // Directory archive mode: instead of expanding the folder into one job
    // per file, pack the whole tree into a single zstd-compressed tar and
    // transfer that, extracting on the far side. Uploads pack locally and
    // (for SFTP) extract on the remote via an exec channel; downloads pack
    // on the remote (SFTP) and extract locally. FTP has no exec channel, so
    // an FTP upload leaves the .tar.zst on the server (the caller surfaces
    // a "extract manually" note) and an FTP download falls back to the
    // plain per-file tree transfer.
    bool dirArchive = false;

    // Saved-site reference for credential recovery on restore (GUI). The
    // journal stores it; restore() users match it against their site store
    // to re-supply the password/key. Empty = inline credentials (CLI).
    std::string siteName;

    // Optional cooperative abort for recursive enqueueTree(). The GUI sets
    // this on a background folder scan so an emergency stop cannot be
    // refilled by a scan that finishes moments later.
    std::shared_ptr<std::atomic<bool>> enqueueAbort;

    // Trusted host public-key blobs (lowercase hex) for strict SSH
    // host-key verification. When non-empty, the SFTP/SSH backend verifies
    // strictly; empty = trust-on-first-use (AcceptNew). Not journaled —
    // restore() callers re-supply it from their known_hosts store.
    std::vector<std::string> trustedHostKeyHex;
};

struct TransferEvent {
    enum class Kind : std::uint8_t { Queued, Started, Progress, Finished };
    Kind kind = Kind::Started;
    std::uint64_t jobId = 0;
    TransferState state = TransferState::Queued;
    TransferMode mode = TransferMode::Store;   // effective pipeline
    TransferProgress progress;
    Decision decision;                          // funnel result (Started)
    std::string error;                          // Failed detail
    // Source/target paths, populated on Queued/Started so the UI can label
    // the row without keeping the original request around.
    std::string localPath;
    std::string remotePath;
    TransferDirection direction = TransferDirection::Upload;
};

class TransferQueue {
public:
    using EventCallback = std::function<void(const TransferEvent&)>;

    // `workers` transfer slots; 2 is the sweet spot for one network path.
    explicit TransferQueue(unsigned workers = 2, int progressMinIntervalMs = 100);
    ~TransferQueue();   // drains nothing; shuts down immediately (joins)

    // Must be set before the first enqueue (events fire on worker threads;
    // the callback must be thread-safe or marshal to another thread).
    void setEventCallback(EventCallback cb);

    // Attach a durable journal (by file path; empty disables it). Queued /
    // running jobs are written on enqueue and removed on a terminal state;
    // restore() replays them. Credentials are NOT persisted (the journal
    // drops passwords/keys) -- supply them again on restore via
    // setCredentialSource or by editing the restored requests.
    void setJournal(std::string journalPath);

    // Recovers in-flight jobs from the journal and re-enqueues them with
    // resume=true (byte-exact continuation). Returns the number restored.
    // Credentials for the restored jobs come from `cred` when set (applied
    // to every restored request). Must be called before any worker has
    // work, ideally right after construction.
    std::size_t restore(std::string* error = nullptr,
                        std::function<void(TransferRequest&)> cred = nullptr);

    std::uint64_t enqueue(TransferRequest req);   // -> jobId (1-based)

    // Bulk enqueue: N requests under one lock, one journal write, and one
    // worker wake-up. Used for large drags / tree walks so the UI thread
    // doesn't stall on per-file locking + per-file journal disk IO. Returns
    // the job ids in order (one per input, 0 for any that failed to enqueue
    // because the queue is stopping).
    std::vector<std::uint64_t> enqueueBatch(std::vector<TransferRequest> reqs);

    // Re-enqueue a fresh copy of a terminal (Failed/Canceled/Skipped) job's
    // ORIGINAL request -- same endpoint, same credentials -- no matter what
    // the caller is looking at now, so a retry can never transfer to the
    // wrong server. `conflict` overrides the copy's policy (the UI lets the
    // user flip it before retrying). The job restarts from scratch
    // (resume=false is the safe default: a Skipped job's destination holds
    // unrelated content and must never be appended to). Returns the new job
    // id, or 0 when the job is unknown or not in a retryable state.
    std::uint64_t retry(std::uint64_t jobId, ConflictPolicy conflict);

    // Recursive directory transfer. The template's localPath/remotePath
    // name the ROOTS: uploads walk the local tree, downloads walk the
    // remote tree (SFTP readdir / FTP LIST). One job per file; uploads
    // set ensureRemoteDir so remote parents are created on the fly.
    // Returns the job ids; empty + *error when the walk fails. Events
    // fire exactly like plain enqueue() jobs.
    std::vector<std::uint64_t> enqueueTree(TransferRequest rootTemplate,
                                           std::string* error = nullptr);

    // Queued job: dropped before it starts. Running job: flips the
    // control flag; the backend aborts at its next chunk boundary.
    bool cancel(std::uint64_t jobId);
    // Emergency brake: stops EVERY queued and running job in one lock,
    // drops the pending queue and truncates the durable journal so a
    // restart does not resurrect the batch. Returns the number of live
    // jobs stopped. Unlike calling cancel() N times this is O(n), not
    // O(n^2), which matters when tens of thousands of files are queued.
    std::size_t cancelAll();
    // Number of records stored in the journal (for a startup prompt before
    // restoring anything). Does not mutate the journal or the live queue.
    std::size_t pendingJournalCount(std::string* error = nullptr) const;
    // Remove every record from the durable journal without touching live
    // jobs (used by the startup "discard this batch" choice).
    void clearJournal();

    bool pause(std::uint64_t jobId);    // running job parks between chunks
    bool resume(std::uint64_t jobId);

    // True while a job's pause flag is set (the backend parks between
    // chunks; the job is still Running until it observes the flag). The
    // UI uses this to reflect the requested pause state immediately.
    bool isPaused(std::uint64_t jobId) const;

    // FileZilla-style global "process queue" toggle. When paused, no new
    // job is dequeued and every running job parks at its next chunk
    // boundary, so the whole queue halts as one unit. pauseAll()/resumeAll()
    // flip the global flag; isGloballyPaused() reports it. Per-job pause()
    // still works independently for fine-grained control.
    void pauseAll();
    void resumeAll();
    bool isGloballyPaused() const;

    // FileZilla "速度限制": a global bytes/sec cap applied to every running
    // and future job. 0 disables it. The limit is pushed onto each job's
    // TransferControl.rate, which the backends poll between chunks.
    void setRateLimit(std::uint64_t bytesPerSec);

    // Blocks until no job is queued or running.
    void waitAll();

    // Number of transfer worker threads currently spawned.
    unsigned workerCount() const;

    // Adjust the number of workers at runtime. Growing spawns more threads
    // immediately; shrinking lets the surplus workers finish their current
    // job (if any) and then exit, blocking until they have joined. Bounds
    // are clamped to [1, 64].
    void setWorkers(unsigned n);

    void shutdown();

    std::size_t pending() const;   // queued + running

private:
    struct Job {
        std::uint64_t id = 0;
        TransferRequest req;
        // Atomic: written by the worker (Running -> terminal) without mu_,
        // read by control/retry paths from other threads.
        std::atomic<TransferState> state{TransferState::Queued};
        TransferControl ctl;
    };

    void workerLoop(int workerIndex);
    void runJob(Job* job);
    // Directory-archive pipeline: pack a folder into one zstd tar, transfer
    // it, and extract on the far side (see TransferRequest::dirArchive).
    // Emits the Started/Progress/Finished events itself.
    void runDirArchiveJob(Job* job, const TransferEvent& started);
    void emitEvent(const TransferEvent& ev);
    std::shared_ptr<Job> popJob(int workerIndex);
    bool allIdleLocked() const { return queue_.empty() && running_ == 0; }
    // Move a terminal job out of `all_` into the bounded `doneCache_` so
    // completed work (and its credentials) does not accumulate forever.
    // The cache keeps the last kDoneCache jobs available for retry().
    void retireJob(std::shared_ptr<Job> job);

    mutable std::mutex mu_;
    std::condition_variable cvPop_;    // wakes workers
    std::condition_variable cvIdle_;   // wakes waitAll
    std::deque<std::shared_ptr<Job>> queue_;
    std::vector<std::shared_ptr<Job>> all_;   // id lookup (active jobs only)
    // Recently finished jobs, bounded FIFO: keeps the original request
    // (endpoint + credentials) available for retry() without unbounded
    // memory/credential retention. Oldest entries are dropped on overflow
    // and on shutdown.
    std::deque<std::shared_ptr<Job>> doneCache_;
    static constexpr std::size_t kDoneCache = 256;
    // Failed / canceled / skipped jobs are kept separately and much longer:
    // the GUI can display thousands of failed rows for a large batch, and
    // "retry all" must still find the ORIGINAL request (host + credentials)
    // for a row that failed minutes ago. doneCache_ alone (256 entries)
    // evicted those and made retry() silently return 0.
    std::deque<std::shared_ptr<Job>> failedCache_;
    static constexpr std::size_t kFailedCache = 20000;
    std::vector<std::thread> workers_;
    EventCallback onEvent_;
    int progressMinIntervalMs_ = 100;
    unsigned running_ = 0;
    std::uint64_t nextId_ = 1;
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> workerTarget_{0};   // desired worker count
    std::atomic<bool> globalPause_{false};    // FileZilla "process queue" gate
    std::atomic<std::uint64_t> rateLimit_{0}; // global bytes/sec cap (0=off)

    // Durable journal (optional). Written on enqueue, trimmed on terminal
    // states, replayed by restore(). See transfer_journal.hpp.
    std::unique_ptr<TransferJournal> journal_;
    mutable std::mutex journalMu_;   // serializes journal load-modify-save
};

}  // namespace ftpflow
