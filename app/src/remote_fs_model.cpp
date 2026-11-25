// ftpflow -- remote filesystem table model implementation.
#include "remote_fs_model.hpp"

#include "entry_sort.hpp"
#include "file_icon.hpp"
#include "ui_util.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QFont>

#include <cstdio>
#include <functional>

#include "ftpflow/tree_walk.hpp"
#include "ftpflow/known_hosts.hpp"

namespace ftpflow {
namespace {

constexpr std::uint32_t kPageEntries = 500;

// Recursive remote removal: files first, then directories deepest-first,
// then the root. Returns "" on success, a human message on the first
// failure. `onItem` (optional) is invoked with each path right before it is
// deleted so the UI can show live progress.
using ItemProgress = std::function<void(const std::string&)>;

std::string removeSftpRecursive(SftpSession& s, const std::string& path,
                                bool isDir, const ItemProgress& onItem = {}) {
    if (!isDir) {
        if (onItem) onItem(path);
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
        if (onItem) onItem(p);
        const SftpResult r = s.removeFile(p);
        if (!r.ok()) return "删除失败 " + p + ": " + r.message;
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!it->isDir) continue;
        const std::string p = joinTreePath(path, it->relPath);
        if (onItem) onItem(p);
        const SftpResult r = s.removeDir(p);
        if (!r.ok()) return "删除目录失败 " + p + ": " + r.message;
    }
    if (onItem) onItem(path);
    const SftpResult r = s.removeDir(path);
    return r.ok() ? std::string()
                  : "删除目录失败 " + path + ": " + r.message;
}

std::string removeFtpRecursive(FtpSession& f, const std::string& path,
                               bool isDir, const ItemProgress& onItem = {}) {
    if (!isDir) {
        if (onItem) onItem(path);
        const FtpResult r = f.removeFile(path);
        return r.ok() ? std::string()
                      : "删除失败 " + path + ": " + r.message;
    }
    // Recursively empty the directory, then RMD it. Some servers (notably
    // IIS) hide entries from LIST/MLSD (system files, cache files, an IIS
    // metadata dir), so a single pass can leave the directory non-empty and
    // the final RMD answers 550. Re-list and retry a bounded number of times
    // so those stragglers get removed before we give up.
    constexpr int kMaxPasses = 5;
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        std::vector<TreeEntry> entries;
        std::string walkErr;
        if (!walkRemoteTree(f, path, entries, &walkErr))
            return "读取目录失败 " + path + ": " + walkErr;
        bool removedAnything = false;
        // Files first (deepest handled by the relPath ordering), then dirs.
        for (const TreeEntry& e : entries) {
            if (e.isDir) continue;
            const std::string p = joinTreePath(path, e.relPath);
            if (onItem) onItem(p);
            const FtpResult r = f.removeFile(p);
            if (!r.ok()) return "删除失败 " + p + ": " + r.message;
            removedAnything = true;
        }
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (!it->isDir) continue;
            const std::string p = joinTreePath(path, it->relPath);
            if (onItem) onItem(p);
            const FtpResult r = f.removeDir(p);
            if (!r.ok()) return "删除目录失败 " + p + ": " + r.message;
            removedAnything = true;
        }
        if (onItem) onItem(path);
        const FtpResult r = f.removeDir(path);
        if (r.ok()) return {};
        // RMD failed. If this pass removed entries (or the listing was empty
        // from the start), re-list and try again -- the directory may have
        // held hidden entries that only become visible/removable on a later
        // listing. Otherwise we have nothing left to retry.
        if (!removedAnything) {
            // The default LIST/MLSD hides dot-files and system files on some
            // servers (IIS / hosting panels): a directory shows as empty yet
            // RMD answers 550 ("not empty"). Re-list with `LIST -a` and drop
            // any hidden stragglers before retrying RMD.
            ListingPage all = f.listAll(path);
            if (!all.entries.empty()) {
                for (const Entry& e : all.entries) {
                    if (e.name == "." || e.name == "..") continue;
                    const std::string p = joinTreePath(path, e.name);
                    if (onItem) onItem(p);
                    if (e.type == EntryType::Directory) {
                        const FtpResult dr = f.removeDir(p);
                        if (!dr.ok())
                            return "删除目录失败 " + p + ": " + dr.message;
                    } else {
                        const FtpResult fr = f.removeFile(p);
                        if (!fr.ok())
                            return "删除失败 " + p + ": " + fr.message;
                    }
                }
                const FtpResult rr = f.removeDir(path);
                if (rr.ok()) return {};
                return "删除目录失败 " + path + ": " + rr.message +
                       "（含隐藏项，已尝试删除但仍有残留）";
            }
            // One more list to distinguish "empty but refused" from
            // "still non-empty but unlistable".
            std::vector<TreeEntry> again;
            std::string againErr;
            if (!walkRemoteTree(f, path, again, &againErr))
                return "删除目录失败 " + path + ": " + r.message +
                       "（重列失败：" + againErr + "）";
            if (again.empty())
                return "删除目录失败 " + path + ": " + r.message +
                       "（目录可能含服务器隐藏项或权限不足）";
            // Fall through: another pass will delete what we just listed.
        }
    }
    return "删除目录失败 " + path + ": 多次重试后目录仍非空";
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
    effEncoding_ = ci.ftp.encoding;   // reset to the configured default
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
    // A key listed as @revoked must never be re-accepted (trust() refuses
    // too; failing here keeps the message friendly).
    if (kh.isRevoked(host.toStdString(), port, blob)) {
        emit connectFailed("该主机密钥已被吊销（@revoked），拒绝信任");
        return;
    }
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

