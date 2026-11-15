// ftpflow -- concurrent transfer queue implementation.
#include "ftpflow/transfer_queue.hpp"

#include <chrono>
#include <cstdio>
#include <sys/stat.h>

namespace ftpflow {

namespace {

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
std::string buildPushCommand(const TransferRequest& req) {
    const std::string tmpl = req.pushCommandTemplate.empty()
        ? sshDefaultPushCommandTemplate()
        : req.pushCommandTemplate;
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
    for (unsigned i = 0; i < workers; ++i) {
        workers_.emplace_back([this, i] { workerLoop(static_cast<int>(i)); });
    }
}

TransferQueue::~TransferQueue() { shutdown(); }

void TransferQueue::setEventCallback(EventCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    onEvent_ = std::move(cb);
}

std::uint64_t TransferQueue::enqueue(TransferRequest req) {
    auto job = std::make_shared<Job>();
    job->req = std::move(req);
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_.load()) return 0;
        id = nextId_++;
        job->id = id;
        all_.push_back(job);
        queue_.push_back(std::move(job));
    }
    cvPop_.notify_one();
    return id;
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

void TransferQueue::emit(const TransferEvent& ev) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cb = onEvent_;
    }
    if (cb) cb(ev);
}

std::shared_ptr<TransferQueue::Job> TransferQueue::popJob() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        cvPop_.wait(lk, [this] { return stopping_.load() || !queue_.empty(); });
        if (stopping_.load()) return nullptr;
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

void TransferQueue::workerLoop(int /*workerIndex*/) {
    for (;;) {
        std::shared_ptr<Job> job = popJob();
        if (!job) return;

        if (job->state == TransferState::Canceled) {
            // Canceled while queued: emit the terminal event without
            // running (popJob left running_ untouched for this job).
            TransferEvent fe;
            fe.jobId = job->id;
            fe.kind = TransferEvent::Kind::Finished;
            fe.state = TransferState::Canceled;
            emit(fe);
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

    // --- pick the pipeline --------------------------------------------------
    TransferMode mode = TransferMode::Store;
    Decision decision;
    if (req.protocol == BackendKind::Sftp &&
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
    emit(ev);

    ProgressGate gate(progressMinIntervalMs_);
    TransferProgress tp;
    tp.compressionRatio = 1.0;

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
        emit(pe);
    };

    std::string error;
    TransferState finalState = TransferState::Done;

    if (req.protocol == BackendKind::Ftp) {
        FtpSession ftp;
        FtpResult r = ftp.open(req.ftp);
        if (!r.ok()) {
            error = std::string("ftp open: ") + r.message;
            finalState = TransferState::Failed;
        } else {
            ftp.setProgressCallback([&](std::uint64_t moved, std::uint64_t total) {
                tp.bytesMoved = moved;
                tp.bytesTotal = total;
                tp.bytesOnWire = moved;   // raw pipeline
                tp.compressionRatio = 1.0;
                emitProgress();
            });
            FtpResult tr = req.direction == TransferDirection::Upload
                ? ftp.upload(req.localPath, req.remotePath, 0, &tp.bytesMoved, &job->ctl)
                : ftp.download(req.remotePath, req.localPath, 0, &tp.bytesMoved, &job->ctl);
            if (tr.status == FtpStatus::Canceled) {
                finalState = TransferState::Canceled;
            } else if (!tr.ok()) {
                error = tr.message;
                finalState = TransferState::Failed;
            }
        }
    } else if (mode == TransferMode::Store) {
        // SFTP: reuse this worker's session when the endpoint matches.
        // (workerLoop owns the cache; for simplicity we re-open per job here
        // -- session caching arrives with the GUI connection manager.)
        SftpSession sftp(HostKeyPolicy::AcceptNew);
        std::string acceptedKey;
        SftpResult r = sftp.open(req.ssh, {}, &acceptedKey);
        if (!r.ok()) {
            error = std::string("sftp open: ") + r.message;
            finalState = TransferState::Failed;
        } else {
            sftp.setProgressCallback([&](std::uint64_t moved, std::uint64_t total) {
                tp.bytesMoved = moved;
                tp.bytesTotal = total;
                tp.bytesOnWire = moved;
                tp.compressionRatio = 1.0;
                emitProgress();
            });
            SftpResult tr = req.direction == TransferDirection::Upload
                ? sftp.upload(req.localPath, req.remotePath, 0, &tp.bytesMoved, &job->ctl)
                : sftp.download(req.remotePath, req.localPath, 0, &tp.bytesMoved, &job->ctl);
            if (tr.status == SftpStatus::Canceled) {
                finalState = TransferState::Canceled;
            } else if (!tr.ok()) {
                error = tr.message;
                finalState = TransferState::Failed;
            }
        }
    } else {
        // Compress: one exec channel per job, streaming zstd.
        const std::uint64_t localSize = localFileSize(req.localPath);
        SshExec ssh(HostKeyPolicy::AcceptNew);
        std::string acceptedKey;
        SshResult r = ssh.open(req.ssh, buildPushCommand(req), {}, &acceptedKey);
        if (!r.ok()) {
            error = std::string("ssh open: ") + r.message;
            finalState = TransferState::Failed;
        } else {
            SshPushOptions opts;
            opts.zstdLevel = req.zstdLevel;
            opts.control = &job->ctl;
            SshPushStats st;
            opts.onProgress = [&](const SshPushStats& s) {
                tp.bytesMoved = s.localBytes;
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
                tp.bytesMoved = st.localBytes;
                tp.bytesTotal = localSize;
                tp.bytesOnWire = st.compressedBytes;
                tp.compressionRatio = st.ratio();
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
    emit(fe);
}

}  // namespace ftpflow
