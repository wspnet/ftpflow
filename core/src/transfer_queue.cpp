// ftpflow -- concurrent transfer queue implementation.
#include "ftpflow/transfer_queue.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <sys/stat.h>

#include "ftpflow/transfer_journal.hpp"
#include "ftpflow/text_codec.hpp"
#include "ftpflow/tree_walk.hpp"
#include "ftpflow/dir_archive.hpp"
#include "ftpflow/shell_quote.hpp"

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
    if (_wstat64(toWide(path).c_str(), &st) != 0) return probe;
    probe.fileSize = (std::uint64_t)st.st_size;
    std::FILE* f = openLocalFile(path, "rb");
    if (!f) return probe;

    constexpr __int64 kWin = 64 * 1024;
    const __int64 size = (__int64)st.st_size;
    const __int64 mid = size / 2;
    const __int64 tail = size > kWin ? size - kWin : 0;
    for (__int64 off : {(__int64)0, mid, tail}) {
        if (off < 0 || off >= size) continue;
        _fseeki64(f, off, SEEK_SET);
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
    // POSIX-safe quoting: the remote shell must see the path as one literal
    // word. The old inline `"\"" + path + "\""` let $/backtick/quote in a
    // filename run commands on the server.
    const std::string quoted = shellQuote(req.remotePath);
    for (std::size_t pos; (pos = cmd.find("{file}")) != std::string::npos;) {
        cmd.replace(pos, 6, quoted);
    }
    return cmd;
}

std::uint64_t localFileSize(const std::string& path) {
    struct _stat64 st{};
    return _wstat64(toWide(path).c_str(), &st) == 0
        ? (std::uint64_t)st.st_size : 0;
}

// Stat a LOCAL file (UTF-8 path) into a RemoteStat-compatible shape so the
// conflict policy can compare local vs remote uniformly.
RemoteStat localStat(const std::string& path) {
    RemoteStat s;
    struct _stat64 st{};
    if (_wstat64(toWide(path).c_str(), &st) == 0) {
        s.exists = true;
        s.isDir = (st.st_mode & _S_IFDIR) != 0;
        s.size = (std::uint64_t)st.st_size;
        s.mtimeUnix = (std::int64_t)st.st_mtime;
    }
    return s;
}

// Stat the request's remote path, opening a short-lived session. Best effort:
// on any failure `out` keeps exists == false (treated as "no conflict").
RemoteStat remoteStatFor(const TransferRequest& req, HostKeyPolicy hkPolicy) {
    RemoteStat s;
    if (req.protocol == BackendKind::Ftp) {
        FtpSession f;
        if (f.open(req.ftp).ok()) {
            FtpResult r = f.stat(req.remotePath, &s);
            (void)r;
        }
    } else {
        SftpSession sftp(hkPolicy);
        std::string k;
        if (sftp.open(req.ssh, req.trustedHostKeyHex, &k).ok()) {
            SftpResult r = sftp.stat(req.remotePath, &s);
            (void)r;
        }
    }
    return s;
}

}  // namespace

void throttle(const TransferControl* ctl, std::uint64_t bytes) {
    if (!ctl || bytes == 0) return;
    const std::uint64_t limit = ctl->rate.bytesPerSecLimit.load();
    if (limit == 0) return;
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // (Re)open the accounting window; every backend calls this between chunks
    // so the window naturally slides forward with actual transfer progress.
    if (ctl->rate.windowStartUs == 0) ctl->rate.windowStartUs = (std::uint64_t)now;
    ctl->rate.windowBytes += bytes;

    // Expected elapsed µs for the bytes credited so far at `limit` B/s.
    const double expectedUs =
        (double)ctl->rate.windowBytes * 1000000.0 / (double)limit;
    const std::uint64_t elapsedUs =
        (std::uint64_t)now - ctl->rate.windowStartUs;
    if ((double)elapsedUs >= expectedUs) {
        // We're on pace (or behind); reset the window so the limiter doesn't
        // accumulate credit and later sleep in one huge burst.
        ctl->rate.windowStartUs = (std::uint64_t)now;
        ctl->rate.windowBytes = 0;
        return;
    }
    const std::uint64_t sleepUs = (std::uint64_t)expectedUs - elapsedUs;
    std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
    // Re-anchor the window at "now + slept" for the next chunk.
    ctl->rate.windowStartUs = (std::uint64_t)now + sleepUs;
    ctl->rate.windowBytes = 0;
}

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
        const std::uint64_t newId = enqueue(std::move(req));
        if (newId != 0) {
            ++restored;
            // Drop the replayed record: enqueue() already journaled the
            // re-born job under its new id. Leaving the old record behind
            // would resurrect it again on every following launch.
            std::string jerr;
            journal_->remove(rec.jobId, &jerr);
        }
    }
    return restored;
}

