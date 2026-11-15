// ftpflow -- remote filesystem table model implementation.
#include "remote_fs_model.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>

#include <cstdio>

#include "ftpflow/tree_walk.hpp"
#include "ftpflow/known_hosts.hpp"

namespace ftpflow {
namespace {

QString humanSize(std::uint64_t n) {
    if (n < 1024) return QString::number(static_cast<qulonglong>(n)) + " B";
    const double kb = n / 1024.0;
    if (kb < 1024.0) return QString::number(kb, 'f', 1) + " KiB";
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return QString::number(mb, 'f', 1) + " MiB";
    return QString::number(mb / 1024.0, 'f', 2) + " GiB";
}

constexpr std::uint32_t kPageEntries = 500;

// Recursive remote removal: files first, then directories deepest-first,
// then the root. Returns "" on success, a human message on the first
// failure.
std::string removeSftpRecursive(SftpSession& s, const std::string& path,
                                bool isDir) {
    if (!isDir) {
        const SftpResult r = s.removeFile(path);
        return r.ok() ? std::string()
                      : "删除失败 " + path + ": " + r.message;
    }
    std::vector<TreeEntry> entries;
    std::string walkErr;
    if (!walkRemoteTree(s, path, entries, &walkErr))
        return "读取目录失败 " + path + ": " + walkErr;
    for (const TreeEntry& e : entries) {
        if (e.isDir) continue;
        const std::string p = joinTreePath(path, e.relPath);
        const SftpResult r = s.removeFile(p);
        if (!r.ok()) return "删除失败 " + p + ": " + r.message;
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!it->isDir) continue;
        const std::string p = joinTreePath(path, it->relPath);
        const SftpResult r = s.removeDir(p);
        if (!r.ok()) return "删除目录失败 " + p + ": " + r.message;
    }
    const SftpResult r = s.removeDir(path);
    return r.ok() ? std::string()
                  : "删除目录失败 " + path + ": " + r.message;
}

std::string removeFtpRecursive(FtpSession& f, const std::string& path,
                               bool isDir) {
    if (!isDir) {
        const FtpResult r = f.removeFile(path);
        return r.ok() ? std::string()
                      : "删除失败 " + path + ": " + r.message;
    }
    std::vector<TreeEntry> entries;
    std::string walkErr;
    if (!walkRemoteTree(f, path, entries, &walkErr))
        return "读取目录失败 " + path + ": " + walkErr;
    for (const TreeEntry& e : entries) {
        if (e.isDir) continue;
        const std::string p = joinTreePath(path, e.relPath);
        const FtpResult r = f.removeFile(p);
        if (!r.ok()) return "删除失败 " + p + ": " + r.message;
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!it->isDir) continue;
        const std::string p = joinTreePath(path, it->relPath);
        const FtpResult r = f.removeDir(p);
        if (!r.ok()) return "删除目录失败 " + p + ": " + r.message;
    }
    const FtpResult r = f.removeDir(path);
    return r.ok() ? std::string()
                  : "删除目录失败 " + path + ": " + r.message;
}

}  // namespace

RemoteFsModel::RemoteFsModel(QObject* parent)
    : QAbstractTableModel(parent) {
    worker_ = std::thread([this] { workerLoop(); });
}

RemoteFsModel::~RemoteFsModel() {
    if (worker_.joinable()) {
        pushTask(Task{Task::Kind::Shutdown});
        worker_.join();
    }
}

void RemoteFsModel::pushTask(Task t) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tasks_.push_back(std::move(t));
    }
    cv_.notify_one();
}

// ---- public API (UI thread) ----------------------------------------------

void RemoteFsModel::connectTo(const ConnInfo& ci) {
    conn_ = ci;
    currentPath_ = ci.startDir.empty() ? "/" : ci.startDir;
    entries_.clear();
    hasMore_ = false;
    connected_ = false;
    ++navGen_;

    Task t;
    t.kind = Task::Kind::Connect;
    t.conn = ci;
    t.generation = navGen_;
    pushTask(std::move(t));
}

