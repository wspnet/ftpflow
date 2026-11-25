// ftpflow -- remote command execution (SSH exec channel).
#include "remote_command.hpp"

#include <thread>
#include <vector>

#include "ftpflow/known_hosts.hpp"

namespace ftpflow {

RemoteCommand::RemoteCommand(const RemoteFsModel::ConnInfo& ci, QObject* parent)
    : QObject(parent), conn_(ci) {}

RemoteCommand::~RemoteCommand() {
    // Break the pump loop first: without this a long-running command
    // (`tail -f`, `top`, ...) would keep the worker alive and this join
    // would block the UI thread forever (e.g. closing the command dialog).
    stop();
    if (worker_ && worker_->joinable()) worker_->join();
}

void RemoteCommand::start(const QString& command) {
    if (running_.exchange(true)) return;  // already running
    stop_ = false;
    worker_ = std::make_unique<std::thread>(
        &RemoteCommand::runWorker, this, command.toStdString());
}

void RemoteCommand::stop() {
    stop_ = true;
}

void RemoteCommand::runWorker(std::string command) {
    // Build the trusted-key list from the same known_hosts store the SFTP
    // session used, mirroring RemoteFsModel's connect path.
    const bool verify = !conn_.knownHostsPath.empty();
    std::vector<std::string> trusted;
    bool storeOk = true;
    if (verify) {
        KnownHosts kh(conn_.knownHostsPath);
        trusted = kh.trustedHexFor(conn_.ssh.host, conn_.ssh.port, &storeOk);
    }
    if (verify && !storeOk) {
        // Fail closed: never run commands when the pinning store is unreadable.
        running_ = false;
        QMetaObject::invokeMethod(
            this,
            [this] {
                emit finished(-1, "无法读取 known_hosts 主机密钥库");
            },
            Qt::QueuedConnection);
        return;
    }

    SshExec exec(verify ? HostKeyPolicy::Strict : HostKeyPolicy::AcceptNew);

    // Forward output as queued signals to the UI thread.
    exec.setStdoutSink([this](const void* p, std::size_t n) {
        QMetaObject::invokeMethod(
            this,
            [this, s = QString::fromUtf8(static_cast<const char*>(p),
                                        static_cast<int>(n))] {
                emit output(s);
            },
            Qt::QueuedConnection);
    });
    exec.setStderrSink([this](const void* p, std::size_t n) {
        QMetaObject::invokeMethod(
            this,
            [this, s = QString::fromUtf8(static_cast<const char*>(p),
                                        static_cast<int>(n))] {
                emit output(s);
            },
            Qt::QueuedConnection);
    });

    std::string acceptedKey;
    SshResult r = exec.open(conn_.ssh, command, trusted, &acceptedKey);
    if (!r.ok()) {
        QString msg;
        if (r.status == SshStatus::HostKeyRejected &&
            !r.observedHostKeyHex.empty()) {
            msg = QString("主机密钥未受信任（指纹 SHA256: %1）。请先通过"
                          "“连接...”完成密钥确认。")
                      .arg(QString::fromStdString(
                          KnownHosts::fingerprintOfHex(r.observedHostKeyHex)));
        } else {
            msg = QString("命令执行失败: %1")
                      .arg(QString::fromStdString(r.message));
        }
        QMetaObject::invokeMethod(
            this, [this, msg] { emit finished(-1, msg); }, Qt::QueuedConnection);
        running_ = false;
        return;
    }

    // Close stdin immediately: a one-shot command must not wait for input
    // (commands that read stdin, e.g. `cat`, would otherwise block forever).
    exec.sendEof();

    // Drain stdout/stderr until the channel closes -- or until stop() is
    // requested (pump() polls in 500 ms slices, so cancellation is prompt).
    while (!stop_.load() && exec.pump(500)) {
    }
    const bool aborted = stop_.load();

    const int code = exec.exitCode();
    exec.close();
    running_ = false;

    QMetaObject::invokeMethod(
        this,
        [this, code, aborted] {
            emit finished(aborted ? -1 : code,
                          aborted ? QStringLiteral("命令已中止")
                                  : QString());
        },
        Qt::QueuedConnection);
}

}  // namespace ftpflow
