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

enum class TransferState : std::uint8_t {
    Queued, Running, Done, Failed, Canceled,
};

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
};

struct TransferEvent {
    enum class Kind : std::uint8_t { Started, Progress, Finished };
    Kind kind = Kind::Started;
    std::uint64_t jobId = 0;
    TransferState state = TransferState::Queued;
    TransferMode mode = TransferMode::Store;   // effective pipeline
    TransferProgress progress;
    Decision decision;                          // funnel result (Started)
    std::string error;                          // Failed detail
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

    std::uint64_t enqueue(TransferRequest req);   // -> jobId (1-based)

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
    bool pause(std::uint64_t jobId);    // running job parks between chunks
    bool resume(std::uint64_t jobId);

    // Blocks until no job is queued or running.
    void waitAll();

    void shutdown();

    std::size_t pending() const;   // queued + running

private:
    struct Job {
        std::uint64_t id = 0;
        TransferRequest req;
        TransferState state = TransferState::Queued;
        TransferControl ctl;
    };

    void workerLoop(int workerIndex);
    void runJob(Job* job);
    void emitEvent(const TransferEvent& ev);
    std::shared_ptr<Job> popJob();
    bool allIdleLocked() const { return queue_.empty() && running_ == 0; }

    mutable std::mutex mu_;
    std::condition_variable cvPop_;    // wakes workers
    std::condition_variable cvIdle_;   // wakes waitAll
    std::deque<std::shared_ptr<Job>> queue_;
    std::vector<std::shared_ptr<Job>> all_;   // id lookup (kept for control)
    std::vector<std::thread> workers_;
    EventCallback onEvent_;
    int progressMinIntervalMs_ = 100;
    unsigned running_ = 0;
    std::uint64_t nextId_ = 1;
    std::atomic<bool> stopping_{false};
};

}  // namespace ftpflow
