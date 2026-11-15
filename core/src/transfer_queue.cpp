// ftpflow -- concurrent transfer queue implementation.
#include "ftpflow/transfer_queue.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <sys/stat.h>

#include "ftpflow/transfer_journal.hpp"
#include "ftpflow/tree_walk.hpp"

namespace ftpflow {

namespace {

// UTF-8 -> wide (Win32 boundary; same policy as vfs_local).
std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0);
    std::wstring w((std::size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// mkdir -p for the local parent chain of a '/'-separated path. Creates
// every missing prefix; "already exists" is not an error. NOTE: the
// loop includes the FINAL slash (index == last) -- that prefix is the
// file's direct parent directory.
void ensureLocalParentDir(const std::string& localPath) {
    const std::size_t last = localPath.find_last_of('/');
    for (std::size_t i = 1; i <= last; ++i) {
        if (localPath[i] != '/') continue;
        const std::string seg = localPath.substr(0, i);
        if (seg.size() < 2 || seg.back() == ':') continue;
        CreateDirectoryW(toWide(seg).c_str(), nullptr);
    }
}

// mkdir -p over the remote parent chain (FTP flavor). Attempts MKD on
// every prefix and ignores failures: existing dirs, drive roots, and
// permission errors all collapse to "still fine to try the transfer".
void ensureRemoteDirFtp(FtpSession& ftp, const std::string& remotePath) {
    std::size_t pos = 0;
    while ((pos = remotePath.find('/', pos + 1)) != std::string::npos) {
        const std::string prefix = remotePath.substr(0, pos);
        if (prefix.size() < 2 || prefix.back() == ':' || prefix == "/")
            continue;
        ftp.mkdir(prefix);
    }
}

// mkdir -p over the remote parent chain (SFTP flavor, same contract).
void ensureRemoteDirSftp(SftpSession& sftp, const std::string& remotePath) {
    std::size_t pos = 0;
    while ((pos = remotePath.find('/', pos + 1)) != std::string::npos) {
        const std::string prefix = remotePath.substr(0, pos);
        if (prefix.size() < 2 || prefix.back() == ':' || prefix == "/")
            continue;
        sftp.mkdir(prefix, 0755);
    }
}

// Throttled progress emitter: computes speed from wall-clock deltas and
// drops events that arrive faster than the configured interval. The final
// update of a job always passes through (force = true).
class ProgressGate {
public:
    explicit ProgressGate(int minIntervalMs)
        : minMs_(minIntervalMs), lastEmit_(std::chrono::steady_clock::now()) {}

    // Returns true when this update should be delivered to the caller.
    bool pass(bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastEmit_).count();
        if (!force && ms < minMs_) return false;
        lastElapsedMs_ = ms > 0 ? ms : 1;
        lastEmit_ = now;
        return true;
    }

    double intervalSeconds() const { return lastElapsedMs_ / 1000.0; }

private:
    int minMs_;
    std::int64_t lastElapsedMs_ = 0;
    std::chrono::steady_clock::time_point lastEmit_;
};

// Builds the stage-3 FileProbe (32B head + head/mid/tail 3x64 KiB) from a
// local file. Best effort: missing file -> empty probe (funnel says Store).
FileProbe probeLocalFile(const std::string& path) {
    FileProbe probe;
    probe.fileName = path;
    struct _stat64 st{};
    if (_stat64(path.c_str(), &st) != 0) return probe;
    probe.fileSize = (std::uint64_t)st.st_size;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return probe;

    constexpr long kWin = 64 * 1024;
    const long size = (long)st.st_size;
    const long mid = size / 2;
    const long tail = size > kWin ? size - kWin : 0;
    for (long off : {0L, mid, tail}) {
        if (off < 0 || off >= size) continue;
        std::fseek(f, off, SEEK_SET);
        std::vector<std::uint8_t> buf(kWin);
        const std::size_t got = std::fread(buf.data(), 1, kWin, f);
        if (off == 0) probe.head.assign(buf.begin(), buf.begin() + (got < 32 ? got : 32));
        probe.sample.insert(probe.sample.end(), buf.data(), buf.data() + got);
    }
    std::fclose(f);
    return probe;
}

// {file} -> "<remote>" (same quoting as the ssh push CLI path).
// append=true swaps the default template for its >> variant (resume mode);
// caller-supplied templates are used verbatim.
std::string buildPushCommand(const TransferRequest& req, bool append) {
    std::string tmpl;
    if (req.pushCommandTemplate.empty()) {
        tmpl = append ? sshDefaultPushAppendTemplate()
                      : sshDefaultPushCommandTemplate();
    } else {
        tmpl = req.pushCommandTemplate;
    }
    std::string cmd = tmpl;
    const std::string quoted = "\"" + req.remotePath + "\"";
    for (std::size_t pos; (pos = cmd.find("{file}")) != std::string::npos;) {
        cmd.replace(pos, 6, quoted);
    }
    return cmd;
}

std::uint64_t localFileSize(const std::string& path) {
    struct _stat64 st{};
    return _stat64(path.c_str(), &st) == 0 ? (std::uint64_t)st.st_size : 0;
}

}  // namespace

TransferQueue::TransferQueue(unsigned workers, int progressMinIntervalMs)
    : progressMinIntervalMs_(progressMinIntervalMs) {
    if (workers == 0) workers = 1;
    if (workers > 64) workers = 64;
    workerTarget_.store(workers);
    for (unsigned i = 0; i < workers; ++i) {
        workers_.emplace_back([this, i] { workerLoop(static_cast<int>(i)); });
    }
}

TransferQueue::~TransferQueue() { shutdown(); }

void TransferQueue::setEventCallback(EventCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    onEvent_ = std::move(cb);
}

void TransferQueue::setJournal(std::string journalPath) {
    std::lock_guard<std::mutex> lk(mu_);
    if (journalPath.empty()) {
        journal_.reset();
    } else {
        journal_ = std::make_unique<TransferJournal>(std::move(journalPath));
    }
}

std::size_t TransferQueue::restore(
        std::string* error, std::function<void(TransferRequest&)> cred) {
    if (!journal_ || !journal_->enabled()) return 0;
    std::vector<JournalRecord> recs;
    std::string err;
    if (!journal_->load(recs, &err)) {
        if (error) *error = err;
        return 0;
    }
    std::size_t restored = 0;
    for (const JournalRecord& rec : recs) {
        TransferRequest req = TransferJournal::toRequest(rec);
        req.resume = true;              // byte-exact continuation
        if (cred) cred(req);
        if (enqueue(std::move(req)) != 0) ++restored;
    }
    return restored;
}

std::uint64_t TransferQueue::enqueue(TransferRequest req) {
    auto job = std::make_shared<Job>();
    job->req = std::move(req);
    std::uint64_t id = 0;
    TransferRequest snapshot;   // copy for the journal (req is moved below)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_.load()) return 0;
        id = nextId_++;
        job->id = id;
        snapshot = job->req;
        all_.push_back(job);
        queue_.push_back(std::move(job));
    }
    // Persist BEFORE waking a worker, so the journal is complete by the
    // time the job can reach a terminal state and be trimmed.
    if (journal_ && journal_->enabled()) {
        std::lock_guard<std::mutex> lk(journalMu_);
        std::vector<JournalRecord> recs;
        std::string err;
        if (journal_->load(recs, &err)) {
            recs.push_back(TransferJournal::fromRequest(id, snapshot));
            journal_->save(recs, &err);
        }
    }
    cvPop_.notify_one();
    return id;
}

