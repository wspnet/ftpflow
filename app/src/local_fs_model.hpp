// ftpflow -- local filesystem table model over LocalFsBackend.
// Paged loading (fetchMore) + background compression verdicts so the UI
// thread never blocks on file sampling.
#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtCore/QFutureWatcher>

#include <map>
#include <string>
#include <vector>

#include "ftpflow/compress_decide.hpp"
#include "ftpflow/vfs_local.hpp"

namespace ftpflow {

class LocalFsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColSize,
        ColModified,
        ColVerdict,
        ColCount,
    };

    // One background verdict probe: the row it belongs to plus the data
    // needed to sample the file off the UI thread.
    struct VerdictJob {
        int row = 0;
        std::string fullPath;
        std::uint64_t size = 0;
    };

    explicit LocalFsModel(QObject* parent = nullptr);

    // Opens `root`; the model resets to its listing (first page).
    bool openRoot(const std::string& root, std::string* error = nullptr);

    // Browses into a subdirectory (relative path from root).
    bool enterDir(const std::string& relative);
    bool goUp();

    std::string root() const { return root_; }
    std::string currentDir() const { return currentDir_; }
    QString pathAt(const QModelIndex& idx) const;
    std::string relativeAt(const QModelIndex& idx) const;
    bool isDir(const QModelIndex& idx) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& idx, int role) const override;
    QVariant headerData(int section, Qt::Orientation o,
                        int role = Qt::DisplayRole) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

signals:
    void verdictsReady();

private:
    void loadFirstPage();
    void appendPage(const ListingPage& page);
    void startVerdictBatch(int from, int count);
    void onVerdictsFinished();
    Entry entryAt(int row) const { return entries_[static_cast<std::size_t>(row)]; }

    LocalFsBackend backend_;
    std::string root_;                    // bound root (absolute)
    std::string currentDir_;              // relative, "" = root
    std::vector<Entry> entries_;
    std::string lastOfCurrentPage_;       // startAfter continuation name
    bool hasMore_ = false;
    bool listingValid_ = false;

    // row -> funnel decision; missing = "not evaluated yet".
    std::map<int, Decision> verdicts_;

    // Background verdict batch: one in flight, one queued.
    QFutureWatcher<Decision> verdictWatcher_;
    std::vector<VerdictJob> batchJobs_;
    bool batchPending_ = false;
    int pendingFrom_ = 0;
    int pendingCount_ = 0;
};

}  // namespace ftpflow