void RemoteFsModel::disconnect() {
    connected_ = false;
    entries_.clear();
    ++navGen_;
    pushTask(Task{Task::Kind::Disconnect});
    beginResetModel();
    endResetModel();
    emit disconnected();
}

void RemoteFsModel::trustHostKey(const QString& host, quint16 port,
                                 const QString& keyHex) {
    if (conn_.knownHostsPath.empty()) return;
    KnownHosts kh(conn_.knownHostsPath);
    std::string err;
    const std::string blob =
        KnownHosts::hexDecode(keyHex.toStdString());
    if (!kh.trust(host.toStdString(), port, "ssh-rsa", blob, &err)) {
        emit connectFailed(QString("无法保存主机密钥: %1")
                               .arg(QString::fromStdString(err)));
        return;
    }
    // Retry the connection with the now-trusted key.
    connectTo(conn_);
}

void RemoteFsModel::enterDir(const std::string& dir) {
    currentPath_ = dir;
    ++navGen_;

    Task t;
    t.kind = Task::Kind::List;
    t.dir = dir;
    t.generation = navGen_;
    pushTask(std::move(t));
    // Rows update when the listing lands; clear immediately so the old
    // directory does not masquerade as the new one.
    beginResetModel();
    entries_.clear();
    lastOfCurrentPage_.clear();
    hasMore_ = false;
    endResetModel();
}

void RemoteFsModel::goUp() {
    if (currentPath_.empty() || currentPath_ == "/" ||
        currentPath_ == ".") return;
    const std::size_t slash = currentPath_.rfind('/');
    if (slash == std::string::npos || slash == 0) {
        enterDir("/");
    } else {
        enterDir(currentPath_.substr(0, slash));
    }
}

void RemoteFsModel::refresh() {
    if (!connected_) return;
    enterDir(currentPath_);
}

std::string RemoteFsModel::joinRemote(const std::string& dir,
                                      const std::string& name) const {
    if (dir.empty() || dir == ".") return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string RemoteFsModel::pathAt(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    return joinRemote(currentPath_, entryAt(idx.row()).name);
}

std::string RemoteFsModel::nameAt(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    return entryAt(idx.row()).name;
}

bool RemoteFsModel::isDir(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return false;
    return entryAt(idx.row()).type == EntryType::Directory;
}

void RemoteFsModel::makeDir(const std::string& name) {
    if (!connected_ || name.empty()) return;
    Task t;
    t.kind = Task::Kind::MakeDir;
    t.dir = currentPath_;
    t.name = name;
    t.generation = navGen_;
    pushTask(std::move(t));
}

void RemoteFsModel::removeEntries(const QModelIndexList& rows) {
    if (!connected_) return;
    Task t;
    t.kind = Task::Kind::Remove;
    t.dir = currentPath_;
    t.generation = navGen_;
    for (const QModelIndex& idx : rows) {
        if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
            continue;
        OpTarget tgt;
        tgt.path = joinRemote(currentPath_, entryAt(idx.row()).name);
        tgt.isDir = entryAt(idx.row()).type == EntryType::Directory;
        t.targets.push_back(std::move(tgt));
    }
    if (!t.targets.empty()) pushTask(std::move(t));
}

void RemoteFsModel::renameEntry(const QModelIndex& idx,
                                const std::string& newName) {
    if (!idx.isValid() || !connected_ || newName.empty()) return;
    Task t;
    t.kind = Task::Kind::Rename;
    t.dir = currentPath_;
    t.name = newName;
    t.generation = navGen_;
    OpTarget tgt;
    tgt.path = pathAt(idx);
    tgt.isDir = isDir(idx);
    t.targets.push_back(std::move(tgt));
    pushTask(std::move(t));
}

// ---- model plumbing -------------------------------------------------------

int RemoteFsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(entries_.size());
}

int RemoteFsModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColCount;
}