std::vector<std::uint64_t> TransferQueue::enqueueTree(TransferRequest rootTemplate,
                                                      std::string* error) {
    std::vector<std::uint64_t> ids;
    std::vector<TreeEntry> entries;
    std::string walkErr;

    if (rootTemplate.direction == TransferDirection::Upload) {
        if (!walkLocalTree(rootTemplate.localPath, entries, &walkErr)) {
            if (error) *error = "walk " + rootTemplate.localPath + ": " + walkErr;
            return ids;
        }
    } else if (rootTemplate.protocol == BackendKind::Ftp) {
        FtpSession ftp;
        const FtpResult r = ftp.open(rootTemplate.ftp);
        if (!r.ok()) {
            if (error) *error = "ftp open: " + r.message;
            return ids;
        }
        if (!walkRemoteTree(ftp, rootTemplate.remotePath, entries, &walkErr)) {
            if (error) *error = "walk " + rootTemplate.remotePath + ": " + walkErr;
            return ids;
        }
    } else {
        const HostKeyPolicy hkPolicy = rootTemplate.trustedHostKeyHex.empty()
            ? HostKeyPolicy::AcceptNew : HostKeyPolicy::Strict;
        SftpSession sftp(hkPolicy);
        std::string acceptedKey;
        const SftpResult r = sftp.open(rootTemplate.ssh,
                                       rootTemplate.trustedHostKeyHex,
                                       &acceptedKey);
        if (!r.ok()) {
            if (error) *error = "sftp open: " + r.message;
            return ids;
        }
        if (!walkRemoteTree(sftp, rootTemplate.remotePath, entries, &walkErr)) {
            if (error) *error = "walk " + rootTemplate.remotePath + ": " + walkErr;
            return ids;
        }
    }

    for (const TreeEntry& e : entries) {
        if (e.isDir) continue;   // created on the fly by the file jobs
        TransferRequest req = rootTemplate;
        req.localPath = joinTreePath(rootTemplate.localPath, e.relPath);
        req.remotePath = joinTreePath(rootTemplate.remotePath, e.relPath);
        if (rootTemplate.direction == TransferDirection::Upload)
            req.ensureRemoteDir = true;
        const std::uint64_t id = enqueue(std::move(req));
        if (id != 0) ids.push_back(id);
    }
    return ids;
}

