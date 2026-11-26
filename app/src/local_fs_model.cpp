// ftpflow -- local filesystem table model implementation.
#include "local_fs_model.hpp"

#include "entry_sort.hpp"
#include "file_icon.hpp"
#include "ui_util.hpp"

#include <QtConcurrent/QtConcurrentMap>
#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <cstdio>
#include <cstring>

namespace ftpflow {
namespace {

// Verdicts are probed for at most this many files per page: the funnel
// samples 3x64 KiB per file, so a hard cap keeps even huge listings snappy.
constexpr int kMaxVerdictsPerPage = 400;

// Thread-safe probe: builds a FileProbe (head + sampled) and runs the
// funnel. Pure I/O + CPU, no Qt objects touched.
Decision probeOne(const LocalFsModel::VerdictJob& j) {
    FileProbe p;
    std::FILE* f = std::fopen(j.fullPath.c_str(), "rb");
    if (!f) return Decision{};
    p.fileSize = j.size;
    const char* slash = std::strrchr(j.fullPath.c_str(), '/');
    const char* slash2 = std::strrchr(j.fullPath.c_str(), '\\');
    if (slash || slash2) {
        const char* last = slash && slash2 ? (slash > slash2 ? slash : slash2)
                                            : (slash ? slash : slash2);
        p.fileName = last + 1;
    } else {
        p.fileName = j.fullPath;
    }
    std::uint8_t head[32] = {};
    std::size_t got = std::fread(head, 1, sizeof(head), f);
    p.head.assign(head, head + got);

    constexpr __int64 kWin = 64 * 1024;
    _fseeki64(f, 0, SEEK_END);
    const __int64 len = _ftelli64(f);
    const __int64 mid = len / 2;
    const __int64 tail = len > kWin ? len - kWin : 0;
    for (__int64 off : {(__int64)0, mid, tail}) {
        if (off < 0 || off >= len) continue;
        _fseeki64(f, off, SEEK_SET);
        std::vector<std::uint8_t> buf(kWin);
        std::size_t g = std::fread(buf.data(), 1, kWin, f);
        p.sample.insert(p.sample.end(), buf.data(), buf.data() + g);
    }
    std::fclose(f);

    CostModel cm;  // default: 20 MiB/s unknown-link assumption
    return decideFullFunnel(p, cm);
}

}  // namespace

LocalFsModel::LocalFsModel(QObject* parent)
    : QAbstractTableModel(parent) {
    connect(&verdictWatcher_, &QFutureWatcherBase::finished, this,
            &LocalFsModel::onVerdictsFinished);
}

void LocalFsModel::onVerdictsFinished() {
    for (int i = 0; i < static_cast<int>(batchJobs_.size()); ++i) {
        const int row = batchJobs_[static_cast<std::size_t>(i)].row;
        if (row < 0 || row >= static_cast<int>(entries_.size())) continue;
        verdicts_[row] = verdictWatcher_.resultAt(i);
    }
    if (!entries_.empty()) {
        const QModelIndex a = index(0, ColVerdict);
        const QModelIndex b = index(static_cast<int>(entries_.size()) - 1,
                                    ColVerdict);
        emit dataChanged(a, b);
    }
    emit verdictsReady();

    if (batchPending_) {
        batchPending_ = false;
        startVerdictBatch(pendingFrom_, pendingCount_);
    }
}

bool LocalFsModel::openRoot(const std::string& root, std::string* error) {
    std::string err;
    if (!backend_.open(root, &err)) {
        if (error) *error = err;
        return false;
    }
    root_ = root;
    currentDir_.clear();
    listingValid_ = true;
    cache_.clear();
    // Watch the root (and later each browsed directory) so external
    // changes invalidate the cache and refresh the view. The callback
    // runs on the watcher thread: marshal to the UI thread.
    watcher_ = std::make_unique<FsWatcher>(
        [this](const std::string& changed) {
            cache_.invalidate(changed);
            QMetaObject::invokeMethod(
                this,
                [this, changed] { onDirChangedExternal(changed); },
                Qt::QueuedConnection);
        });
    watcher_->watch(currentFullPath());
    loadFirstPage();
    return true;
}

std::string LocalFsModel::currentFullPath() const {
    std::string full = root_;
    while (!full.empty() && (full.back() == '/' || full.back() == '\\'))
        full.pop_back();
    if (!currentDir_.empty()) {
        full += "/";
        full += currentDir_;
    }
    return full;
}

std::string LocalFsModel::parentFullPath() const {
    std::string full = currentFullPath();
    // Strip trailing separators (a drive root "C:\" collapses to "C:").
    while (full.size() > 1 &&
           (full.back() == '/' || full.back() == '\\'))
        full.pop_back();
    const std::size_t slash = full.find_last_of("/\\");
    // "C:" (drive root) has no separator -> no parent.
    if (slash == std::string::npos) return {};
    // Parent up to and including the separator (e.g. "C:\foo" -> "C:\").
    return full.substr(0, slash + 1);
}

void LocalFsModel::onDirChangedExternal(const std::string& dir) {
    // Only reload when the change affects the directory on screen. The
    // cache is already invalidated (watcher thread did that); this just
    // refreshes rows.
    if (dir != currentFullPath()) return;
    if (!listingValid_) return;
    loadFirstPage();
}

bool LocalFsModel::enterDir(const std::string& relative) {
    if (!listingValid_) return false;
    currentDir_ = relative;
    if (watcher_) watcher_->watch(currentFullPath());
    loadFirstPage();
    return true;
}

bool LocalFsModel::goUp() {
    if (!currentDir_.empty()) {
        const std::size_t slash = currentDir_.rfind('/');
        currentDir_ = slash == std::string::npos
            ? std::string() : currentDir_.substr(0, slash);
        if (watcher_) watcher_->watch(currentFullPath());
        loadFirstPage();
        return true;
    }
    // Already at root_: move up only when root_ itself is not a drive root.
    const std::string parent = parentFullPath();
    if (parent.empty()) return false;
    std::string err;
    if (!openRoot(parent, &err)) return false;
    return true;
}

void LocalFsModel::loadFirstPage() {
    verdictWatcher_.cancel();
    batchJobs_.clear();
    batchPending_ = false;

    // Load the whole directory at once (the backend enumerates + sorts the
    // full listing regardless; paging would fight the "folders first" sort),
    // prepend the synthetic ".." row when not at the root, and sort folders
    // before files.
    std::vector<Entry> all;
    if (!parentFullPath().empty()) all.push_back(parentEntry());
    ListingPage page =
        cache_.list(backend_, currentFullPath(), {}, 0xFFFFFFFFu);
    for (Entry& e : page.entries) all.push_back(std::move(e));
    sortDirFirst(all);
    hasMore_ = false;
    lastOfCurrentPage_.clear();

    beginResetModel();
    entries_ = std::move(all);
    verdicts_.clear();
    endResetModel();

    startVerdictBatch(0, static_cast<int>(entries_.size()));
}

void LocalFsModel::appendPage(const ListingPage& page) {
    hasMore_ = page.hasMore;
    if (page.entries.empty()) return;
    for (const Entry& e : page.entries) {
        entries_.push_back(e);
        lastOfCurrentPage_ = e.name;
    }
    sortDirFirst(entries_);
    // Row order changed; re-publish everything and recompute verdicts for
    // the new ordering.
    beginResetModel();
    verdicts_.clear();
    endResetModel();

    startVerdictBatch(0, static_cast<int>(entries_.size()));
}

void LocalFsModel::startVerdictBatch(int from, int count) {
    if (verdictWatcher_.isRunning()) {
        // One batch at a time; queue the request, start when finished.
        batchPending_ = true;
        pendingFrom_ = from;
        pendingCount_ = count;
        return;
    }
    const int n = qMin(count, kMaxVerdictsPerPage);
    batchJobs_.clear();
    for (int i = 0; i < n; ++i) {
        const Entry& e = entries_[static_cast<std::size_t>(from + i)];
        if (e.type == EntryType::Directory) continue;
        batchJobs_.push_back({from + i, pathAt(index(from + i, 0)).toStdString(),
                              e.size});
    }
    if (batchJobs_.empty()) {
        if (batchPending_) {  // nothing to run; drain the queue
            batchPending_ = false;
        }
        return;
    }
    verdictWatcher_.setFuture(QtConcurrent::mapped(batchJobs_, probeOne));
}

int LocalFsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(entries_.size());
}

int LocalFsModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColCount;
}

QVariant LocalFsModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    const Entry& e = entryAt(idx.row());
    if (role == Qt::TextAlignmentRole &&
        (idx.column() == ColSize || idx.column() == ColModified))
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::DecorationRole && idx.column() == ColName)
        return fileIcon(e);
    // Fixed group key for EntrySortProxyModel: ".." pinned first, folders
    // above files, regardless of the active column or sort direction.
    if (role == kEntryKindRole)
        return isParentRow(idx) ? 0
             : (e.type == EntryType::Directory ? 1 : 2);
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
            case ColType:
                return fileTypeText(e);
            case ColModified:
                // Folders sort by their own mtime too (not a hardcoded -1),
                // so time sorting is meaningful across files and directories.
                // Entries with an unknown time (0) sink to the very end of a
                // descending sort and lead an ascending one.
                return QVariant::fromValue<qlonglong>(
                    static_cast<qlonglong>(e.mtimeUnix));
            default:
                return {};
        }
    }
    // Inline rename: the delegate reads the raw file name through EditRole
    // (never the formatted size/date columns, never the synthetic ".." row).
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
        case ColType:
            return fileTypeText(e);
        case ColModified:
            return e.mtimeUnix == 0
                ? QString("-")
                : QDateTime::fromSecsSinceEpoch(e.mtimeUnix)
                      .toString("yyyy-MM-dd HH:mm");
        case ColVerdict: {
            if (e.type == EntryType::Directory) return QString("-");
            auto it = verdicts_.find(idx.row());
            if (it == verdicts_.end()) return QString("...");
            const Decision& d = it->second;
            if (d.verdict == Verdict::Compress)
                return QString("压缩 (L%1)").arg(d.stage);
            if (d.verdict == Verdict::Store)
                return QString("直传 (L%1)").arg(d.stage);
            return QString("?");
        }
    }
    return {};
}