std::uint64_t TransferQueue::enqueue(TransferRequest req) {
    auto job = std::make_shared<Job>();
    job->req = std::move(req);
    job->ctl.rate.bytesPerSecLimit = rateLimit_.load();   // inherit global cap
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
        std::string err;
        journal_->add(TransferJournal::fromRequest(id, snapshot), &err);
    }
    // Announce the newly queued job immediately so the UI can list every
    // pending file up front (rather than only once a worker starts it).
    TransferEvent qe;
    qe.kind = TransferEvent::Kind::Queued;
    qe.jobId = id;
    qe.state = TransferState::Queued;
    qe.localPath = snapshot.localPath;
    qe.remotePath = snapshot.remotePath;
    qe.direction = snapshot.direction;
    qe.mode = snapshot.mode;
    emitEvent(qe);
    cvPop_.notify_one();
    return id;
}

std::vector<std::uint64_t> TransferQueue::enqueueBatch(
    std::vector<TransferRequest> reqs) {
    std::vector<std::uint64_t> ids;
    ids.reserve(reqs.size());
    if (reqs.empty()) return ids;

    // Commit everything under one lock, but keep a shared_ptr to each job so
    // we can emit events outside the lock (emitEvent re-locks mu_).
    std::vector<std::shared_ptr<Job>> added;
    added.reserve(reqs.size());
    std::vector<JournalRecord> jrecs;
    const bool journaling = journal_ && journal_->enabled();
    if (journaling) jrecs.reserve(reqs.size());

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_.load()) {
            ids.assign(reqs.size(), 0);
            return ids;
        }
        for (auto& req : reqs) {
            auto job = std::make_shared<Job>();
            job->req = std::move(req);
            job->ctl.rate.bytesPerSecLimit = rateLimit_.load();
            job->id = nextId_++;
            ids.push_back(job->id);
            if (journaling)
                jrecs.push_back(
                    TransferJournal::fromRequest(job->id, job->req));
            all_.push_back(job);
            queue_.push_back(job);
            added.push_back(std::move(job));
        }
    }

    // One journal write for the whole batch (persist before waking workers).
    if (journaling && !jrecs.empty()) {
        std::lock_guard<std::mutex> lk(journalMu_);
        std::string err;
        journal_->addBatch(jrecs, &err);
    }

    // Announce each freshly queued job, then wake the workers once.
    for (const auto& job : added) {
        TransferEvent qe;
        qe.kind = TransferEvent::Kind::Queued;
        qe.jobId = job->id;
        qe.state = TransferState::Queued;
        qe.localPath = job->req.localPath;
        qe.remotePath = job->req.remotePath;
        qe.direction = job->req.direction;
        qe.mode = job->req.mode;
        emitEvent(qe);
    }
    cvPop_.notify_all();
    return ids;
}

std::uint64_t TransferQueue::retry(std::uint64_t jobId, ConflictPolicy conflict) {
    std::shared_ptr<Job> src;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& j : all_) {
            if (j->id == jobId) { src = j; break; }
        }
        if (!src) {
            for (const auto& j : doneCache_) {
                if (j->id == jobId) { src = j; break; }
            }
        }
        if (!src) {
            // Failed rows can outlive the small doneCache_ window by a long
            // margin in a 7,000-file batch; they live here until the user
            // removes them or the queue shuts down.
            for (const auto& j : failedCache_) {
                if (j->id == jobId) { src = j; break; }
            }
        }
        if (!src) return 0;
        // Check the state under the same lock that owns the lookup so a
        // concurrent terminal transition cannot slip between the two.
        const TransferState s = src->state.load();
        if (s != TransferState::Failed && s != TransferState::Canceled &&
            s != TransferState::Skipped)
            return 0;
    }
    // Fresh copy of the ORIGINAL request: the retry talks to the same
    // endpoint with the same credentials, never to whatever the caller is
    // looking at now. Restart from scratch (resume=false): a Skipped job's
    // destination holds unrelated content and must not be appended to.
    TransferRequest req = src->req;
    req.conflict = conflict;
    req.resume = false;
    return enqueue(std::move(req));
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
        if (rootTemplate.enqueueAbort &&
            rootTemplate.enqueueAbort->load(std::memory_order_relaxed)) {
            if (error) *error = "已停止：扫描结果未加入队列";
            return ids;
        }
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

