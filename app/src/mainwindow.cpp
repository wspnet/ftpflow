// ftpflow -- main window implementation.
#include "mainwindow.hpp"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QVBoxLayout>
#include <QWidget>

#include "connect_dialog.hpp"
#include "ftpflow/protocol_status.hpp"
#include "ftpflow/version.hpp"

namespace ftpflow {

namespace {

QString humanSpeed(double bps) {
    if (bps <= 0) return {};
    if (bps < 1024.0) return QString::number(bps, 'f', 0) + " B/s";
    if (bps < 1024.0 * 1024.0)
        return QString::number(bps / 1024.0, 'f', 1) + " KiB/s";
    return QString::number(bps / (1024.0 * 1024.0), 'f', 2) + " MiB/s";
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setWindowTitle(QString("ftpflow %1 - 文件传输工作台")
                       .arg(versionString()));

    queue_ = std::make_unique<TransferQueue>(2, 100);
    queue_->setEventCallback([this](const TransferEvent& ev) {
        // Worker thread -> UI thread.
        QMetaObject::invokeMethod(
            this, [this, ev] { onTransferEvent(ev); }, Qt::QueuedConnection);
    });
}

MainWindow::~MainWindow() {
    // The queue's callback captures `this`; destroy it before the widgets.
    queue_.reset();
}

void MainWindow::buildUi() {
    // --- top bar: local path + open + up ---------------------------------
    auto* top = new QWidget(this);
    auto* topLay = new QHBoxLayout(top);
    topLay->setContentsMargins(6, 4, 6, 4);
    pathEdit_ = new QLineEdit(top);
    pathEdit_->setPlaceholderText("本地目录（输入路径回车，或点浏览）");
    auto* browseBtn = new QPushButton("浏览...", top);
    auto* upBtn = new QPushButton("向上", top);
    topLay->addWidget(pathEdit_, 1);
    topLay->addWidget(browseBtn);
    topLay->addWidget(upBtn);

    connect(browseBtn, &QPushButton::clicked, this, &MainWindow::onOpen);
    connect(upBtn, &QPushButton::clicked, this, &MainWindow::onUp);
    connect(pathEdit_, &QLineEdit::returnPressed, this, [this] {
        showRoot(pathEdit_->text().toStdString());
    });

    // --- local pane (left) --------------------------------------------------
    auto* leftPane = new QWidget(this);
    auto* leftLay = new QVBoxLayout(leftPane);
    leftLay->setContentsMargins(0, 0, 0, 0);
    auto* leftTitle = new QLabel("本地", leftPane);
    leftTitle->setStyleSheet("font-weight: bold; padding: 2px;");
    model_ = new LocalFsModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(Qt::DisplayRole);
    left_ = new QTableView(leftPane);
    left_->setModel(proxy_);
    left_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    left_->verticalHeader()->setDefaultSectionSize(22);
    left_->setShowGrid(false);
    left_->setAlternatingRowColors(true);
    left_->setSelectionBehavior(QAbstractItemView::SelectRows);
    left_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    left_->setSortingEnabled(true);
    left_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    left_->verticalHeader()->hide();
    left_->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    leftLay->addWidget(leftTitle);
    leftLay->addWidget(left_, 1);
    connect(left_, &QTableView::doubleClicked, this,
            [this](const QModelIndex& idx) {
                onActivated(proxy_->mapToSource(idx));
            });
    connect(model_, &LocalFsModel::verdictsReady, this,
            &MainWindow::onVerdictsReady);

    // --- transfer arrow column (middle) ------------------------------------
    auto* arrows = new QWidget(this);
    auto* arrowLay = new QVBoxLayout(arrows);
    arrowLay->setContentsMargins(4, 4, 4, 4);
    arrowLay->addStretch(1);
    auto* uploadBtn = new QPushButton("上传 →", arrows);
    auto* downloadBtn = new QPushButton("← 下载", arrows);
    arrowLay->addWidget(uploadBtn);
    arrowLay->addWidget(downloadBtn);
    arrowLay->addStretch(1);
    connect(uploadBtn, &QPushButton::clicked, this,
            &MainWindow::enqueueUploads);
    connect(downloadBtn, &QPushButton::clicked, this,
            &MainWindow::enqueueDownloads);

    // --- remote pane (right) -------------------------------------------------
    auto* rightPane = new QWidget(this);
    auto* rightLay = new QVBoxLayout(rightPane);
    rightLay->setContentsMargins(0, 0, 0, 0);
    auto* rightTitle = new QLabel("远端", rightPane);
    rightTitle->setStyleSheet("font-weight: bold; padding: 2px;");
    auto* remoteBar = new QWidget(rightPane);
    auto* remoteBarLay = new QHBoxLayout(remoteBar);
    remoteBarLay->setContentsMargins(2, 0, 2, 0);
    remotePathEdit_ = new QLineEdit(remoteBar);
    remotePathEdit_->setPlaceholderText("未连接（点“连接...”打开远端）");
    remotePathEdit_->setReadOnly(true);
    connectBtn_ = new QPushButton("连接...", remoteBar);
    disconnectBtn_ = new QPushButton("断开", remoteBar);
    auto* remoteUpBtn = new QPushButton("向上", remoteBar);
    auto* remoteRefreshBtn = new QPushButton("刷新", remoteBar);
    disconnectBtn_->setEnabled(false);
    remoteUpBtn->setEnabled(false);
    remoteRefreshBtn->setEnabled(false);
    remoteBarLay->addWidget(remotePathEdit_, 1);
    remoteBarLay->addWidget(connectBtn_);
    remoteBarLay->addWidget(disconnectBtn_);
    remoteBarLay->addWidget(remoteUpBtn);
    remoteBarLay->addWidget(remoteRefreshBtn);

    remote_ = new RemoteFsModel(this);
    remoteProxy_ = new QSortFilterProxyModel(this);
    remoteProxy_->setSourceModel(remote_);
    remoteProxy_->setSortRole(Qt::DisplayRole);
    rightTable_ = new QTableView(rightPane);
    rightTable_->setModel(remoteProxy_);
    rightTable_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    rightTable_->verticalHeader()->setDefaultSectionSize(22);
    rightTable_->setShowGrid(false);
    rightTable_->setAlternatingRowColors(true);
    rightTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rightTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rightTable_->setSortingEnabled(true);
    rightTable_->horizontalHeader()->setSectionResizeMode(0,
                                                          QHeaderView::Stretch);
    rightTable_->verticalHeader()->hide();
    rightTable_->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    rightLay->addWidget(rightTitle);
    rightLay->addWidget(remoteBar);
    rightLay->addWidget(rightTable_, 1);

    connect(connectBtn_, &QPushButton::clicked, this, &MainWindow::onConnect);
    connect(disconnectBtn_, &QPushButton::clicked, this,
            &MainWindow::onDisconnect);
    connect(remoteUpBtn, &QPushButton::clicked, this, &MainWindow::onRemoteUp);
    connect(remoteRefreshBtn, &QPushButton::clicked, this,
            &MainWindow::onRemoteRefresh);
    connect(rightTable_, &QTableView::doubleClicked, this,
            [this](const QModelIndex& idx) {
                onRemoteActivated(remoteProxy_->mapToSource(idx));
            });
    connect(remote_, &RemoteFsModel::connected, this,
            &MainWindow::onRemoteConnected);
    connect(remote_, &RemoteFsModel::connectFailed, this,
            &MainWindow::onRemoteConnectFailed);
    connect(remote_, &RemoteFsModel::browseFailed, this,
            &MainWindow::onRemoteBrowseFailed);
    // Enable/disable the navigation buttons with connection state.
    connect(remote_, &RemoteFsModel::connected, this, [this, remoteUpBtn,
                                                       remoteRefreshBtn] {
        remoteUpBtn->setEnabled(true);
        remoteRefreshBtn->setEnabled(true);
    });
    connect(remote_, &RemoteFsModel::disconnected, this,
            [this, remoteUpBtn, remoteRefreshBtn] {
                remoteUpBtn->setEnabled(false);
                remoteRefreshBtn->setEnabled(false);
                remotePathEdit_->setText("未连接");
            });

    // --- splitters -------------------------------------------------------------
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftPane);
    splitter->addWidget(arrows);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    splitter->setCollapsible(1, false);

