// ftpflow -- interactive SSH shell session implementation.
#include "remote_shell.hpp"

#include <vector>

#include "ftpflow/known_hosts.hpp"

namespace ftpflow {

RemoteShell::RemoteShell(const RemoteFsModel::ConnInfo& ci, QObject* parent)
    : QObject(parent), conn_(ci) {}

RemoteShell::~RemoteShell() {
    stopRequested_ = true;
    cv_.notify_all();
    if (worker_ && worker_->joinable()) worker_->join();
}

void RemoteShell::start(int cols, int rows) {
    if (running_.exchange(true)) return;
    stopRequested_ = false;
    worker_ = std::make_unique<std::thread>(
        &RemoteShell::runWorker, this, cols, rows);
}

void RemoteShell::write(const QByteArray& data) {
    if (data.isEmpty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    pending_.push_back(std::string(data.constData(),
                                    static_cast<std::size_t>(data.size())));
    cv_.notify_all();
}

void RemoteShell::resize(int cols, int rows) {
    std::lock_guard<std::mutex> lk(mu_);
    resizePending_ = true;
    pendingCols_ = cols;
    pendingRows_ = rows;
    cv_.notify_all();
}

void RemoteShell::stop() {
    stopRequested_ = true;
    cv_.notify_all();
}

void RemoteShell::runWorker(int cols, int rows) {
    const bool verify = !conn_.knownHostsPath.empty();
    std::vector<std::string> trusted;
    bool storeOk = true;
    if (verify) {
        KnownHosts kh(conn_.knownHostsPath);
        trusted = kh.trustedHexFor(conn_.ssh.host, conn_.ssh.port, &storeOk);
    }
    if (verify && !storeOk) {
        // Fail closed: never open a shell when the pinning store is unreadable.
        running_ = false;
        QMetaObject::invokeMethod(
            this,
            [this] { emit finished("无法读取 known_hosts 主机密钥库"); },
            Qt::QueuedConnection);
        return;
    }

    SshExec exec(verify ? HostKeyPolicy::Strict : HostKeyPolicy::AcceptNew);

    exec.setStdoutSink([this](const void* p, std::size_t n) {
        QMetaObject::invokeMethod(
            this,
            [this, d = QByteArray(static_cast<const char*>(p),
                                  static_cast<int>(n))] {
                emit output(d);
            },
            Qt::QueuedConnection);
    });

    std::string acceptedKey;
    SshResult r = exec.openShell(conn_.ssh, trusted, cols, rows, &acceptedKey);
    if (!r.ok()) {
        QString msg;
        if (r.status == SshStatus::HostKeyRejected &&
            !r.observedHostKeyHex.empty()) {
            msg = QString("主机密钥未受信任（指纹 SHA256: %1）。请先通过"
                          "“连接...”完成密钥确认。")
                      .arg(QString::fromStdString(
                          KnownHosts::fingerprintOfHex(r.observedHostKeyHex)));
        } else {
            msg = QString("打开终端失败: %1")
                      .arg(QString::fromStdString(r.message));
        }
        running_ = false;
        QMetaObject::invokeMethod(
            this, [this, msg] { emit finished(msg); }, Qt::QueuedConnection);
        return;
    }

    // Pump loop: flush pending input, forward resizes, then read output.
    while (!stopRequested_ && exec.isOpen()) {
        // Flush pending keyboard input.
        {
            std::lock_guard<std::mutex> lk(mu_);
            while (!pending_.empty()) {
                const std::string& s = pending_.front();
                exec.write(s.data(), s.size());
                pending_.pop_front();
            }
            if (resizePending_) {
                resizePending_ = false;
                exec.resize(pendingCols_, pendingRows_);
            }
        }
        if (!exec.pump(100)) break;   // channel closed
    }

    exec.close();
    running_ = false;
    QMetaObject::invokeMethod(
        this, [this] { emit finished(QString()); }, Qt::QueuedConnection);
}

}  // namespace ftpflow