std::size_t TransferQueue::cancelAll() {
    std::size_t stopped = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = all_.begin(); it != all_.end();) {
            const TransferState s = (*it)->state.load();
            if (s == TransferState::Queued) {
                // These jobs are dropped below and never reach a worker, so
                // no Finished event is emitted for them. Erase them from the
                // lookup set as well, otherwise a huge canceled batch would
                // stay resident (with credentials) until shutdown().
                (*it)->state = TransferState::Canceled;
                it = all_.erase(it);
                ++stopped;
            } else {
                if (s == TransferState::Running)
                    (*it)->ctl.cancel = true;   // abort at next chunk
                ++it;
            }
        }
        queue_.clear();
        // Emergency stop wipes the UI rows too; do not retain credentials of
        // old terminal jobs that can no longer be retried from the list.
        doneCache_.clear();
        failedCache_.clear();
    }
    cvPop_.notify_all();   // wake parked workers so they observe the empty queue
    cvIdle_.notify_all();
    clearJournal();
    return stopped;
}

std::size_t TransferQueue::pendingJournalCount(std::string* error) const {
    if (!journal_ || !journal_->enabled()) return 0;
    std::vector<JournalRecord> recs;
    std::string err;
    if (!journal_->load(recs, &err)) {
        if (error) *error = err;
        return 0;
    }
    return recs.size();
}

void TransferQueue::clearJournal() {
    if (!journal_ || !journal_->enabled()) return;
    std::lock_guard<std::mutex> lk(journalMu_);
    std::string err;
    journal_->save({}, &err);   // atomic replace: no records survive a restart
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

void TransferQueue::pauseAll() {
    globalPause_.store(true);
    // Park every running job at its next chunk boundary so the queue halts
    // as one unit (mirrors the per-job pause flag the backends poll).
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& j : all_) j->ctl.pause = true;
}

void TransferQueue::resumeAll() {
    globalPause_.store(false);
    // Un-park jobs; note this clears any per-job pause as well (the global
    // toggle owns the whole queue in FileZilla semantics).
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& j : all_) j->ctl.pause = false;
    cvPop_.notify_all();   // wake workers to resume dequeuing
}

bool TransferQueue::isGloballyPaused() const {
    return globalPause_.load();
}

void TransferQueue::setRateLimit(std::uint64_t bytesPerSec) {
    rateLimit_.store(bytesPerSec);
    // Push the new cap onto every live job so running transfers adjust
    // immediately (backends poll `rate.bytesPerSecLimit` between chunks).
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& j : all_) j->ctl.rate.bytesPerSecLimit = bytesPerSec;
}

void TransferQueue::waitAll() {
    std::unique_lock<std::mutex> lk(mu_);
    // `stopping_` is part of the predicate: once shutdown() has begun the
    // queue is dead and waitAll must not block forever on a job that is
    // being aborted (or on a queue_ that shutdown just cleared).
    cvIdle_.wait(lk, [this] {
        return allIdleLocked() || stopping_.load();
    });
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
    // Clearing the queue can make the pool idle: wake waitAll sleepers so
    // they can observe stopping_ / the empty queue (previously they could
    // sleep forever here).
    cvIdle_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    // Drop every retained job (and its credentials) at teardown.
    {
        std::lock_guard<std::mutex> lk(mu_);
        all_.clear();
        doneCache_.clear();
        failedCache_.clear();
    }
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
            return stopping_.load() || (!globalPause_.load() && !queue_.empty()) ||
                   workerIndex >= (int)workerTarget_.load();
        });
        if (stopping_.load()) return nullptr;
        if (workerIndex >= (int)workerTarget_.load()) return nullptr;  // surplus
        if (globalPause_.load()) continue;   // queue halted: wait for resume
        if (!queue_.empty()) {
            auto job = queue_.front();
            queue_.pop_front();
            // The queue shrank outside a RunningGuard (canceled-while-queued
            // jobs never run): wake waitAll here or it can sleep forever.
            if (allIdleLocked()) cvIdle_.notify_all();
            // Canceled jobs return with state == Canceled; the worker emits
            // their terminal event without running them.
            if (job->state.load() != TransferState::Canceled) {
                job->state = TransferState::Running;
                ++running_;
            }
            return job;
        }
    }
}

