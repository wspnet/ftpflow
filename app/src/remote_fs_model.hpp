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
        ColModified,
        ColCount,
    };

    struct ConnInfo {
        bool isSftp = true;
        FtpOptions ftp;               // when !isSftp
        SshEndpoint ssh;              // when isSftp
        std::string startDir;         // remote initial directory
        std::string pushCmdTemplate;  // SSH compress agent template (opt)
    };

    explicit RemoteFsModel(QObject* parent = nullptr);
    ~RemoteFsModel();

    // Async: worker connects, then lists startDir. Emits connected() or
    // connectFailed().
    void connectTo(const ConnInfo& ci);
    void disconnect();

    bool isConnected() const { return connected_; }
    ConnInfo connInfo() const { return conn_; }
    std::string currentPath() const { return currentPath_; }
    std::string startDir() const { return conn_.startDir; }

    // Absolute remote path navigation.
    void enterDir(const std::string& dir);
    void goUp();
    void refresh();

    std::string pathAt(const QModelIndex& idx) const;
    std::string nameAt(const QModelIndex& idx) const;
    bool isDir(const QModelIndex& idx) const;

    // ---- async remote operations (worker thread) -------------------------
    // Each emits opFinished / opFailed; success auto-refreshes the current
    // directory. `name` is relative to the current directory.
    void makeDir(const std::string& name);
    void removeEntries(const QModelIndexList& rows);   // recursive for dirs
    void renameEntry(const QModelIndex& idx, const std::string& newName);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& idx, int role) const override;
    QVariant headerData(int section, Qt::Orientation o,
                        int role = Qt::DisplayRole) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

signals:
    void connected();
    void connectFailed(const QString& message);
    void browseFailed(const QString& message);
    void disconnected();
    void opFinished(const QString& message);
    void opFailed(const QString& message);

private:
    struct OpTarget {
        std::string path;
        bool isDir = false;
    };

    struct Task {
        enum class Kind : std::uint8_t {
            Connect, Disconnect, List, MakeDir, Remove, Rename, Shutdown,
        };
        Kind kind = Kind::List;
        ConnInfo conn;                 // Connect
        std::string dir;               // List / op working directory
        std::string startAfter;        // List cursor (SFTP)
        std::string name;              // MakeDir target / Rename new name
        std::vector<OpTarget> targets; // Remove / Rename source
        std::uint64_t generation = 0;  // List staleness tag
    };

    void workerLoop();
    void postListing(std::uint64_t generation, const std::string& dir,
                     std::vector<Entry> entries, bool hasMore, bool append,
                     const std::string& error);
    void pushTask(Task t);
    std::string joinRemote(const std::string& dir, const std::string& name) const;
    Entry entryAt(int row) const { return entries_[static_cast<std::size_t>(row)]; }

    // UI-thread state.
    ConnInfo conn_;
    std::string currentPath_;
    std::vector<Entry> entries_;
    std::string lastOfCurrentPage_;
    bool hasMore_ = false;
    bool connected_ = false;
    std::uint64_t navGen_ = 0;

    // Worker machinery.
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
};

}  // namespace ftpflow