QVariant RemoteFsModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    const Entry& e = entryAt(idx.row());
    if (role == Qt::TextAlignmentRole &&
        (idx.column() == ColSize || idx.column() == ColModified))
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole) return {};

    switch (idx.column()) {
        case ColName:
            return QString::fromStdString(e.name);
        case ColSize:
            return e.type == EntryType::Directory
                ? QString("<dir>") : humanSize(e.size);
        case ColModified:
            return e.mtimeUnix == 0
                ? QString("-")
                : QDateTime::fromSecsSinceEpoch(e.mtimeUnix)
                      .toString("yyyy-MM-dd HH:mm");
    }
    return {};
}

QVariant RemoteFsModel::headerData(int section, Qt::Orientation o,
                                   int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case ColName: return "名称";
        case ColSize: return "大小";
        case ColModified: return "修改时间";
    }
    return {};
}

bool RemoteFsModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    return hasMore_ && connected_;
}

void RemoteFsModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || !canFetchMore(parent)) return;
    Task t;
    t.kind = Task::Kind::List;
    t.dir = currentPath_;
    t.startAfter = lastOfCurrentPage_;   // SFTP cursor; FTP ignores it
    t.generation = navGen_;
    pushTask(std::move(t));
}

// ---- worker thread --------------------------------------------------------