void TransferQueue::retireJob(std::shared_ptr<Job> job) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = all_.begin(); it != all_.end(); ++it) {
        if (it->get() == job.get()) { all_.erase(it); break; }
    }
    const TransferState s = job->state.load();
    if (s == TransferState::Failed || s == TransferState::Canceled ||
        s == TransferState::Skipped) {
        // Keep terminal failures retryable far beyond the small done cache.
        failedCache_.push_back(std::move(job));
        while (failedCache_.size() > kFailedCache) failedCache_.pop_front();
    } else {
        doneCache_.push_back(std::move(job));
        while (doneCache_.size() > kDoneCache) doneCache_.pop_front();
    }
}

void TransferQueue::workerLoop(int workerIndex) {
    for (;;) {
        std::shared_ptr<Job> job = popJob(workerIndex);
        if (!job) return;

        if (job->state.load() == TransferState::Canceled) {
            // Canceled while queued: emit the terminal event without
            // running (popJob left running_ untouched for this job).
            TransferEvent fe;
            fe.jobId = job->id;
            fe.kind = TransferEvent::Kind::Finished;
            fe.state = TransferState::Canceled;
            fe.localPath = job->req.localPath;
            fe.remotePath = job->req.remotePath;
            fe.direction = job->req.direction;
            emitEvent(fe);
            if (journal_ && journal_->enabled()) {
                std::lock_guard<std::mutex> lk(journalMu_);
                std::string jerr;
                journal_->remove(job->id, &jerr);
            }
            retireJob(std::move(job));
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
        // runJob always ends in a terminal state (Done/Failed/Canceled/
        // Skipped) with the Finished event emitted: move the job (and its
        // request credentials) out of the active set.
        retireJob(std::move(job));
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
        // A custom SFTP startup command (e.g. sudo ... sftp-server) only
        // applies to the SFTP subsystem/exec-started sftp-server channel,
        // NOT the raw SSH exec compression pipeline. To make elevated
        // writes actually work, force the SFTP Store path when a startup
        // command is configured -- otherwise an auto-Decide "Compress"
        // verdict would upload through an unprivileged exec channel.
        if (!req.ssh.sftpCommand.empty()) {
            mode = TransferMode::Store;
        } else if (req.autoDecide) {
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

    // Directory archive is its own pipeline (pack -> transfer -> extract);
    // it emits its own progress and terminal events and returns early.
    if (req.dirArchive) {
        runDirArchiveJob(job, ev);
        return;
    }

    // ---- same-name conflict detection (plain files only) ------------------
    // Overwrite (default) and Ftz1 containers skip this entirely (zero cost).
    // Skip/Smart stat the destination once and decide before transferring.
    if (req.conflict != ConflictPolicy::Overwrite &&
        mode != TransferMode::Ftz1) {
        RemoteStat src, dst;
        if (req.direction == TransferDirection::Upload) {
            src = localStat(req.localPath);
            dst = remoteStatFor(req, hkPolicy);
        } else {
            src = remoteStatFor(req, hkPolicy);
            dst = localStat(req.localPath);
        }
        bool skip = false;
        if (req.conflict == ConflictPolicy::Skip) {
            skip = dst.exists;
        } else {   // Smart
            skip = smartShouldSkip(src, dst);
        }
        if (skip) {
            TransferEvent se;
            se.jobId = job->id;
            se.kind = TransferEvent::Kind::Finished;
            se.state = TransferState::Skipped;
            se.mode = mode;
            se.localPath = req.localPath;
            se.remotePath = req.remotePath;
            se.direction = req.direction;
            se.error = "目标已存在，已跳过";
            job->state = TransferState::Skipped;
            emitEvent(se);
            if (journal_ && journal_->enabled()) {
                std::lock_guard<std::mutex> lk(journalMu_);
                std::string jerr;
                journal_->remove(job->id, &jerr);
            }
            return;
        }
    }

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
            fe.localPath = req.localPath;
            fe.remotePath = req.remotePath;
            fe.direction = req.direction;
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
    fe.localPath = req.localPath;
    fe.remotePath = req.remotePath;
    fe.direction = req.direction;
    job->state = finalState;
    emitEvent(fe);

    // Trim the journal: a terminal job is no longer resumable.
    if (journal_ && journal_->enabled()) {
        std::lock_guard<std::mutex> lk(journalMu_);
        std::string jerr;
        journal_->remove(job->id, &jerr);
    }
}

// ---- directory archive pipeline -------------------------------------------

namespace {

// Quotes a path for a remote shell command. See shell_quote.hpp: single-quote
// POSIX scheme; neutralizes $, `, \, ", ! -- the old double-quote scheme
// opened all of those to command substitution in filenames.
inline std::string shQuote(const std::string& s) { return shellQuote(s); }

// Runs one remote shell command over a fresh SSH exec channel and returns
// true on exit status 0. Used for the remote tar/zstd extract/pack steps of
// a directory-archive transfer.
bool runRemoteCommand(const SshEndpoint& ep, const std::string& command,
                      const std::vector<std::string>& trusted,
                      const HostKeyPolicy hkPolicy, std::string* errOut) {
    SshExec ssh(hkPolicy);
    std::string accepted;
    SshResult r = ssh.open(ep, command, trusted, &accepted);
    if (!r.ok()) {
        if (errOut) *errOut = "ssh open: " + r.message;
        return false;
    }
    // Send EOF so a command that reads stdin (e.g. `tar`) won't hang.
    ssh.sendEof();
    std::string stderrTail;
    ssh.setStderrSink([&](const void* p, std::size_t n) {
        stderrTail.append(static_cast<const char*>(p), n);
        if (stderrTail.size() > 512) stderrTail = stderrTail.substr(stderrTail.size() - 512);
    });
    while (ssh.pump(500)) {}
    const int code = ssh.exitCode();
    ssh.close();
    if (code != 0) {
        if (errOut) *errOut = "remote command exited " + std::to_string(code) +
                              (stderrTail.empty() ? "" : ": " + stderrTail);
        return false;
    }
    return true;
}

// A directory upload/download can only use `tar --zstd` when the remote
// actually has the zstd executable. Minimal AlmaLinux/CentOS installs
// usually do NOT, and GNU tar then dies with "zstd: Cannot exec". Probe
// once per host (cached for the process lifetime) and fall back to a plain
// .tar archive, which any POSIX tar can unpack.
bool remoteSupportsZstd(const SshEndpoint& ep,
                        const std::vector<std::string>& trusted,
                        HostKeyPolicy hkPolicy) {
    static std::mutex cacheMu;
    static std::map<std::string, bool> cache;
    const std::string key = ep.host + ":" + std::to_string(ep.port) + "@" + ep.user;
    {
        std::lock_guard<std::mutex> lk(cacheMu);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    const std::string probe =
        "command -v zstd >/dev/null 2>&1 && tar --help 2>&1 | grep -q -- '--zstd'";
    std::string probeErr;
    const bool ok = runRemoteCommand(ep, probe, trusted, hkPolicy, &probeErr);
    std::lock_guard<std::mutex> lk(cacheMu);
    cache[key] = ok;
    return ok;
}

}  // namespace

void TransferQueue::runDirArchiveJob(Job* job, const TransferEvent& started) {
    const TransferRequest& req = job->req;
    const HostKeyPolicy hkPolicy = req.trustedHostKeyHex.empty()
        ? HostKeyPolicy::AcceptNew : HostKeyPolicy::Strict;
    ProgressGate gate(progressMinIntervalMs_);
    TransferProgress tp;
    tp.compressionRatio = 1.0;

    auto emitProgress = [&](std::uint64_t moved, std::uint64_t total,
                            bool force = false) {
        if (!gate.pass(force)) return;
        tp.bytesMoved = moved;
        tp.bytesTotal = total;
        tp.bytesOnWire = moved;
        TransferEvent pe;
        pe.jobId = job->id;
        pe.kind = TransferEvent::Kind::Progress;
        pe.state = TransferState::Running;
        pe.mode = TransferMode::Ftz1;   // archive path reports as Ftz1-like
        pe.progress = tp;
        emitEvent(pe);
    };

    std::string error;
    TransferState finalState = TransferState::Done;

    // Pack/extract happens on the side that can run code. Upload: pack here.
    // Download: the remote (SFTP) packs via exec; we extract here.
    const bool isUpload = req.direction == TransferDirection::Upload;
    const bool sftp = req.protocol == BackendKind::Sftp;
    // `tar --zstd` also needs the zstd binary on the remote. Minimal Linux
    // images often omit it; in that case use a plain .tar that any POSIX tar
    // can unpack. The probe runs once per host and is cached.
    const bool useZstd = sftp && remoteSupportsZstd(req.ssh,
                                                    req.trustedHostKeyHex,
                                                    hkPolicy);
    const std::string archiveExt = useZstd ? ".tar.zst" : ".tar";

    if (isUpload) {
        // --- pack locally ------------------------------------------------
        const std::string tmp = req.localPath + ".dirarchive.tmp";
        emitProgress(0, 0);
        const DirArchiveStatus as = archiveDirectory(req.localPath, tmp,
                                                     req.zstdLevel, useZstd);
        if (as != DirArchiveStatus::Ok) {
            error = std::string("打包失败: ") + dirArchiveStatusName(as);
            finalState = TransferState::Failed;
        } else {
            const std::string remoteArchive = req.remotePath + archiveExt;
            std::uint64_t moved = 0;
            // One SftpSession spans the upload + remote extract so we can
            // mkdir/remove via SFTP (no `mkdir -p`/`rm` shell dependence);
            // the exec channel runs only `tar`.
            std::unique_ptr<SftpSession> sftpSess;
            if (sftp) {
                sftpSess = std::make_unique<SftpSession>(hkPolicy);
                std::string accepted;
                SftpResult r = sftpSess->open(req.ssh, req.trustedHostKeyHex,
                                              &accepted);
                if (!r.ok()) {
                    error = "sftp open: " + r.message;
                    finalState = TransferState::Failed;
                }
            } else {
                FtpSession f;
                FtpResult r = f.open(req.ftp);
                if (!r.ok()) {
                    error = "ftp open: " + r.message;
                    finalState = TransferState::Failed;
                } else {
                    f.setProgressCallback([&](std::uint64_t m, std::uint64_t t) {
                        emitProgress(m, t);
                    });
                    FtpResult tr = f.upload(tmp, remoteArchive, 0, &moved, &job->ctl);
                    if (tr.status == FtpStatus::Canceled) finalState = TransferState::Canceled;
                    else if (!tr.ok()) { error = tr.message; finalState = TransferState::Failed; }
                }
            }

            if (sftp && finalState == TransferState::Done) {
                // Make the target directory (and its parents) via SFTP, then
                // stream the archive up and unpack it on the remote. Plain
                // `tar -xf` is used when the host has no zstd executable.
                ensureRemoteDirSftp(*sftpSess, req.remotePath);
                sftpSess->mkdir(req.remotePath);
                sftpSess->setProgressCallback([&](std::uint64_t m, std::uint64_t t) {
                    emitProgress(m, t);
                });
                SftpResult tr = sftpSess->upload(tmp, remoteArchive, 0, &moved, &job->ctl);
                if (tr.status == SftpStatus::Canceled) {
                    finalState = TransferState::Canceled;
                } else if (!tr.ok()) {
                    error = tr.message;
                    finalState = TransferState::Failed;
                } else {
                    std::string cmdErr;
                    const std::string cmd = std::string(useZstd
                            ? "tar --zstd -xf " : "tar -xf ") +
                        shQuote(remoteArchive) + " -C " + shQuote(req.remotePath);
                    if (!runRemoteCommand(req.ssh, cmd, req.trustedHostKeyHex,
                                          hkPolicy, &cmdErr)) {
                        // The archive transferred but the remote could not
                        // unpack it. Surface as a real failure so the user
                        // notices instead of silently leaving a .tar.zst.
                        error = "远端自动解压失败：" + cmdErr +
                                "（压缩包已上传：" + remoteArchive + "）";
                        finalState = TransferState::Failed;
                    } else {
                        sftpSess->removeFile(remoteArchive);
                    }
                }
            } else if (!sftp && finalState == TransferState::Done) {
                // Defensive: FTP has no exec channel, so a dirArchive upload
                // can never auto-extract. The UI already avoids routing FTP
                // folders here; if one slips through, fail loudly rather than
                // leaving an un-extractable archive.
                error = "FTP 无法在远端自动解压，请用普通递归上传";
                finalState = TransferState::Failed;
            }
            std::remove(tmp.c_str());
        }
    } else {
        // --- download ----------------------------------------------------
        if (!sftp) {
            // FTP cannot pack on the remote; the caller should have routed
            // this to the per-file tree transfer instead.
            error = "FTP 无法在远端打包文件夹，请用普通递归下载";
            finalState = TransferState::Failed;
        } else {
            const std::string remoteArchive = req.remotePath + archiveExt;
            // 1) pack on the remote via exec.
            std::string cmdErr;
            const std::string cmd = std::string(useZstd
                    ? "tar --zstd -cf " : "tar -cf ") +
                shQuote(remoteArchive) + " -C " + shQuote(req.remotePath) + " .";
            if (!runRemoteCommand(req.ssh, cmd, req.trustedHostKeyHex,
                                  hkPolicy, &cmdErr)) {
                error = "远端打包失败: " + cmdErr;
                finalState = TransferState::Failed;
            } else {
                // 2) download the archive.
                const std::string tmp = req.localPath + ".dirarchive.tmp";
                SftpSession s(hkPolicy);
                std::string accepted;
                SftpResult r = s.open(req.ssh, req.trustedHostKeyHex, &accepted);
                std::uint64_t moved = 0;
                if (!r.ok()) {
                    error = "sftp open: " + r.message;
                    finalState = TransferState::Failed;
                } else {
                    s.setProgressCallback([&](std::uint64_t m, std::uint64_t t) {
                        emitProgress(m, t);
                    });
                    SftpResult tr = s.download(remoteArchive, tmp, 0, &moved, &job->ctl);
                    if (tr.status == SftpStatus::Canceled) {
                        finalState = TransferState::Canceled;
                        std::remove(tmp.c_str());
                    } else if (!tr.ok()) {
                        error = tr.message;
                        finalState = TransferState::Failed;
                        std::remove(tmp.c_str());
                    } else {
                        // 3) extract locally into localPath (the target
                        //    folder), then drop the remote archive.
                        createDirUtf8(req.localPath);
                        const DirArchiveStatus as = useZstd
                            ? extractArchive(tmp, req.localPath)
                            : extractArchiveRaw(tmp, req.localPath);
                        std::remove(tmp.c_str());
                        if (as != DirArchiveStatus::Ok) {
                            error = std::string("解压失败: ") + dirArchiveStatusName(as);
                            finalState = TransferState::Failed;
                        } else {
                            // Remove the remote archive now that we have it
                            // unpacked locally.
                            SftpResult rr = s.removeFile(remoteArchive);
                            (void)rr;
                        }
                    }
                }
            }
        }
    }

    TransferEvent fe;
    fe.jobId = job->id;
    fe.kind = TransferEvent::Kind::Finished;
    fe.state = finalState;
    fe.mode = TransferMode::Ftz1;
    fe.progress = tp;
    fe.error = error;
    fe.localPath = req.localPath;
    fe.remotePath = req.remotePath;
    fe.direction = req.direction;
    job->state = finalState;
    emitEvent(fe);

    if (journal_ && journal_->enabled()) {
        std::lock_guard<std::mutex> lk(journalMu_);
        std::string jerr;
        journal_->remove(job->id, &jerr);
    }
}

}  // namespace ftpflow
