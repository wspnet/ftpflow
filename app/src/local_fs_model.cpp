// ftpflow -- local filesystem table model implementation.
#include "local_fs_model.hpp"

#include <QtConcurrent/QtConcurrentMap>
#include <QtCore/QDateTime>
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

    constexpr long kWin = 64 * 1024;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    const long mid = len / 2;
    const long tail = len > kWin ? len - kWin : 0;
    for (long off : {0L, mid, tail}) {
        if (off < 0 || off >= len) continue;
        std::fseek(f, off, SEEK_SET);
        std::vector<std::uint8_t> buf(kWin);
        std::size_t g = std::fread(buf.data(), 1, kWin, f);
        p.sample.insert(p.sample.end(), buf.data(), buf.data() + g);
    }
    std::fclose(f);

    CostModel cm;  // default: 20 MiB/s unknown-link assumption
    return decideFullFunnel(p, cm);
}

QString humanSize(std::uint64_t n) {
    if (n < 1024) return QString::number(static_cast<qulonglong>(n)) + " B";
    const double kb = n / 1024.0;
    if (kb < 1024.0) return QString::number(kb, 'f', 1) + " KiB";
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return QString::number(mb, 'f', 1) + " MiB";
    return QString::number(mb / 1024.0, 'f', 2) + " GiB";
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
    loadFirstPage();
    return true;
}

bool LocalFsModel::enterDir(const std::string& relative) {
    if (!listingValid_) return false;
    currentDir_ = relative;
    loadFirstPage();
    return true;
}

bool LocalFsModel::goUp() {
    if (currentDir_.empty()) return false;
    const std::size_t slash = currentDir_.rfind('/');
    currentDir_ = slash == std::string::npos
        ? std::string() : currentDir_.substr(0, slash);
    loadFirstPage();
    return true;
}

void LocalFsModel::loadFirstPage() {
    beginResetModel();
    entries_.clear();
    verdicts_.clear();
    lastOfCurrentPage_.clear();
    hasMore_ = false;
    endResetModel();
    verdictWatcher_.cancel();
    batchJobs_.clear();
    batchPending_ = false;

    ListingPage page = backend_.list(currentDir_, {}, 500);
    appendPage(page);
}

void LocalFsModel::appendPage(const ListingPage& page) {
    hasMore_ = page.hasMore;
    if (page.entries.empty()) return;
    const int first = static_cast<int>(entries_.size());
    beginInsertRows({}, first,
                    first + static_cast<int>(page.entries.size()) - 1);
    for (const Entry& e : page.entries) {
        entries_.push_back(e);
        lastOfCurrentPage_ = e.name;
    }
    endInsertRows();

    startVerdictBatch(first, static_cast<int>(page.entries.size()));
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
    ListingPage page = backend_.list(currentDir_, lastOfCurrentPage_, 500);
    appendPage(page);
}

QString LocalFsModel::pathAt(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    const Entry& e = entryAt(idx.row());
    std::string rel = currentDir_.empty()
        ? e.name : currentDir_ + "/" + e.name;
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

std::string LocalFsModel::relativeAt(const QModelIndex& idx) const {
    if (!idx.isValid() || idx.row() >= static_cast<int>(entries_.size()))
        return {};
    const Entry& e = entryAt(idx.row());
    return currentDir_.empty()
        ? e.name : currentDir_ + "/" + e.name;
}

}  // namespace ftpflow