bool TransferQueue::cancel(std::uint64_t jobId) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& j : all_) {
        if (j->id != jobId) continue;
        if (j->state == TransferState::Queued) j->state = TransferState::Canceled;
        j->ctl.cancel = true;   // running: backend aborts at next chunk
        return true;
    }
    return false;
}

bool TransferQueue::pause(std::uint64_t jobId) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& j : all_) {
        if (j->id == jobId) { j->ctl.pause = true; return true; }
    }
    return false;
}

bool TransferQueue::resume(std::uint64_t jobId) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& j : all_) {
        if (j->id == jobId) { j->ctl.pause = false; return true; }
    }
    return false;
}

bool TransferQueue::isPaused(std::uint64_t jobId) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& j : all_) {
        if (j->id == jobId) return j->ctl.pause;
    }
    return false;
}

void TransferQueue::waitAll() {
    std::unique_lock<std::mutex> lk(mu_);
    cvIdle_.wait(lk, [this] { return allIdleLocked(); });
}

void TransferQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_.exchange(true)) return;
        for (auto& j : queue_) j->state = TransferState::Canceled;
        for (auto& j : all_) j->ctl.cancel = true;
        queue_.clear();
    }
    cvPop_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

std::size_t TransferQueue::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size() + running_;
}

unsigned TransferQueue::workerCount() const {
    return workerTarget_.load();
}

void TransferQueue::setWorkers(unsigned n) {
    if (n == 0) n = 1;
    if (n > 64) n = 64;
    const unsigned cur = workerTarget_.load();
    if (n == cur) return;

    if (n < cur) {
        // Shrink: surplus workers finish their current job, then exit. The
        // workers with index >= n are the ones to retire.
        workerTarget_.store(n);
        cvPop_.notify_all();   // wake any idle surplus workers to observe the target
        // Join the retiring workers (index n..cur-1). They are the tail of
        // workers_; join blocks until each has observed the target and left
        // workerLoop (after finishing its in-flight job, if any).
        for (unsigned i = n; i < cur; ++i) {
            if (i < workers_.size() && workers_[i].joinable()) workers_[i].join();
        }
        workers_.erase(workers_.begin() + n, workers_.end());
    } else {
        // Grow: spawn n - cur additional workers.
        workerTarget_.store(n);
        for (unsigned i = cur; i < n; ++i) {
            workers_.emplace_back([this, i] { workerLoop(static_cast<int>(i)); });
        }
    }
}

void TransferQueue::emitEvent(const TransferEvent& ev) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cb = onEvent_;
    }
    if (cb) cb(ev);
}