void RemoteFsModel::downloadFile(const std::string& remotePath,
                                 const std::string& localPath) {
    if (!connected_) return;
    Task t;
    t.kind = Task::Kind::DownloadFile;
    t.remotePath = remotePath;
    t.localPath = localPath;
    pushTask(std::move(t));
}

void RemoteFsModel::uploadFile(const std::string& localPath,
                               const std::string& remotePath) {
    if (!connected_) return;
    Task t;
    t.kind = Task::Kind::UploadFile;
    t.localPath = localPath;
    t.remotePath = remotePath;
    pushTask(std::move(t));
}

std::string RemoteFsModel::joinRemote(const std::string& dir,
                                      const std::string& name) const {
    if (dir.empty() || dir == ".") return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string RemoteFsModel::parentPath() const {
    if (currentPath_.empty() || currentPath_ == "/" || currentPath_ == ".")
        return {};
    const std::size_t slash = currentPath_.rfind('/');
    if (slash == std::string::npos) return "/";
    if (slash == 0) return "/";
    return currentPath_.substr(0, slash);
}

std::string RemoteFsModel::pathAt(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    const Entry& e = entryAt(idx.row());
    if (e.name == "..") return parentPath();
    return joinRemote(currentPath_, e.name);
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

bool RemoteFsModel::isParentRow(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return false;
    return entryAt(idx.row()).name == "..";
}

void RemoteFsModel::makeDir(const std::string& name) {
    if (!connected_ || name.empty()) return;
    Task t;
    t.kind = Task::Kind::MakeDir;
    t.dir = currentPath_;
    t.name = name;
    t.generation = navGen_;
    emit opStarted(QString("正在新建文件夹：%1…").arg(QString::fromStdString(name)));
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
        if (isParentRow(idx)) continue;   // never delete ".."
        OpTarget tgt;
        tgt.path = joinRemote(currentPath_, entryAt(idx.row()).name);
        tgt.isDir = entryAt(idx.row()).type == EntryType::Directory;
        t.targets.push_back(std::move(tgt));
    }
    if (!t.targets.empty()) {
        // Dim the affected rows immediately: a recursive delete can take a
        // while, and without a visual cue on the rows themselves the list
        // looks frozen. The rows vanish when the follow-up listing lands.
        for (const OpTarget& tgt : t.targets) pendingRemove_.insert(tgt.path);
        if (!entries_.empty()) {
            emit dataChanged(index(0, 0),
                             index(rowCount() - 1, ColCount - 1),
                             {Qt::ForegroundRole, Qt::FontRole});
        }
        emit opStarted(QString("正在删除 %1 个条目…").arg(int(t.targets.size())));
        pushTask(std::move(t));
    }
}

void RemoteFsModel::renameEntry(const QModelIndex& idx,
                                const std::string& newName) {
    if (!idx.isValid() || !connected_ || newName.empty()) return;
    if (isParentRow(idx)) return;   // never rename ".."
    Task t;
    t.kind = Task::Kind::Rename;
    t.dir = currentPath_;
    t.name = newName;
    t.generation = navGen_;
    OpTarget tgt;
    tgt.path = pathAt(idx);
    tgt.isDir = isDir(idx);
    t.targets.push_back(std::move(tgt));
    emit opStarted(QString("正在重命名：%1…")
                       .arg(QString::fromStdString(nameAt(idx))));
    pushTask(std::move(t));
}

bool RemoteFsModel::setData(const QModelIndex& idx, const QVariant& value,
                            int role) {
    if (!idx.isValid() || role != Qt::EditRole || idx.column() != ColName)
        return false;
    if (isParentRow(idx)) return false;
    const QString newName = value.toString().trimmed();
    if (!isValidEntryName(newName)) {
        // browseFailed only shows a status-bar message; opFailed would
        // unbalance the busy-op counter used by the progress indicator.
        emit browseFailed(QStringLiteral(
            "重命名失败：名称为空或包含非法字符 / \\ : * ? \" < > |"));
        return false;
    }
    const QString oldName = QString::fromStdString(nameAt(idx));
    if (newName == oldName) return false;
    if (!connected_) {
        emit browseFailed(QStringLiteral("重命名失败：未连接到服务器"));
        return false;
    }

    // Let the item delegate finish closing its editor before queuing the
    // worker task (the worker relists and resets the model when it returns).
    const int row = idx.row();
    QTimer::singleShot(0, this, [this, row, oldName, newName] {
        const QModelIndex i = index(row, ColName);
        if (!i.isValid() || isParentRow(i) || !connected_) return;
        if (QString::fromStdString(nameAt(i)) != oldName) return;
        renameEntry(i, newName.toStdString());
    });
    return true;
}

void RemoteFsModel::moveEntries(const std::vector<std::string>& sourcePaths,
                                const std::string& destDir) {
    if (!connected_ || destDir.empty() || sourcePaths.empty()) return;
    Task t;
    t.kind = Task::Kind::Move;
    t.dir = destDir;       // destination directory (remote path)
    t.srcDir = currentPath_;   // source dir to relist after the move
    t.generation = navGen_;
    for (const std::string& p : sourcePaths) {
        if (p.empty()) continue;
        OpTarget tgt;
        // Strip a trailing slash (directory marker) before moving.
        std::string path = p;
        if (path.size() > 1 && path.back() == '/') path.pop_back();
        tgt.path = path;
        tgt.isDir = false;   // rename() doesn't care; only used by Remove
        t.targets.push_back(std::move(tgt));
    }
    if (!t.targets.empty()) {
        emit opStarted(QString("正在移动 %1 个条目…").arg(int(t.targets.size())));
        pushTask(std::move(t));
    }
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
    if (role == Qt::DecorationRole && idx.column() == ColName)
        return fileIcon(e);
    // Rows with a delete in flight: dimmed + italic (see removeEntries).
    if (!pendingRemove_.empty()) {
        if (pendingRemove_.count(pathAt(idx))) {
            if (role == Qt::ForegroundRole)
                return QBrush(QColor(145, 145, 148));
            if (role == Qt::FontRole) {
                QFont f;
                f.setItalic(true);
                return f;
            }
        }
    }
    if (role == kDirFirstSortRole) {
        switch (idx.column()) {
            case ColName:
                return e.type == EntryType::Directory
                    ? QStringLiteral("0") + QString::fromStdString(e.name)
                    : QStringLiteral("1") + QString::fromStdString(e.name);
            case ColSize:
                return e.type == EntryType::Directory
                    ? QVariant::fromValue<qlonglong>(-1)
                    : QVariant::fromValue<qlonglong>(
                          static_cast<qlonglong>(e.size));
            case ColModified:
                // Folders sort by their own mtime too (not a hardcoded -1),
                // so time sorting is meaningful across files and directories.
                return QVariant::fromValue<qlonglong>(
                    static_cast<qlonglong>(e.mtimeUnix));
            default:
                return {};
        }
    }
    // Inline rename: the delegate reads the raw remote name through
    // EditRole (never the formatted columns, never the synthetic ".." row).
    if (role == Qt::EditRole) {
        if (idx.column() != ColName || isParentRow(idx)) return {};
        return QString::fromStdString(e.name);
    }
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

// ---- drag source -----------------------------------------------------------

// Custom MIME carrying remote absolute paths (one per line, "dir" suffix on
// directories). Distinct from the local MIME so each pane only accepts the
// other's drops.
const char* kRemotePathsMime = "application/x-ftpflow-remote-paths";

Qt::ItemFlags RemoteFsModel::flags(const QModelIndex& idx) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(idx);
    if (idx.isValid() && !isParentRow(idx)) {
        f |= Qt::ItemIsDragEnabled;
        // Only the name cell is editable (inline rename).
        if (idx.column() == ColName) f |= Qt::ItemIsEditable;
    }
    return f;
}

QStringList RemoteFsModel::mimeTypes() const {
    return {QLatin1String(kRemotePathsMime)};
}

QMimeData* RemoteFsModel::mimeData(const QModelIndexList& indexes) const {
    QStringList paths;
    QSet<int> seenRows;
    for (const QModelIndex& idx : indexes) {
        if (!idx.isValid() || idx.column() != ColName) continue;
        if (isParentRow(idx)) continue;
        if (seenRows.contains(idx.row())) continue;
        seenRows.insert(idx.row());
        std::string p = pathAt(idx);
        if (p.empty()) continue;
        if (isDir(idx)) p += "/";   // trailing slash marks a directory
        paths << QString::fromStdString(p);
    }
    if (paths.isEmpty()) return nullptr;
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(kRemotePathsMime),
                  paths.join('\n').toUtf8());
    return mime;
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

        // Text-file edit plumbing: transfer a single file through the live
        // session (download = remote -> local staging; upload = staging ->
        // remote). Runs on the worker thread so a large file never stalls
        // the UI, and reuses the already-open connection.
        if (t.kind == Task::Kind::DownloadFile ||
            t.kind == Task::Kind::UploadFile) {
            std::string error;
            if (t.kind == Task::Kind::DownloadFile) {
                if (sftp) {
                    const SftpResult r = sftp->download(t.remotePath,
                                                        t.localPath);
                    if (!r.ok()) error = "下载失败: " + r.message;
                } else if (ftp) {
                    const FtpResult r = ftp->download(t.remotePath, t.localPath);
                    if (!r.ok()) error = "下载失败: " + r.message;
                } else {
                    error = "会话未连接";
                }
            } else {
                if (sftp) {
                    const SftpResult r = sftp->upload(t.localPath,
                                                      t.remotePath);
                    if (!r.ok()) error = "上传失败: " + r.message;
                } else if (ftp) {
                    const FtpResult r = ftp->upload(t.localPath, t.remotePath);
                    if (!r.ok()) error = "上传失败: " + r.message;
                } else {
                    error = "会话未连接";
                }
            }
            const bool lost = !error.empty() &&
                ((sftp && sftp->connectionLost()) ||
                 (ftp && ftp->connectionLost()));
            const std::string remote = t.remotePath;
            const std::string local = t.localPath;
            const bool isDownload = (t.kind == Task::Kind::DownloadFile);
            QMetaObject::invokeMethod(
                this,
                [this, error, lost, remote, local, isDownload] {
                    if (error.empty()) {
                        if (isDownload)
                            emit editDownloaded(
                                QString::fromStdString(remote),
                                QString::fromStdString(local));
                        else
                            emit editUploaded(QString::fromStdString(remote));
                    } else if (lost) {
                        connected_ = false;
                        emit connectionLost(QString::fromStdString(error));
                    } else {
                        emit opFailed(QString::fromStdString(error));
                    }
                },
                Qt::QueuedConnection);
            if (lost) { sftp.reset(); ftp.reset(); }
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
                bool storeOk = true;
                if (verify) {
                    KnownHosts kh(t.conn.knownHostsPath);
                    trusted = kh.trustedHexFor(t.conn.ssh.host, t.conn.ssh.port,
                                               &storeOk);
                }
                if (verify && !storeOk) {
                    // Fail closed: an unreadable known_hosts store must abort
                    // the connection -- never fall through to trust-on-first-
                    // use, or a locked file silently bypasses pinning.
                    error = "SFTP 连接失败: 无法读取 known_hosts 主机密钥库";
                } else {
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
            t.kind == Task::Kind::Rename || t.kind == Task::Kind::Move) {
            std::string error;
            QString summary;   // success message handed to the UI
            // Throttled per-entry progress. A recursive delete can touch
            // thousands of entries; posting one queued signal per file would
            // flood the UI event loop and make the app feel slower than the
            // delete itself. ~8 updates a second is plenty.
            qint64 lastProgressMs = 0;
            auto reportProgress = [this, &lastProgressMs](const std::string& item) {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - lastProgressMs < 120) return;
                lastProgressMs = now;
                const QString msg =
                    QString("正在删除：%1").arg(QString::fromStdString(item));
                QMetaObject::invokeMethod(
                    this, [this, msg] { emit opProgress(msg); },
                    Qt::QueuedConnection);
            };
            auto postProgress = [this](const QString& msg) {
                QMetaObject::invokeMethod(
                    this, [this, msg] { emit opProgress(msg); },
                    Qt::QueuedConnection);
            };
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
                if (error.empty()) summary = "已新建文件夹";
            } else if (t.kind == Task::Kind::Remove) {
                const int total = static_cast<int>(t.targets.size());
                int done = 0;
                int failed = 0;
                std::string firstErr;
                for (const OpTarget& tgt : t.targets) {
                    ++done;
                    postProgress(QString("正在删除（%1/%2）：%3")
                                     .arg(done)
                                     .arg(total)
                                     .arg(QString::fromStdString(tgt.path)));
                    std::string one;
                    if (sftp) {
                        one = removeSftpRecursive(*sftp, tgt.path, tgt.isDir,
                                                  reportProgress);
                    } else if (ftp) {
                        one = removeFtpRecursive(*ftp, tgt.path, tgt.isDir,
                                                 reportProgress);
                    } else {
                        one = "会话未连接";
                    }
                    if (!one.empty()) {
                        ++failed;
                        if (firstErr.empty()) firstErr = one;
                        // A dead transport fails every remaining call: stop
                        // grinding through the list and report the loss.
                        if ((sftp && sftp->connectionLost()) ||
                            (ftp && ftp->connectionLost()))
                            break;
                    }
                }
                if (failed > 0) {
                    error = firstErr;
                    if (failed > 1) {
                        error += "（共 " + std::to_string(failed) +
                                 " 个条目删除失败）";
                    }
                } else {
                    summary = QString("已删除 %1 个条目").arg(total);
                }
            } else if (t.kind == Task::Kind::Rename) {   // single target
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
                if (error.empty()) summary = "已重命名";
            } else {   // Move: each source into t.dir, keeping its base name
                const int total = static_cast<int>(t.targets.size());
                int moved = 0;
                int failed = 0;
                std::string firstErr;
                for (const OpTarget& tgt : t.targets) {
                    const std::size_t slash = tgt.path.find_last_of('/');
                    const std::string base = slash == std::string::npos
                        ? tgt.path : tgt.path.substr(slash + 1);
                    if (base.empty()) continue;
                    const std::string to = joinRemote(t.dir, base);
                    if (to == tgt.path) continue;   // no-op self move
                    if (t.dir == tgt.path) continue;  // dropped onto itself
                    // Refuse to move a folder into itself / its own subtree:
                    // the server would answer 550 (or worse, loop).
                    if (t.dir.size() > tgt.path.size() &&
                        t.dir.compare(0, tgt.path.size(), tgt.path) == 0 &&
                        t.dir[tgt.path.size()] == '/') {
                        ++failed;
                        if (firstErr.empty()) {
                            firstErr = "不能把文件夹移动到它自己的子目录: " +
                                       tgt.path;
                        }
                        continue;
                    }
                    std::string one;
                    if (sftp) {
                        const SftpResult r = sftp->rename(tgt.path, to);
                        if (!r.ok()) one = "移动失败: " + r.message;
                    } else if (ftp) {
                        const FtpResult r = ftp->rename(tgt.path, to);
                        if (!r.ok()) one = "移动失败: " + r.message;
                    } else {
                        one = "会话未连接";
                    }
                    if (!one.empty()) {
                        ++failed;
                        if (firstErr.empty()) firstErr = one;
                        if ((sftp && sftp->connectionLost()) ||
                            (ftp && ftp->connectionLost()))
                            break;
                    } else {
                        ++moved;
                        postProgress(QString("已移动（%1/%2）：%3")
                                         .arg(moved)
                                         .arg(total)
                                         .arg(QString::fromStdString(base)));
                    }
                }
                if (failed > 0) {
                    error = firstErr;
                    if (failed > 1) {
                        error += "（共 " + std::to_string(failed) +
                                 " 个条目移动失败）";
                    }
                } else if (moved == 0) {
                    summary = "条目已在目标位置，无需移动";
                } else {
                    summary = QString("已移动 %1 个条目").arg(moved);
                }
            }
            // A dropped transport is a connection loss, not an op failure.
            const bool lost = !error.empty() &&
                ((sftp && sftp->connectionLost()) ||
                 (ftp && ftp->connectionLost()));
            QMetaObject::invokeMethod(
                this,
                [this, error, lost, summary] {
                    if (error.empty()) {
                        emit opFinished(summary);
                    } else {
                        // Never leave rows dimmed after a failed delete: the
                        // follow-up relist may itself fail (browse error), so
                        // clear the markers here.
                        if (!pendingRemove_.empty()) {
                            pendingRemove_.clear();
                            if (!entries_.empty()) {
                                emit dataChanged(
                                    index(0, 0),
                                    index(rowCount() - 1, ColCount - 1),
                                    {Qt::ForegroundRole, Qt::FontRole});
                            }
                        }
                        if (lost) {
                            connected_ = false;
                            emit connectionLost(QString::fromStdString(error));
                        } else {
                            emit opFailed(QString::fromStdString(error));
                        }
                    }
                },
                Qt::QueuedConnection);
            // Only a lost connection aborts the follow-up relist; a partial
            // failure still refreshes the view so the user sees what was
            // actually removed/moved.
            if (!error.empty() && lost) {
                sftp.reset();
                ftp.reset();
                continue;
            }
            // Success (or partial success): relist the directory the op
            // happened in. A move leaves the view on the source directory
            // (entries just left it).
            t.kind = Task::Kind::List;
            if (!t.srcDir.empty()) t.dir = t.srcDir;
            t.startAfter.clear();
            t.name.clear();
            t.targets.clear();
            t.srcDir.clear();
        }

        // List.
        std::vector<Entry> entries;
        bool hasMore = false;
        std::string error;
        bool lost = false;
        ListingPage page;
        if (sftp) {
            page = sftp->list(t.dir, t.startAfter, kPageEntries);
            error = sftp->lastError();      // "" on success
            entries = std::move(page.entries);
            hasMore = page.hasMore;
            if (!error.empty() && sftp->connectionLost()) lost = true;
        } else if (ftp) {
            page = ftp->list(t.dir);
            error = ftp->lastError();       // "" on success (also for empty)
            entries = std::move(page.entries);
            hasMore = false;                // whole listing in one page
            if (!error.empty() && ftp->connectionLost()) lost = true;
            // In "auto" mode list() may have learned the server's real
            // encoding (GBK) and locked the session to it. Propagate that
            // back to conn_ (on the UI thread) so a subsequent transfer
            // request built from connInfo() carries the right encoding --
            // otherwise a Chinese filename uploads/downloads with UTF-8
            // bytes against a GBK server and stalls (SIZE/CWD never match).
            if (error.empty()) {
                const std::string eff = ftp->effectiveEncoding();
                if (eff != conn_.ftp.encoding) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, eff] {
                            conn_.ftp.encoding = eff;
                            effEncoding_ = eff;
                        },
                        Qt::QueuedConnection);
                }
            }
        } else {
            error = "会话未连接";
        }

        // A dropped transport is a different signal from a failed browse:
        // close the dead session and let the UI decide to reconnect.
        if (lost) {
            sftp.reset();
            ftp.reset();
            QMetaObject::invokeMethod(
                this,
                [this, error] {
                    connected_ = false;
                    emit connectionLost(QString::fromStdString(error));
                },
                Qt::QueuedConnection);
            continue;
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
        // Cursor must reflect the backend's name order (used by the next
        // fetchMore), so capture it BEFORE we sort folders first.
        lastOfCurrentPage_ = entries.empty() ? std::string()
                                             : entries.back().name;
        // Non-root listing: prepend the synthetic ".." parent row.
        const std::string up = parentPath();
        if (!up.empty()) entries.insert(entries.begin(), parentEntry());
        sortDirFirst(entries);
        beginResetModel();
        entries_ = std::move(entries);
        hasMore_ = hasMore;
        // A fresh listing reflects the deletes that just finished, so the
        // "deleting" markers no longer apply.
        pendingRemove_.clear();
        endResetModel();
    } else {
        if (entries.empty()) {
            hasMore_ = false;
            return;
        }
        for (Entry& e : entries) {
            entries_.push_back(std::move(e));
            lastOfCurrentPage_ = entries_.back().name;
        }
        hasMore_ = hasMore;
        // Re-sort the now-larger listing (folders first) and republish.
        sortDirFirst(entries_);
        beginResetModel();
        endResetModel();
    }
}

}  // namespace ftpflow
