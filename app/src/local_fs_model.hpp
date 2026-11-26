// ftpflow -- local filesystem table model over LocalFsBackend.
// Paged loading (fetchMore) + background compression verdicts so the UI
// thread never blocks on file sampling.
#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtCore/QFutureWatcher>
#include <QtCore/QModelIndexList>

#include <map>
#include <string>
#include <vector>

#include <memory>

#include "ftpflow/compress_decide.hpp"
#include "ftpflow/dir_cache.hpp"
#include "ftpflow/fs_watcher.hpp"
#include "ftpflow/vfs_local.hpp"

namespace ftpflow {

class LocalFsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColSize,
        ColType,
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
    // Absolute path of the directory on screen (root + currentDir).
    std::string currentDirFullPath() const { return currentFullPath(); }
    QString pathAt(const QModelIndex& idx) const;
    std::string relativeAt(const QModelIndex& idx) const;
    bool isDir(const QModelIndex& idx) const;
    // True when `idx` is the synthetic ".." parent row (top of a non-root
    // listing). Such rows cannot be renamed/deleted/dragged.
    bool isParentRow(const QModelIndex& idx) const;

    // Filesystem mutations. Each operates on absolute paths and reloads the
    // current directory on success. `name` is relative to the current dir.
    // The entry-taking variants return how many entries were actually
    // deleted / moved so the UI can report a result.
    void makeDir(const std::string& name);
    int removeEntries(const QModelIndexList& rows);   // recursive for dirs
    void renameEntry(const QModelIndex& idx, const std::string& newName);

    // Move the given absolute source paths into `destDir` (absolute), keeping
    // each entry's base name. Cross-directory rename on the same volume.
    // Returns the number of entries moved.
    int moveEntries(const QStringList& sourcePaths, const QString& destDir);

    // Reload the current directory (drop the cache and re-list).
    void refresh();

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& idx, int role) const override;
    // Inline rename: the item delegate writes the edited name through
    // Qt::EditRole on the name column.
    bool setData(const QModelIndex& idx, const QVariant& value,
                 int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation o,
                        int role = Qt::DisplayRole) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    // Drag source: expose selected entries as local file paths (copy).
    Qt::ItemFlags flags(const QModelIndex& idx) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

signals:
    void verdictsReady();
    // A filesystem mutation (delete / rename / move / mkdir) failed.
    // Carries a human-readable summary for the status bar / message log.
    void operationError(const QString& message);

private:
    void loadFirstPage();
    void appendPage(const ListingPage& page);
    void startVerdictBatch(int from, int count);
    void onVerdictsFinished();
    void onDirChangedExternal(const std::string& dir);
    Entry entryAt(int row) const { return entries_[static_cast<std::size_t>(row)]; }
    std::string currentFullPath() const;
    std::string parentRelative() const;   // relative path of the parent dir
    // Absolute path of the parent of the current directory; empty when the
    // current directory is already a drive root (e.g. "C:\"). Used both to
    // decide whether to show the ".." row and as the goUp() target.
    std::string parentFullPath() const;

    LocalFsBackend backend_;
    DirCache cache_;                     // full-listing snapshots per dir
    std::unique_ptr<FsWatcher> watcher_; // auto-invalidate on external change
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