    // --- transfer dock (bottom) --------------------------------------------------
    transferDock_ = new QDockWidget("传输队列", this);
    transferList_ = new QListWidget(transferDock_);
    transferList_->setAlternatingRowColors(true);
    transferDock_->setWidget(transferList_);
    transferDock_->setFeatures(QDockWidget::DockWidgetMovable |
                               QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, transferDock_);
    transferDock_->hide();   // appears with the first job

    auto* central = new QWidget(this);
    auto* centralLay = new QVBoxLayout(central);
    centralLay->setContentsMargins(0, 0, 0, 0);
    centralLay->setSpacing(0);
    centralLay->addWidget(top);
    centralLay->addWidget(splitter, 1);
    setCentralWidget(central);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);

    const auto st = protocolEngineStatus();
    statusLabel_->setText(QString("engines: ftp=%1 ssh=%2 zstd=%3 "
                                  "exec-compression=%4")
                              .arg(st.ftpAvailable).arg(st.sshAvailable)
                              .arg(st.zstdAvailable)
                              .arg(st.execChannelCompression));

    const QString home = qEnvironmentVariable("USERPROFILE");
    if (!home.isEmpty()) {
        pathEdit_->setText(home);
        showRoot(home.toStdString());
    }
}

void MainWindow::showRoot(const std::string& root) {
    std::string err;
    if (!model_->openRoot(root, &err)) {
        statusBar()->showMessage(
            QString("打开失败: %1").arg(QString::fromStdString(err)), 5000);
        return;
    }
    pathEdit_->setText(QString::fromStdString(model_->root()));
    refreshStatusBar();
}