QVariant LocalFsModel::headerData(int section, Qt::Orientation o,
                                  int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case ColName: return "名称";
        case ColSize: return "大小";
        case ColType: return "文件类型";
        case ColModified: return "修改时间";
        case ColVerdict: return "压缩判定";
    }
    return {};
}

bool LocalFsModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    return hasMore_ && listingValid_;
}

void LocalFsModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || !canFetchMore(parent)) return;
    ListingPage page =
        cache_.list(backend_, currentFullPath(), lastOfCurrentPage_, 500);
    appendPage(page);
}

QString LocalFsModel::pathAt(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    const Entry& e = entryAt(idx.row());
    std::string rel;
    if (e.name == "..") {
        rel = parentRelative();
    } else {
        rel = currentDir_.empty() ? e.name : currentDir_ + "/" + e.name;
    }
    std::string full = root_;
    if (!full.empty() && full.back() != '/' && full.back() != '\\')
        full += "/";
    full += rel;
    return QString::fromStdString(full);
}

bool LocalFsModel::isDir(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return false;
    return entryAt(idx.row()).type == EntryType::Directory;
}

bool LocalFsModel::isParentRow(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return false;
    return entryAt(idx.row()).name == "..";
}

std::string LocalFsModel::parentRelative() const {
    if (currentDir_.empty()) return {};
    const std::size_t slash = currentDir_.rfind('/');
    return slash == std::string::npos
        ? std::string() : currentDir_.substr(0, slash);
}