std::shared_ptr<TransferQueue::Job> TransferQueue::popJob(int workerIndex) {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        cvPop_.wait(lk, [this, workerIndex] {
            return stopping_.load() || !queue_.empty() ||
                   workerIndex >= (int)workerTarget_.load();
        });
        if (stopping_.load()) return nullptr;
        if (workerIndex >= (int)workerTarget_.load()) return nullptr;  // surplus
        if (!queue_.empty()) {
            auto job = queue_.front();
            queue_.pop_front();
            // The queue shrank outside a RunningGuard (canceled-while-queued
            // jobs never run): wake waitAll here or it can sleep forever.
            if (allIdleLocked()) cvIdle_.notify_all();
            // Canceled jobs return with state == Canceled; the worker emits
            // their terminal event without running them.
            if (job->state != TransferState::Canceled) {
                job->state = TransferState::Running;
                ++running_;
            }
            return job;
        }
    }
}

void TransferQueue::workerLoop(int workerIndex) {
    for (;;) {
        std::shared_ptr<Job> job = popJob(workerIndex);
        if (!job) return;

        if (job->state == TransferState::Canceled) {
            // Canceled while queued: emit the terminal event without
            // running (popJob left running_ untouched for this job).
            TransferEvent fe;
            fe.jobId = job->id;
            fe.kind = TransferEvent::Kind::Finished;
            fe.state = TransferState::Canceled;
            emitEvent(fe);
            if (journal_ && journal_->enabled()) {
                std::lock_guard<std::mutex> lk(journalMu_);
                std::string jerr;
                journal_->remove(job->id, &jerr);
            }
            continue;
        }

        // Decrements running_ and wakes waitAll when this job completes.
        struct RunningGuard {
            TransferQueue& q;
            ~RunningGuard() {
                std::lock_guard<std::mutex> lk(q.mu_);
                --q.running_;
                if (q.allIdleLocked()) q.cvIdle_.notify_all();
            }
        } guard{*this};

        runJob(job.get());
    }
}