void MainWindow::onOpen() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, "选择本地目录", pathEdit_->text());
    if (!dir.isEmpty()) {
        showRoot(dir.toStdString());
    }
}

void MainWindow::onUp() {
    if (model_->goUp()) refreshStatusBar();
}

void MainWindow::onActivated(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    if (model_->isDir(idx)) {
        model_->enterDir(model_->relativeAt(idx));
        refreshStatusBar();
    }
}

void MainWindow::onVerdictsReady() {
    refreshStatusBar();
}

// ---- remote pane ------------------------------------------------------------

void MainWindow::onConnect() {
    ConnectDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    connectBtn_->setEnabled(false);
    remotePathEdit_->setText("连接中...");
    remote_->connectTo(dlg.connInfo());
}

void MainWindow::onDisconnect() {
    remote_->disconnect();
    connectBtn_->setEnabled(true);
    disconnectBtn_->setEnabled(false);
}

void MainWindow::onRemoteConnected() {
    connectBtn_->setEnabled(false);
    disconnectBtn_->setEnabled(true);
    remotePathEdit_->setText(QString::fromStdString(remote_->currentPath()));
    refreshStatusBar();
}

void MainWindow::onRemoteConnectFailed(const QString& msg) {
    connectBtn_->setEnabled(true);
    disconnectBtn_->setEnabled(false);
    remotePathEdit_->setText("未连接");
    statusBar()->showMessage(msg, 8000);
}

void MainWindow::onRemoteBrowseFailed(const QString& msg) {
    statusBar()->showMessage(msg, 8000);
}

void MainWindow::onRemoteUp() {
    remote_->goUp();
    remotePathEdit_->setText(QString::fromStdString(remote_->currentPath()));
}

void MainWindow::onRemoteRefresh() {
    remote_->refresh();
}

void MainWindow::onRemoteActivated(const QModelIndex& idx) {
    if (!idx.isValid() || !remote_->isConnected()) return;
    if (remote_->isDir(idx)) {
        remote_->enterDir(remote_->pathAt(idx));
        remotePathEdit_->setText(
            QString::fromStdString(remote_->currentPath()));
    }
}

// ---- transfer queue -----------------------------------------------------

