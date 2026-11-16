// ftpflow -- interactive SSH shell session (PTY-backed terminal transport).
//
// A QObject wrapper around core's SshExec::openShell() that runs the
// read/write pump on a worker thread and bridges it to the UI via queued
// signals. The TerminalWidget feeds keyboard bytes in through write(); the
// remote's PTY output flows back out through output().

#pragma once

#include <QObject>
#include <QByteArray>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ftpflow/ssh_exec.hpp"
#include "remote_fs_model.hpp"

namespace ftpflow {

class RemoteShell : public QObject {
    Q_OBJECT
public:
    explicit RemoteShell(const RemoteFsModel::ConnInfo& ci,
                         QObject* parent = nullptr);
    ~RemoteShell() override;

    RemoteShell(const RemoteShell&) = delete;
    RemoteShell& operator=(const RemoteShell&) = delete;

    // Open the PTY + shell and start the pump thread.
    void start(int cols, int rows);

    // Send keyboard input to the remote shell (thread-safe; queued).
    void write(const QByteArray& data);

    // Forward a window-size change to the remote PTY.
    void resize(int cols, int rows);

    // Close the shell and stop the worker.
    void stop();

    bool isRunning() const { return running_; }

signals:
    void output(const QByteArray& data);     // remote PTY -> terminal
    void finished(const QString& message);   // shell ended (empty = clean)

private:
    void runWorker(int cols, int rows);

    RemoteFsModel::ConnInfo conn_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::unique_ptr<std::thread> worker_;

    // Pending keyboard input (UI thread -> worker thread).
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> pending_;
    bool resizePending_ = false;
    int pendingCols_ = 0, pendingRows_ = 0;
};

}  // namespace ftpflow