void TransferQueue::runJob(Job* job) {
    const TransferRequest& req = job->req;
    TransferEvent ev;
    ev.jobId = job->id;

    // Strict host-key verification when the caller supplied trusted blobs;
    // otherwise trust-on-first-use (AcceptNew), the legacy default.
    const HostKeyPolicy hkPolicy = req.trustedHostKeyHex.empty()
        ? HostKeyPolicy::AcceptNew
        : HostKeyPolicy::Strict;

    // --- pick the pipeline --------------------------------------------------
    TransferMode mode = TransferMode::Store;
    Decision decision;
    if (req.ftzArchive) {
        mode = TransferMode::Ftz1;
    } else if (req.protocol == BackendKind::Sftp &&
               req.direction == TransferDirection::Upload) {
        if (req.autoDecide) {
            const FileProbe probe = probeLocalFile(req.localPath);
            decision = decideFullFunnel(probe, req.costModel);
            mode = decision.verdict == Verdict::Compress
                ? TransferMode::Compress
                : TransferMode::Store;
        } else {
            mode = req.mode;
        }
    }
    ev.kind = TransferEvent::Kind::Started;
    ev.mode = mode;
    ev.decision = decision;
    ev.state = TransferState::Running;
    ev.localPath = req.localPath;
    ev.remotePath = req.remotePath;
    ev.direction = req.direction;
    emitEvent(ev);

    ProgressGate gate(progressMinIntervalMs_);
    TransferProgress tp;
    tp.compressionRatio = 1.0;

    // Downloads always materialize the local parent chain (recursive
    // downloads land in trees that may not exist yet).
    if (req.direction == TransferDirection::Download)
        ensureLocalParentDir(req.localPath);

    auto emitProgress = [&](bool force = false) {
        if (!gate.pass(force)) return;
        tp.speedBytesPerSec = tp.bytesMoved > 0
            ? (double)tp.bytesMoved / gate.intervalSeconds()
            : 0.0;
        TransferEvent pe;
        pe.jobId = job->id;
        pe.kind = TransferEvent::Kind::Progress;
        pe.state = TransferState::Running;
        pe.mode = mode;
        pe.progress = tp;
        emitEvent(pe);
    };

    std::string error;
    TransferState finalState = TransferState::Done;
    // Already-transferred prefix picked up by resume mode. Backends count
    // progress differently (curl includes the prefix, SFTP does not), so
    // mapProgress normalizes to "moved counts the prefix exactly once".
    std::uint64_t resumePrefix = 0;
    auto mapProgress = [&](std::uint64_t moved, std::uint64_t total) {
        if (moved >= resumePrefix) {
            tp.bytesMoved = moved;
            tp.bytesTotal = total;
        } else {
            tp.bytesMoved = resumePrefix + moved;
            tp.bytesTotal = resumePrefix + total;
        }
        tp.bytesOnWire = tp.bytesMoved;
        tp.compressionRatio = 1.0;
        emitProgress();
    };

    if (mode == TransferMode::Ftz1 && req.protocol == BackendKind::Ftp) {
        error = "ftz archive requires an sftp/ssh endpoint";
        finalState = TransferState::Failed;
    } else if (mode == TransferMode::Ftz1) {
        // ---- FTZ1 container pipeline (SFTP endpoints, both ways) --------
        SftpSession sftp(hkPolicy);
        std::string acceptedKey;
        SftpResult r = sftp.open(req.ssh, req.trustedHostKeyHex, &acceptedKey);
        if (!r.ok()) {
            error = std::string("sftp open: ") + r.message;
            finalState = TransferState::Failed;
        } else if (req.direction == TransferDirection::Upload) {
            // local -> container -> remotePath + ".ftz1"
            const std::string container = req.localPath + ".ftz1.part";
            FtzInfo info;
            const FtzStatus fs = ftzWrite(req.localPath, container, 8,
                                          req.zstdLevel, &info);
            if (fs != FtzStatus::Ok) {
                error = std::string("ftzWrite: ") + ftzStatusName(fs);
                finalState = TransferState::Failed;
            } else {
                const std::uint64_t containerSize = localFileSize(container);
                tp.compressionRatio = info.originalSize > 0
                    ? (double)containerSize / (double)info.originalSize
                    : 1.0;
                sftp.setProgressCallback(
                    [&](std::uint64_t moved, std::uint64_t total) {
                        tp.bytesMoved = moved;
                        tp.bytesTotal = total;
                        tp.bytesOnWire = moved;
                        emitProgress();
                    });
                std::uint64_t moved = 0;
                SftpResult tr = sftp.upload(container, req.remotePath + ".ftz1",
                                            0, &moved, &job->ctl);
                std::remove(container.c_str());
                if (tr.status == SftpStatus::Canceled) {
                    finalState = TransferState::Canceled;
                } else if (!tr.ok()) {
                    error = tr.message;
                    finalState = TransferState::Failed;
                } else {
                    tp.bytesMoved = tp.bytesTotal = containerSize;
                    tp.bytesOnWire = containerSize;
                }
            }
        } else {
            // remotePath is the container; localPath the extraction target.
            const std::string container = req.localPath + ".ftz1.part";
            sftp.setProgressCallback(
                [&](std::uint64_t moved, std::uint64_t total) {
                    tp.bytesMoved = moved;
                    tp.bytesTotal = total;
                    tp.bytesOnWire = moved;
                    emitProgress();
                });
            std::uint64_t moved = 0;
            SftpResult tr = sftp.download(req.remotePath, container, 0, &moved,
                                          &job->ctl);
            if (tr.status == SftpStatus::Canceled) {
                finalState = TransferState::Canceled;
                std::remove(container.c_str());
            } else if (!tr.ok()) {
                error = tr.message;
                finalState = TransferState::Failed;
                std::remove(container.c_str());
            } else {
                // Frame-level hash verification; corrupt frames are named.
                std::vector<std::uint32_t> bad;
                const FtzStatus fs = ftzExtract(container, req.localPath, &bad);
                std::remove(container.c_str());
                if (fs != FtzStatus::Ok) {
                    error = std::string("ftzExtract: ") + ftzStatusName(fs);
                    if (!bad.empty()) {
                        error += " (bad frames";
                        for (std::uint32_t f : bad)
                            error += " " + std::to_string(f);
                        error += ")";
                    }
                    finalState = TransferState::Failed;
                } else {
                    tp.bytesMoved = tp.bytesTotal = moved;
                    tp.bytesOnWire = moved;
                }
            }
        }
    } else if (req.protocol == BackendKind::Ftp) {
        FtpSession ftp;
        FtpResult r = ftp.open(req.ftp);
        if (!r.ok()) {
            error = std::string("ftp open: ") + r.message;
            finalState = TransferState::Failed;
        } else {
            if (req.direction == TransferDirection::Upload &&
                req.ensureRemoteDir) {
                ensureRemoteDirFtp(ftp, req.remotePath);
            }
            if (req.resume) {
                if (req.direction == TransferDirection::Upload) {
                    std::uint64_t remoteSize = 0;
                    if (ftp.size(req.remotePath, &remoteSize).ok())
                        resumePrefix = remoteSize;
                } else {
                    resumePrefix = localFileSize(req.localPath);
                }
            }
            ftp.setProgressCallback(
                [&](std::uint64_t moved, std::uint64_t total) {
                    mapProgress(moved, total);
                });
            std::uint64_t moved = 0;
            FtpResult tr = req.direction == TransferDirection::Upload
                ? ftp.upload(req.localPath, req.remotePath, resumePrefix,
                             &moved, &job->ctl)
                : ftp.download(req.remotePath, req.localPath, resumePrefix,
                               &moved, &job->ctl);
            if (tr.status == FtpStatus::Canceled) {
                finalState = TransferState::Canceled;
            } else if (!tr.ok()) {
                error = tr.message;
                finalState = TransferState::Failed;
            } else {
                tp.bytesMoved = resumePrefix + moved;
                tp.bytesTotal = std::max(tp.bytesTotal, tp.bytesMoved);
                tp.bytesOnWire = tp.bytesMoved;
            }
        }
    } else if (mode == TransferMode::Store) {
        // SFTP: reuse this worker's session when the endpoint matches.
        // (workerLoop owns the cache; for simplicity we re-open per job here
        // -- session caching arrives with the GUI connection manager.)
        SftpSession sftp(hkPolicy);
        std::string acceptedKey;
        SftpResult r = sftp.open(req.ssh, req.trustedHostKeyHex, &acceptedKey);
        if (!r.ok()) {
            error = std::string("sftp open: ") + r.message;
            finalState = TransferState::Failed;
        } else {
            if (req.direction == TransferDirection::Upload &&
                req.ensureRemoteDir) {
                ensureRemoteDirSftp(sftp, req.remotePath);
            }
            if (req.resume) {
                if (req.direction == TransferDirection::Upload) {
                    std::uint64_t remoteSize = 0;
                    if (sftp.size(req.remotePath, &remoteSize).ok())
                        resumePrefix = remoteSize;
                } else {
                    resumePrefix = localFileSize(req.localPath);
                }
            }
            sftp.setProgressCallback(
                [&](std::uint64_t moved, std::uint64_t total) {
                    mapProgress(moved, total);
                });
            std::uint64_t moved = 0;
            SftpResult tr = req.direction == TransferDirection::Upload
                ? sftp.upload(req.localPath, req.remotePath, resumePrefix,
                              &moved, &job->ctl)
                : sftp.download(req.remotePath, req.localPath, resumePrefix,
                                &moved, &job->ctl);
            if (tr.status == SftpStatus::Canceled) {
                finalState = TransferState::Canceled;
            } else if (!tr.ok()) {
                error = tr.message;
                finalState = TransferState::Failed;
            } else {
                tp.bytesMoved = resumePrefix + moved;
                tp.bytesTotal = std::max(tp.bytesTotal, tp.bytesMoved);
                tp.bytesOnWire = tp.bytesMoved;
            }
        }
    } else {
        // Compress: one exec channel per job, streaming zstd. Resume stats
        // the remote partial through SFTP and appends from that offset.
        const std::uint64_t localSize = localFileSize(req.localPath);
        std::uint64_t resumeFrom = 0;
        bool alreadyComplete = false;

        // One SFTP helper session covers mkdir -p (recursive uploads),
        // the resume stat, and the post-push size verification.
        std::unique_ptr<SftpSession> sftpHelper;
        if (req.direction == TransferDirection::Upload &&
            (req.ensureRemoteDir || req.resume)) {
            sftpHelper = std::make_unique<SftpSession>(hkPolicy);
            std::string hk;
            if (sftpHelper->open(req.ssh, req.trustedHostKeyHex, &hk).ok()) {
                if (req.ensureRemoteDir)
                    ensureRemoteDirSftp(*sftpHelper, req.remotePath);
                if (req.resume) {
                    std::uint64_t remoteSize = 0;
                    if (sftpHelper->size(req.remotePath, &remoteSize).ok()) {
                        if (remoteSize == localSize) {
                            alreadyComplete = true;
                        } else if (remoteSize < localSize) {
                            resumeFrom = remoteSize;
                        } else {
                            // Remote longer than local (stale file): restart
                            // from scratch so the -o template can truncate.
                            sftpHelper->removeFile(req.remotePath);
                        }
                    }
                }
            }
        }
        if (alreadyComplete) {
            // Nothing to ship: the remote already holds the exact file.
            tp.bytesMoved = tp.bytesTotal = localSize;
            TransferEvent fe;
            fe.jobId = job->id;
            fe.kind = TransferEvent::Kind::Finished;
            fe.state = TransferState::Done;
            fe.mode = mode;
            fe.progress = tp;
            job->state = TransferState::Done;
            emitEvent(fe);
            return;
        }

        SshExec ssh(hkPolicy);
        std::string acceptedKey;
        SshResult r = ssh.open(req.ssh, buildPushCommand(req, resumeFrom > 0),
                               req.trustedHostKeyHex, &acceptedKey);
        if (!r.ok()) {
            error = std::string("ssh open: ") + r.message;
            finalState = TransferState::Failed;
        } else {
            SshPushOptions opts;
            opts.zstdLevel = req.zstdLevel;
            opts.control = &job->ctl;
            opts.resumeFrom = resumeFrom;
            resumePrefix = resumeFrom;
            SshPushStats st;
            opts.onProgress = [&](const SshPushStats& s) {
                tp.bytesMoved = resumePrefix + s.localBytes;
                tp.bytesOnWire = s.compressedBytes;
                tp.bytesTotal = localSize;
                tp.compressionRatio = s.localBytes > 0
                    ? (double)s.compressedBytes / (double)s.localBytes
                    : 1.0;
                emitProgress();
            };
            SshResult pr = sshPushFile(ssh, req.localPath, opts, &st);
            if (pr.status == SshStatus::Canceled) {
                finalState = TransferState::Canceled;
            } else if (!pr.ok()) {
                error = pr.message;
                finalState = TransferState::Failed;
            } else {
                tp.bytesMoved = resumePrefix + st.localBytes;
                tp.bytesTotal = localSize;
                tp.bytesOnWire = st.compressedBytes;
                tp.compressionRatio = st.localBytes > 0
                    ? (double)st.compressedBytes / (double)st.localBytes
                    : 1.0;
                // 0-byte-drift check: the remote file must now be exactly
                // localSize bytes (upload direction only).
                if (req.direction == TransferDirection::Upload) {
                    std::unique_ptr<SftpSession> verify;
                    if (sftpHelper && sftpHelper->isOpen()) {
                        verify = std::move(sftpHelper);
                    } else {
                        verify = std::make_unique<SftpSession>(hkPolicy);
                        std::string vk;
                        verify->open(req.ssh, req.trustedHostKeyHex, &vk);
                    }
                    std::uint64_t remoteSize = 0;
                    if (verify->isOpen() &&
                        verify->size(req.remotePath, &remoteSize).ok() &&
                        remoteSize != localSize) {
                        error = "size mismatch after push: remote " +
                                std::to_string(remoteSize) + " vs local " +
                                std::to_string(localSize);
                        finalState = TransferState::Failed;
                    }
                }
            }
        }
    }

    // Final event: always forced so the last sample is not swallowed by
    // the throttle.
    TransferEvent fe;
    fe.jobId = job->id;
    fe.kind = TransferEvent::Kind::Finished;
    fe.state = finalState;
    fe.mode = mode;
    fe.progress = tp;
    fe.error = error;
    job->state = finalState;
    emitEvent(fe);

    // Trim the journal: a terminal job is no longer resumable.
    if (journal_ && journal_->enabled()) {
        std::lock_guard<std::mutex> lk(journalMu_);
        std::string jerr;
        journal_->remove(job->id, &jerr);
    }
}

}  // namespace ftpflow
