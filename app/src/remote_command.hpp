// ftpflow -- remote command execution (SSH exec channel).
//
// Reuses the SSH credentials of the current SFTP connection to run a single
// shell command on the remote host and stream its stdout/stderr back to the
// UI. The heavy lift (SshExec::pump) blocks, so the run happens on a worker
// thread; results are delivered via Qt signals on the owner's thread.
//
// This is a QObject wrapper around core's SshExec (which is pure C++17 and
// can't post Qt signals itself).

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "ftpflow/ssh_exec.hpp"
#include "remote_fs_model.hpp"

namespace ftpflow {

// Runs a single command to completion and reports the outcome. One instance
// per execution; create it, connect the signals, then call start().
class RemoteCommand : public QObject {
    Q_OBJECT
public:
    // `ci` carries the SSH endpoint + known-hosts path of the active tab.
    explicit RemoteCommand(const RemoteFsModel::ConnInfo& ci,
                           QObject* parent = nullptr);
    ~RemoteCommand() override;

    RemoteCommand(const RemoteCommand&) = delete;
    RemoteCommand& operator=(const RemoteCommand&) = delete;

    // Launch the command on a worker thread. Safe to call once.
    void start(const QString& command);

    bool isRunning() const { return running_; }

signals:
    // Chunk of stdout/stderr, delivered in arrival order.
    void output(const QString& text);
    // Finished (cleanly or not). `exitCode` is the remote exit status (-1 if
    // the channel never reported one), `message` carries a human error when
    // the run itself failed (connect/auth/etc.).
    void finished(int exitCode, const QString& message);

private:
    void runWorker(std::string command);

    RemoteFsModel::ConnInfo conn_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> worker_;
};

}  // namespace ftpflow