std::string LocalFsModel::relativeAt(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    const Entry& e = entryAt(idx.row());
    if (e.name == "..") return parentRelative();
    return currentDir_.empty()
        ? e.name : currentDir_ + "/" + e.name;
}

// ---- drag source -----------------------------------------------------------

// Custom MIME carrying local absolute paths (one per line). Distinct from
// text/uri-list so the remote pane only accepts local drops and vice versa.
const char* kLocalPathsMime = "application/x-ftpflow-local-paths";

Qt::ItemFlags LocalFsModel::flags(const QModelIndex& idx) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(idx);
    if (idx.isValid() && !isParentRow(idx)) {
        f |= Qt::ItemIsDragEnabled;
        // Only the name cell is editable: an inline QLineEdit appears on it
        // for F2 / context-menu / slow second click renames.
        if (idx.column() == ColName) f |= Qt::ItemIsEditable;
    }
    return f;
}

QStringList LocalFsModel::mimeTypes() const {
    return {QLatin1String(kLocalPathsMime), QStringLiteral("text/uri-list")};
}

QMimeData* LocalFsModel::mimeData(const QModelIndexList& indexes) const {
    QStringList paths;
    QSet<int> seenRows;
    for (const QModelIndex& idx : indexes) {
        if (!idx.isValid() || idx.column() != ColName) continue;
        if (isParentRow(idx)) continue;
        if (seenRows.contains(idx.row())) continue;
        seenRows.insert(idx.row());
        const QString p = pathAt(idx);
        if (!p.isEmpty()) paths << p;
    }
    if (paths.isEmpty()) return nullptr;
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(kLocalPathsMime),
                  paths.join('\n').toUtf8());
    // Also provide standard URI list so dropping onto the OS shell works.
    QList<QUrl> urls;
    for (const QString& p : paths)
        urls << QUrl::fromLocalFile(p);
    mime->setUrls(urls);
    return mime;
}

// ---- filesystem mutations --------------------------------------------------

void LocalFsModel::makeDir(const std::string& name) {
    if (name.empty()) return;
    std::string full = currentFullPath();
    if (!full.empty() && full.back() != '/' && full.back() != '\\')
        full += "/";
    full += name;
    const std::string err = backend_.mkdir(full);
    if (err.empty()) {
        loadFirstPage();
    } else {
        emit operationError(QString("新建文件夹失败：%1")
                                .arg(QString::fromStdString(err)));
    }
}

int LocalFsModel::removeEntries(const QModelIndexList& rows) {
    if (rows.empty()) return 0;
    // Collect absolute paths first (indices may invalidate mid-loop).
    std::vector<std::string> paths;
    for (const QModelIndex& idx : rows) {
        if (!idx.isValid()) continue;
        if (isParentRow(idx)) continue;   // never delete ".."
        const QString p = pathAt(idx);
        if (!p.isEmpty()) paths.push_back(p.toStdString());
    }
    int failed = 0;
    std::string firstErr;
    for (const std::string& p : paths) {
        const std::string err = backend_.remove(p);
        if (!err.empty()) {
            ++failed;
            if (firstErr.empty()) firstErr = p + ": " + err;
        }
    }
    if (!paths.empty()) loadFirstPage();
    if (failed > 0) {
        emit operationError(QString("删除失败（%1/%2 个）：%3")
                                .arg(failed)
                                .arg((int)paths.size())
                                .arg(QString::fromStdString(firstErr)));
    }
    return static_cast<int>(paths.size()) - failed;
}

