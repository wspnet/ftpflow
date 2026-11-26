// ftpflow -- remote filesystem table model over FtpSession/SftpSession.
// All network I/O happens on one internal worker thread (sessions are not
// thread-safe); results are marshaled back to the UI thread via queued
// invocations. Navigation uses a generation counter so stale listings
// (superseded by a newer click) are dropped.
#pragma once

#include <QtCore/QAbstractTableModel>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ftpflow/ftp_session.hpp"
#include "ftpflow/sftp_session.hpp"
#include "ftpflow/vfs.hpp"

namespace ftpflow {

class RemoteFsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColSize,
        ColType,
        ColModified,
        ColCount,
    };

    struct ConnInfo {
        bool isSftp = true;
        FtpOptions ftp;               // when !isSftp
        SshEndpoint ssh;              // when isSftp
        std::string startDir;         // remote initial directory
        std::string pushCmdTemplate;  // SSH compress agent template (opt)
        std::string siteName;         // saved-site reference (cred recovery)
        std::string knownHostsPath;   // host-key store ("" = no strict verify)
    };

    explicit RemoteFsModel(QObject* parent = nullptr);
    ~RemoteFsModel();

    // Async: worker connects, then lists startDir. Emits connected() or
    // connectFailed().
    void connectTo(const ConnInfo& ci);
    void disconnect();

    // Persist a host key as trusted and reconnect with strict verification.
    // Called after the user confirms a hostKeyUnknown() prompt.
    void trustHostKey(const QString& host, quint16 port, const QString& keyHex);

    bool isConnected() const { return connected_; }
    ConnInfo connInfo() const { return conn_; }
    std::string currentPath() const { return currentPath_; }
    std::string startDir() const { return conn_.startDir; }

    // The FTP filename encoding the live session has locked in (after a
    // list() in "auto" mode this becomes "gbk" for a Chinese server). The
    // transfer queue opens its OWN FtpSession from a request snapshot, so
    // the request must carry the already-locked encoding -- otherwise a
    // Chinese filename is sent as UTF-8 bytes to a GBK server and the
    // SIZE/RETR never match (the transfer stalls). Defaults to "auto" when
    // not yet connected or when the endpoint is SFTP.
    std::string effectiveEncoding() const { return effEncoding_; }

    // Absolute remote path navigation.
    void enterDir(const std::string& dir);
    void goUp();
    void refresh();

    std::string pathAt(const QModelIndex& idx) const;
    std::string nameAt(const QModelIndex& idx) const;
    bool isDir(const QModelIndex& idx) const;
    // True when `idx` is the synthetic ".." parent row (top of a non-root
    // listing). Such rows cannot be renamed/deleted/dragged.
    bool isParentRow(const QModelIndex& idx) const;

    // ---- async remote operations (worker thread) -------------------------
    // Each emits opFinished / opFailed; success auto-refreshes the current
    // directory. `name` is relative to the current directory.
    void makeDir(const std::string& name);
    void removeEntries(const QModelIndexList& rows);   // recursive for dirs
    void renameEntry(const QModelIndex& idx, const std::string& newName);
    // Move the given remote source paths into the remote directory
    // `destDir`, keeping each entry's base name. Async (worker thread).
    void moveEntries(const std::vector<std::string>& sourcePaths,
                     const std::string& destDir);
    // Fetch a remote text file into `localPath`, then emit editDownloaded()
    // (or opFailed). Reuses the live session on the worker thread.
    void downloadFile(const std::string& remotePath,
                      const std::string& localPath);
    // Push a local text file back to `remotePath`, then emit editUploaded()
    // (or opFailed). Reuses the live session on the worker thread.
    void uploadFile(const std::string& localPath,
                    const std::string& remotePath);

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

    // Drag source: expose selected remote entries as remote paths (copy).
    Qt::ItemFlags flags(const QModelIndex& idx) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

signals:
    void connected();
    void connectFailed(const QString& message);
    void browseFailed(const QString& message);
    void disconnected();
    // Long-running remote operations (recursive delete / move / mkdir /
    // rename) report progress so the UI can show a busy indicator instead of
    // looking frozen. `opStarted` is emitted synchronously from the calling
    // (UI) thread right after the task is queued; `opProgress` streams from
    // the worker (already marshalled to the UI thread). Exactly one of
    // opFinished / opFailed terminates the run.
    void opStarted(const QString& message);
    void opProgress(const QString& message);
    void opFinished(const QString& message);
    void opFailed(const QString& message);
    // Emitted when the remote transport dropped mid-session (socket error),
    // as opposed to a business/protocol error. The UI can auto-reconnect.
    void connectionLost(const QString& message);
    // Emitted when a text file edit download finished (remote -> local
    // staging path) or the upload-back finished. `localPath` lets the UI
    // pair the operation to the editor it opened.
    void editDownloaded(const QString& remotePath, const QString& localPath);
    void editUploaded(const QString& remotePath);
    // Emitted on strict host-key rejection: carry the standard SHA256
    // fingerprint and the observed blob hex for a trust-on-first-use prompt.
    void hostKeyUnknown(const QString& fingerprint, const QString& host,
                        quint16 port, const QString& keyHex);

private:
    struct OpTarget {
        std::string path;
        bool isDir = false;
    };

    struct Task {
        enum class Kind : std::uint8_t {
            Connect, Disconnect, List, MakeDir, Remove, Rename, Move,
            DownloadFile, UploadFile, Shutdown,
        };
        Kind kind = Kind::List;
        ConnInfo conn;                 // Connect
        std::string dir;               // List / op working directory
        std::string srcDir;            // Move: source dir to relist after
        std::string startAfter;        // List cursor (SFTP)
        std::string name;              // MakeDir target / Rename new name
        std::vector<OpTarget> targets; // Remove / Rename / Move source
        std::uint64_t generation = 0;  // List staleness tag
        std::string remotePath;        // DownloadFile / UploadFile remote side
        std::string localPath;         // DownloadFile / UploadFile local side
    };

    void workerLoop();
    void postListing(std::uint64_t generation, const std::string& dir,
                     std::vector<Entry> entries, bool hasMore, bool append,
                     const std::string& error);
    void pushTask(Task t);
    std::string joinRemote(const std::string& dir, const std::string& name) const;
    std::string parentPath() const;   // remote path of the parent dir
    Entry entryAt(int row) const { return entries_[static_cast<std::size_t>(row)]; }

    // UI-thread state.
    ConnInfo conn_;
    std::string currentPath_;
    std::vector<Entry> entries_;
    std::string lastOfCurrentPage_;
    bool hasMore_ = false;
    bool connected_ = false;
    std::uint64_t navGen_ = 0;
    std::string effEncoding_ = "auto";   // FTP encoding locked by list()
    // Absolute remote paths with a delete in flight. Those rows are drawn
    // dimmed/italic so the user immediately sees WHICH entries the running
    // delete is working on. Cleared when the follow-up listing lands.
    std::set<std::string> pendingRemove_;

    // Worker machinery.
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
};

}  // namespace ftpflow