void MainWindow::enqueueUploads() {
    if (!remote_->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    const QModelIndexList rows = left_->selectionModel()->selectedRows(0);
    if (rows.isEmpty()) {
        statusBar()->showMessage("未选中本地文件", 4000);
        return;
    }
    const auto ci = remote_->connInfo();
    for (const QModelIndex& proxyIdx : rows) {
        const QModelIndex src = proxy_->mapToSource(proxyIdx);
        if (model_->isDir(src)) continue;   // file transfers only (M3: dirs)
        const QString localPath = model_->pathAt(src);
        TransferRequest req;
        req.direction = TransferDirection::Upload;
        req.protocol = ci.isSftp ? BackendKind::Sftp : BackendKind::Ftp;
        req.localPath = localPath.toStdString();
        // Remote target: current remote dir + file base name.
        std::string name = localPath.toStdString();
        const std::size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const std::string remoteDir = remote_->currentPath();
        req.remotePath = (remoteDir.empty() || remoteDir == ".")
            ? name : remoteDir + "/" + name;
        req.ftp = ci.ftp;
        req.ssh = ci.ssh;
        req.pushCommandTemplate = ci.pushCmdTemplate;
        queue_->enqueue(std::move(req));
    }
    transferDock_->show();
}

void MainWindow::enqueueDownloads() {
    if (!remote_->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    const QModelIndexList rows = rightTable_->selectionModel()->selectedRows(0);
    if (rows.isEmpty()) {
        statusBar()->showMessage("未选中远端文件", 4000);
        return;
    }
    const auto ci = remote_->connInfo();
    std::string localDir = model_->currentDirFullPath();
    for (const QModelIndex& proxyIdx : rows) {
        const QModelIndex src = remoteProxy_->mapToSource(proxyIdx);
        if (remote_->isDir(src)) continue;
        const std::string remotePath = remote_->pathAt(src);
        const std::size_t slash = remotePath.find_last_of('/');
        const std::string name = slash == std::string::npos
            ? remotePath : remotePath.substr(slash + 1);
        TransferRequest req;
        req.direction = TransferDirection::Download;
        req.protocol = ci.isSftp ? BackendKind::Sftp : BackendKind::Ftp;
        req.remotePath = remotePath;
        req.localPath = localDir + "\\" + name;
        req.ftp = ci.ftp;
        req.ssh = ci.ssh;
        queue_->enqueue(std::move(req));
    }
    transferDock_->show();
}

void MainWindow::onTransferEvent(const TransferEvent& ev) {
    auto it = transferItems_.find(ev.jobId);
    if (it == transferItems_.end()) {
        auto* item = new QListWidgetItem;
        transferList_->addItem(item);
        it = transferItems_.emplace(ev.jobId, item).first;
    }

    const QString arrow = ev.progress.bytesOnWire < ev.progress.bytesMoved &&
                                  ev.progress.bytesOnWire > 0
        ? "⇗" : "⇕";
    const double pct = ev.progress.bytesTotal > 0
        ? 100.0 * (double)ev.progress.bytesMoved /
              (double)ev.progress.bytesTotal
        : 0.0;
    QString text = QString("%1 [job %2] %3 %4%")
                       .arg(arrow)
                       .arg((unsigned long long)ev.jobId)
                       .arg(ev.mode == TransferMode::Compress ? "压缩" : "直传")
                       .arg(pct, 0, 'f', 1);
    if (ev.kind == TransferEvent::Kind::Progress ||
        ev.kind == TransferEvent::Kind::Finished) {
        text += QString("  %1/%2")
                    .arg((unsigned long long)ev.progress.bytesMoved)
                    .arg((unsigned long long)ev.progress.bytesTotal);
        const QString spd = humanSpeed(ev.progress.speedBytesPerSec);
        if (!spd.isEmpty()) text += "  " + spd;
        if (ev.mode == TransferMode::Compress && ev.progress.bytesMoved > 0) {
            text += QString("  压缩比 %1")
                        .arg(ev.progress.compressionRatio, 0, 'f', 3);
        }
    }
    if (ev.kind == TransferEvent::Kind::Finished) {
        if (ev.state == TransferState::Done) {
            text += "  ✓ 完成";
        } else if (ev.state == TransferState::Failed) {
            text += QString("  ✗ 失败: %1").arg(
                QString::fromStdString(ev.error));
        } else if (ev.state == TransferState::Canceled) {
            text += "  已取消";
        }
    }
    it->second->setText(text);

    // Uploads may have changed the remote directory: refresh on finish.
    if (ev.kind == TransferEvent::Kind::Finished && remote_->isConnected()) {
        remote_->refresh();
    }
}

void MainWindow::refreshStatusBar() {
    const std::string rel = model_->currentDir();
    const QString where = rel.empty()
        ? QString("(root)")
        : QString::fromStdString(rel);
    const auto st = protocolEngineStatus();
    QString remote = remote_->isConnected()
        ? QString::fromStdString(remote_->currentPath())
        : QString("-");
    statusLabel_->setText(QString("%1 | %2 行 | 远端: %3 | engines: ftp=%4 "
                                  "ssh=%5 zstd=%6 exec=%7")
                              .arg(where)
                              .arg(model_->rowCount({}))
                              .arg(remote)
                              .arg(st.ftpAvailable).arg(st.sshAvailable)
                              .arg(st.zstdAvailable)
                              .arg(st.execChannelCompression));
}

}  // namespace ftpflow