void LocalFsModel::renameEntry(const QModelIndex& idx,
                               const std::string& newName) {
    if (!idx.isValid() || newName.empty()) return;
    if (isParentRow(idx)) return;   // never rename ".."
    const QString oldPath = pathAt(idx);
    if (oldPath.isEmpty()) return;
    const std::string old = oldPath.toStdString();
    const std::size_t slash = old.find_last_of("/\\");
    const std::string parent = slash == std::string::npos
        ? std::string() : old.substr(0, slash + 1);
    const std::string err = backend_.rename(old, parent + newName);
    if (err.empty()) {
        loadFirstPage();
    } else {
        emit operationError(QString("重命名失败：%1")
                                .arg(QString::fromStdString(err)));
    }
}

bool LocalFsModel::setData(const QModelIndex& idx, const QVariant& value,
                           int role) {
    if (!idx.isValid() || role != Qt::EditRole || idx.column() != ColName)
        return false;
    if (isParentRow(idx)) return false;
    const QString newName = value.toString().trimmed();
    if (!isValidEntryName(newName)) {
        emit operationError(QStringLiteral(
            "重命名失败：名称为空或包含非法字符 / \\ : * ? \" < > |"));
        return false;
    }
    const QString oldName = QString::fromStdString(entryAt(idx.row()).name);
    if (newName == oldName) return false;

    // The item delegate is still closing its editor when setData() runs, and
    // renameEntry() resets the model (loadFirstPage). Defer the mutation to
    // the next event-loop turn so the editor closes cleanly first. Keep the
    // row (not the QModelIndex) and re-check the old name in case the listing
    // was refreshed in between.
    const int row = idx.row();
    QTimer::singleShot(0, this, [this, row, oldName, newName] {
        const QModelIndex i = index(row, ColName);
        if (!i.isValid() || isParentRow(i)) return;
        if (QString::fromStdString(entryAt(row).name) != oldName) return;
        renameEntry(i, newName.toStdString());
    });
    return true;
}

int LocalFsModel::moveEntries(const QStringList& sourcePaths,
                              const QString& destDir) {
    if (sourcePaths.isEmpty() || destDir.isEmpty()) return 0;
    bool changed = false;
    int moved = 0;
    int failed = 0;
    std::string firstErr;
    // Normalised destination with a trailing separator, for the subtree test.
    QString destNorm = destDir;
    if (!destNorm.endsWith('/') && !destNorm.endsWith('\\')) destNorm += '/';
    for (const QString& sp : sourcePaths) {
        const QString src = sp;
        if (src.isEmpty()) continue;
        // Base name of the source (last path component).
        const int slash = std::max(src.lastIndexOf('/'), src.lastIndexOf('\\'));
        const QString base = slash < 0 ? src : src.mid(slash + 1);
        if (base.isEmpty()) continue;
        QString dst = destNorm + base;
        // Guard: never move an entry into itself or its own child. Moving a
        // folder into a descendant would fail (or, on some backends, recurse
        // forever), so refuse it up front with a clear message.
        if (src == dst) continue;
        QString srcDirPrefix = src;
        if (!srcDirPrefix.endsWith('/') && !srcDirPrefix.endsWith('\\'))
            srcDirPrefix += '/';
        if (destNorm.startsWith(srcDirPrefix, Qt::CaseInsensitive)) {
            ++failed;
            if (firstErr.empty())
                firstErr = "不能把文件夹移动到它自己的子目录: " +
                           src.toStdString();
            continue;
        }
        const std::string err = backend_.rename(src.toStdString(), dst.toStdString());
        if (err.empty()) {
            changed = true;
            ++moved;
        } else {
            ++failed;
            if (firstErr.empty()) firstErr = err;
        }
    }
    if (changed) loadFirstPage();
    if (failed > 0) {
        emit operationError(QString("移动失败（%1 个）：%2")
                                .arg(failed)
                                .arg(QString::fromStdString(firstErr)));
    }
    return moved;
}

void LocalFsModel::refresh() {
    if (!listingValid_) return;
    cache_.invalidate(currentFullPath());
    loadFirstPage();
}

}  // namespace ftpflow