void RemoteFsModel::workerLoop() {
    std::unique_ptr<FtpSession> ftp;
    std::unique_ptr<SftpSession> sftp;

    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !tasks_.empty(); });
            t = std::move(tasks_.front());
            tasks_.pop_front();
        }
        if (t.kind == Task::Kind::Shutdown) return;

        if (t.kind == Task::Kind::Disconnect) {
            ftp.reset();
            sftp.reset();
            continue;
        }

        if (t.kind == Task::Kind::Connect) {
            ftp.reset();
            sftp.reset();
            std::string error;
            if (t.conn.isSftp) {
                // Strict host-key verification against known_hosts when a
                // store path is configured; otherwise AcceptNew (legacy).
                const bool verify = !t.conn.knownHostsPath.empty();
                std::vector<std::string> trusted;
                if (verify) {
                    KnownHosts kh(t.conn.knownHostsPath);
                    trusted = kh.trustedHexFor(t.conn.ssh.host, t.conn.ssh.port);
                }
                auto s = std::make_unique<SftpSession>(
                    verify ? HostKeyPolicy::Strict : HostKeyPolicy::AcceptNew);
                std::string acceptedKey;
                SftpResult r = s->open(t.conn.ssh, trusted, &acceptedKey);
                if (verify && r.status == SftpStatus::HostKeyRejected &&
                    !r.observedHostKeyHex.empty()) {
                    // Ask the user (UI thread) to confirm the fingerprint.
                    QMetaObject::invokeMethod(
                        this,
                        [this, host = t.conn.ssh.host, port = t.conn.ssh.port,
                         keyHex = r.observedHostKeyHex] {
                            emit hostKeyUnknown(
                                QString::fromStdString(
                                    KnownHosts::fingerprintOfHex(keyHex)),
                                QString::fromStdString(host), port,
                                QString::fromStdString(keyHex));
                        },
                        Qt::QueuedConnection);
                    error = "SFTP 连接失败: 主机密钥未受信任，请在弹窗中确认";
                } else if (!r.ok()) {
                    error = "SFTP 连接失败: " + r.message;
                } else {
                    sftp = std::move(s);
                }
            } else {
                auto f = std::make_unique<FtpSession>();
                FtpResult r = f->open(t.conn.ftp);
                if (!r.ok()) {
                    error = "FTP 连接失败: " + r.message;
                } else {
                    ftp = std::move(f);
                }
            }
            // Post the connect result, then (on success) the first listing.
            QMetaObject::invokeMethod(
                this,
                [this, error] {
                    if (error.empty()) {
                        connected_ = true;
                        emit connected();
                    } else {
                        connected_ = false;
                        emit connectFailed(QString::fromStdString(error));
                    }
                },
                Qt::QueuedConnection);
            if (!error.empty()) continue;
            // Fall through to listing startDir.
            t.kind = Task::Kind::List;
            t.dir = t.conn.startDir;
        }

        if (t.kind == Task::Kind::MakeDir || t.kind == Task::Kind::Remove ||
            t.kind == Task::Kind::Rename) {
            std::string error;
            if (t.kind == Task::Kind::MakeDir) {
                const std::string path = joinRemote(t.dir, t.name);
                if (sftp) {
                    const SftpResult r = sftp->mkdir(path, 0755);
                    if (!r.ok()) error = "新建目录失败: " + r.message;
                } else if (ftp) {
                    const FtpResult r = ftp->mkdir(path);
                    if (!r.ok()) error = "新建目录失败: " + r.message;
                } else {
                    error = "会话未连接";
                }
            } else if (t.kind == Task::Kind::Remove) {
                for (const OpTarget& tgt : t.targets) {
                    std::string one;
                    if (sftp) {
                        one = removeSftpRecursive(*sftp, tgt.path, tgt.isDir);
                    } else if (ftp) {
                        one = removeFtpRecursive(*ftp, tgt.path, tgt.isDir);
                    } else {
                        one = "会话未连接";
                    }
                    if (!one.empty()) {
                        error = one;
                        break;
                    }
                }
            } else {   // Rename (single target; new name under t.dir)
                const std::string from = t.targets.front().path;
                const std::string to = joinRemote(t.dir, t.name);
                if (sftp) {
                    const SftpResult r = sftp->rename(from, to);
                    if (!r.ok()) error = "重命名失败: " + r.message;
                } else if (ftp) {
                    const FtpResult r = ftp->rename(from, to);
                    if (!r.ok()) error = "重命名失败: " + r.message;
                } else {
                    error = "会话未连接";
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, error] {
                    if (error.empty()) emit opFinished({});
                    else emit opFailed(QString::fromStdString(error));
                },
                Qt::QueuedConnection);
            if (!error.empty()) continue;
            // Success: relist the directory the op happened in.
            t.kind = Task::Kind::List;
            t.startAfter.clear();
            t.name.clear();
            t.targets.clear();
        }

        // List.
        std::vector<Entry> entries;
        bool hasMore = false;
        std::string error;
        ListingPage page;
        if (sftp) {
            page = sftp->list(t.dir, t.startAfter, kPageEntries);
            error = sftp->lastError();      // "" on success
            entries = std::move(page.entries);
            hasMore = page.hasMore;
        } else if (ftp) {
            page = ftp->list(t.dir);
            error = ftp->lastError();       // "" on success (also for empty)
            entries = std::move(page.entries);
            hasMore = false;                // whole listing in one page
        } else {
            error = "会话未连接";
        }

        QMetaObject::invokeMethod(
            this,
            [this, gen = t.generation, dir = t.dir,
             entries = std::move(entries), hasMore, error,
             append = !t.startAfter.empty()]() mutable {
                postListing(gen, dir, std::move(entries), hasMore, append,
                            error);
            },
            Qt::QueuedConnection);
    }
}

void RemoteFsModel::postListing(std::uint64_t generation,
                                const std::string& dir,
                                std::vector<Entry> entries, bool hasMore,
                                bool append, const std::string& error) {
    // Stale result (the user navigated again meanwhile)?
    if (generation != navGen_) return;

    if (!error.empty()) {
        emit browseFailed(QString::fromStdString(error));
        return;
    }

    // append=true only for fetchMore continuation (startAfter cursor set);
    // refreshes replace the listing wholesale.
    const bool appending = append && !lastOfCurrentPage_.empty();
    if (!appending) {
        beginResetModel();
        entries_ = std::move(entries);
        lastOfCurrentPage_ = entries_.empty() ? std::string()
                                              : entries_.back().name;
        hasMore_ = hasMore;
        endResetModel();
    } else {
        if (entries.empty()) {
            hasMore_ = false;
            return;
        }
        const int first = static_cast<int>(entries_.size());
        beginInsertRows({}, first, first + static_cast<int>(entries.size()) - 1);
        for (Entry& e : entries) {
            entries_.push_back(std::move(e));
            lastOfCurrentPage_ = entries_.back().name;
        }
        hasMore_ = hasMore;
        endInsertRows();
    }
}

}  // namespace ftpflow
