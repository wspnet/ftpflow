// ftpflow -- main window implementation.
#include "mainwindow.hpp"

#include <QAction>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QProgressBar>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "connect_dialog.hpp"
#include "ftpflow/known_hosts.hpp"
#include "ftpflow/protocol_status.hpp"
#include "ftpflow/transfer_journal.hpp"
#include "ftpflow/version.hpp"

#include <fstream>

namespace ftpflow {

namespace {

QString humanSpeed(double bps) {
    if (bps <= 0) return {};
    if (bps < 1024.0) return QString::number(bps, 'f', 0) + " B/s";
    if (bps < 1024.0 * 1024.0)
        return QString::number(bps / 1024.0, 'f', 1) + " KiB/s";
    return QString::number(bps / (1024.0 * 1024.0), 'f', 2) + " MiB/s";
}

std::string readKeyFile(const QString& path) {
    std::ifstream f(path.toStdString(), std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setWindowTitle(QString("ftpflow %1 - 文件传输工作台")
                       .arg(versionString()));

    siteStore_.load();

    queue_ = std::make_unique<TransferQueue>(2, 100);
    workersSpin_->setValue(static_cast<int>(queue_->workerCount()));
    queue_->setEventCallback([this](const TransferEvent& ev) {
        // Worker thread -> UI thread.
        QMetaObject::invokeMethod(
            this, [this, ev] { onTransferEvent(ev); }, Qt::QueuedConnection);
    });

    // Durable journal: resume in-flight transfers after a restart. The
    // journal drops credentials; re-supply them from the saved site.
    queue_->setJournal(TransferJournal::defaultPath());
    {
        std::string terr;
        const std::size_t restored = queue_->restore(&terr,
            [this](TransferRequest& r) {
                if (r.siteName.empty()) return;
                const QString name = QString::fromStdString(r.siteName);
                for (const SiteEntry& e : siteStore_.sites()) {
                    if (e.name != name) continue;
                    const SiteEntry full = siteStore_.withPassword(e);
                    r.ssh.host = e.host.toStdString();
                    r.ssh.port = (std::uint16_t)e.port;
                    r.ssh.user = e.user.toStdString();
                    r.ssh.password = full.password.toStdString();
                    if (!e.keyPath.isEmpty())
                        r.ssh.privateKeyPem = readKeyFile(e.keyPath);
                    r.ftp.host = r.ssh.host;
                    r.ftp.port = r.ssh.port;
                    r.ftp.user = r.ssh.user;
                    r.ftp.password = r.ssh.password;
                    r.pushCommandTemplate = e.pushCmd.toStdString();
                    // Re-supply strict host-key verification from known_hosts.
                    if (r.protocol == BackendKind::Sftp) {
                        KnownHosts kh(KnownHosts::defaultPath());
                        r.trustedHostKeyHex =
                            kh.trustedHexFor(r.ssh.host, r.ssh.port);
                    }
                    break;
                }
            });
        if (restored > 0) {
            statusBar()->showMessage(
                QString("已恢复 %1 个未完成的传输任务").arg(restored), 6000);
        }
        if (!terr.empty()) {
            statusBar()->showMessage(
                QString("传输恢复失败: %1").arg(QString::fromStdString(terr)),
                8000);
        }
    }

    // One-shot reconnect after a transport drop.
    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, [this] {
        RemoteTab* t = currentTab();
        if (!t) return;
        const auto ci = t->model->connInfo();
        t->pathEdit->setText("重连中...");
        t->model->connectTo(ci);
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
    leftTitle->setObjectName("paneTitle");
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
    // Drag & drop: local pane is a drag source (uploads) and drop target
    // (downloads from remote).
    left_->setDragEnabled(true);
    left_->setAcceptDrops(true);
    left_->setDragDropMode(QAbstractItemView::DragDrop);
    left_->setDefaultDropAction(Qt::CopyAction);
    left_->viewport()->installEventFilter(this);
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
    uploadBtn->setProperty("accent", true);
    downloadBtn->setProperty("accent", true);
    uploadBtn->setMinimumWidth(88);
    downloadBtn->setMinimumWidth(88);
    arrowLay->addWidget(uploadBtn);
    arrowLay->addWidget(downloadBtn);
    arrowLay->addStretch(1);
    connect(uploadBtn, &QPushButton::clicked, this,
            &MainWindow::enqueueUploads);
    connect(downloadBtn, &QPushButton::clicked, this,
            &MainWindow::enqueueDownloads);

    // --- remote pane (right): tabbed, one session per tab ------------------
    auto* rightPane = new QWidget(this);
    auto* rightLay = new QVBoxLayout(rightPane);
    rightLay->setContentsMargins(0, 0, 0, 0);
    auto* rightTitle = new QLabel("远端", rightPane);
    rightTitle->setObjectName("paneTitle");
    remoteTabs_ = new QTabWidget(rightPane);
    remoteTabs_->setDocumentMode(true);
    remoteTabs_->setTabsClosable(true);
    remoteTabs_->setMovable(true);
    // "+" button to open another session (XTerminal multi-tab).
    auto* newTabBtn = new QPushButton("＋", remoteTabs_);
    newTabBtn->setFixedSize(24, 24);
    newTabBtn->setToolTip("新建会话标签");
    connect(newTabBtn, &QPushButton::clicked, this,
            &MainWindow::onNewRemoteTab);
    remoteTabs_->setCornerWidget(newTabBtn, Qt::TopRightCorner);
    rightLay->addWidget(rightTitle);
    rightLay->addWidget(remoteTabs_, 1);
    connect(remoteTabs_, &QTabWidget::tabCloseRequested, this,
            &MainWindow::onTabClosed);
    addRemoteTab();   // initial tab

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
    auto* dockBody = new QWidget(transferDock_);
    auto* dockLay = new QVBoxLayout(dockBody);
    dockLay->setContentsMargins(4, 4, 4, 4);
    transferList_ = new QListWidget(dockBody);
    transferList_->setAlternatingRowColors(true);
    transferList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    transferList_->setContextMenuPolicy(Qt::CustomContextMenu);
    dockLay->addWidget(transferList_, 1);

    auto* dockBar = new QWidget(dockBody);
    auto* dockBarLay = new QHBoxLayout(dockBar);
    dockBarLay->setContentsMargins(0, 2, 0, 0);
    pauseBtn_ = new QPushButton("暂停 / 继续", dockBar);
    cancelBtn_ = new QPushButton("取消", dockBar);
    clearDoneBtn_ = new QPushButton("清空已完成", dockBar);
    auto* clearAllBtn = new QPushButton("清空全部", dockBar);
    dockBarLay->addWidget(pauseBtn_);
    dockBarLay->addWidget(cancelBtn_);
    dockBarLay->addWidget(clearDoneBtn_);
    dockBarLay->addWidget(clearAllBtn);
    dockBarLay->addStretch(1);
    // Concurrent worker count (live: applies to the queue immediately).
    dockBarLay->addWidget(new QLabel("并发:", dockBar));
    workersSpin_ = new QSpinBox(dockBar);
    workersSpin_->setRange(1, 64);
    workersSpin_->setValue(2);   // matches TransferQueue(2, ...) below
    workersSpin_->setToolTip(
        "同时传输的 worker 数（1-64）。调小会让多余 worker 跑完当前文件后退出。");
    dockBarLay->addWidget(workersSpin_);
    dockLay->addWidget(dockBar);

    transferDock_->setWidget(dockBody);
    transferDock_->setFeatures(QDockWidget::DockWidgetMovable |
                               QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, transferDock_);
    transferDock_->hide();   // appears with the first job

    connect(pauseBtn_, &QPushButton::clicked, this,
            &MainWindow::onPauseSelected);
    connect(cancelBtn_, &QPushButton::clicked, this,
            &MainWindow::onCancelSelected);
    connect(clearDoneBtn_, &QPushButton::clicked, this,
            &MainWindow::onClearFinished);
    connect(clearAllBtn, &QPushButton::clicked, this,
            &MainWindow::onClearAll);
    connect(workersSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        queue_->setWorkers(static_cast<unsigned>(v));
        statusBar()->showMessage(
            QString("并发 worker 数已设为 %1").arg(v), 3000);
    });
    connect(transferList_, &QListWidget::customContextMenuRequested, this,
            &MainWindow::onTransferContextMenu);

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

// ---- remote tabs -----------------------------------------------------------

MainWindow::RemoteTab* MainWindow::currentTab() {
    const int i = remoteTabs_->currentIndex();
    if (i < 0 || i >= tabs_.size()) return nullptr;
    return tabs_[i];
}

const MainWindow::RemoteTab* MainWindow::currentTab() const {
    const int i = remoteTabs_->currentIndex();
    if (i < 0 || i >= tabs_.size()) return nullptr;
    return tabs_[i];
}

RemoteFsModel* MainWindow::currentRemote() const {
    const RemoteTab* t = currentTab();
    return t ? t->model : nullptr;
}

MainWindow::RemoteTab* MainWindow::addRemoteTab() {
    auto* tab = new RemoteTab;
    auto* page = new QWidget(remoteTabs_);
    auto* pageLay = new QVBoxLayout(page);
    pageLay->setContentsMargins(0, 0, 0, 0);

    // Address bar + nav/connect buttons.
    auto* remoteBar = new QWidget(page);
    auto* remoteBarLay = new QHBoxLayout(remoteBar);
    remoteBarLay->setContentsMargins(2, 0, 2, 0);
    tab->pathEdit = new QLineEdit(remoteBar);
    tab->pathEdit->setPlaceholderText("未连接（点“连接...”打开远端）");
    tab->pathEdit->setReadOnly(true);
    tab->pathEdit->setClearButtonEnabled(true);
    tab->connectBtn = new QPushButton("连接...", remoteBar);
    tab->disconnectBtn = new QPushButton("断开", remoteBar);
    tab->upBtn = new QPushButton("向上", remoteBar);
    tab->refreshBtn = new QPushButton("刷新", remoteBar);
    tab->disconnectBtn->setEnabled(false);
    tab->upBtn->setEnabled(false);
    tab->refreshBtn->setEnabled(false);
    remoteBarLay->addWidget(tab->pathEdit, 1);
    remoteBarLay->addWidget(tab->connectBtn);
    remoteBarLay->addWidget(tab->disconnectBtn);
    remoteBarLay->addWidget(tab->upBtn);
    remoteBarLay->addWidget(tab->refreshBtn);

    // Model/proxy are children of MainWindow (not the page) so they survive
    // independently; onTabClosed() deletes them explicitly.
    tab->model = new RemoteFsModel(this);
    tab->proxy = new QSortFilterProxyModel(this);
    tab->proxy->setSourceModel(tab->model);
    tab->proxy->setSortRole(Qt::DisplayRole);
    tab->table = new QTableView(page);
    tab->table->setModel(tab->proxy);
    tab->table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    tab->table->verticalHeader()->setDefaultSectionSize(22);
    tab->table->setShowGrid(false);
    tab->table->setAlternatingRowColors(true);
    tab->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    tab->table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tab->table->setSortingEnabled(true);
    tab->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tab->table->verticalHeader()->hide();
    tab->table->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    pageLay->addWidget(remoteBar);
    pageLay->addWidget(tab->table, 1);

    // Drag & drop + F2 rename (per tab).
    tab->table->setDragEnabled(true);
    tab->table->setAcceptDrops(true);
    tab->table->setDragDropMode(QAbstractItemView::DragDrop);
    tab->table->setDefaultDropAction(Qt::CopyAction);
    tab->table->viewport()->installEventFilter(this);
    auto* renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), tab->table);
    renameShortcut->setContext(Qt::WidgetShortcut);
    connect(renameShortcut, &QShortcut::activated, this,
            &MainWindow::renameRemoteSelected);

    // Wire this tab's widgets/models. Lambdas capture the stable `tab`
    // pointer so every signal targets the right tab, even a background one.
    connect(tab->pathEdit, &QLineEdit::returnPressed, this, [this, tab] {
        if (!tab->model->isConnected()) return;
        const QString p = tab->pathEdit->text().trimmed();
        if (p.isEmpty()) return;
        tab->model->enterDir(p.toStdString());
    });
    connect(tab->connectBtn, &QPushButton::clicked, this, [this, tab] {
        ConnectDialog dlg(this, &siteStore_);
        if (dlg.exec() != QDialog::Accepted) return;
        tab->connectBtn->setEnabled(false);
        tab->pathEdit->setText("连接中...");
        tab->model->connectTo(dlg.connInfo());
    });
    connect(tab->disconnectBtn, &QPushButton::clicked, this, [this, tab] {
        reconnectTimer_->stop();
        tab->model->disconnect();
        tab->connectBtn->setEnabled(true);
        tab->disconnectBtn->setEnabled(false);
        tab->pathEdit->setReadOnly(true);
        tab->pathEdit->setText("未连接");
    });
    connect(tab->upBtn, &QPushButton::clicked, this, [this, tab] {
        tab->model->goUp();
        tab->pathEdit->setText(QString::fromStdString(tab->model->currentPath()));
    });
    connect(tab->refreshBtn, &QPushButton::clicked, this, [this, tab] {
        tab->model->refresh();
    });
    connect(tab->table, &QTableView::doubleClicked, this, [this, tab](
            const QModelIndex& i) {
        if (!i.isValid() || !tab->model->isConnected()) return;
        const QModelIndex src = tab->proxy->mapToSource(i);
        if (tab->model->isDir(src)) {
            tab->model->enterDir(tab->model->pathAt(src));
            tab->pathEdit->setText(
                QString::fromStdString(tab->model->currentPath()));
        }
    });
    connect(tab->table, &QTableView::customContextMenuRequested, this,
            [this, tab](const QPoint& pos) {
                onRemoteContextMenu(*tab, pos);
            });

    connect(tab->model, &RemoteFsModel::connected, this, [this, tab] {
        reconnectTimer_->stop();
        tab->connectBtn->setEnabled(false);
        tab->disconnectBtn->setEnabled(true);
        tab->upBtn->setEnabled(true);
        tab->refreshBtn->setEnabled(true);
        tab->pathEdit->setReadOnly(false);
        tab->pathEdit->setText(QString::fromStdString(tab->model->currentPath()));
        refreshStatusBar();
    });
    connect(tab->model, &RemoteFsModel::connectFailed, this,
            [this, tab](const QString& msg) {
        tab->connectBtn->setEnabled(true);
        tab->disconnectBtn->setEnabled(false);
        tab->pathEdit->setReadOnly(true);
        tab->pathEdit->setText("未连接");
        statusBar()->showMessage(msg, 8000);
    });
    connect(tab->model, &RemoteFsModel::hostKeyUnknown, this,
            [this, tab](const QString& fp, const QString& host, quint16 port,
                        const QString& keyHex) {
        onHostKeyUnknown(*tab, fp, host, port, keyHex);
    });
    connect(tab->model, &RemoteFsModel::browseFailed, this,
            [this](const QString& msg) { statusBar()->showMessage(msg, 8000); });
    connect(tab->model, &RemoteFsModel::opFailed, this,
            [this](const QString& msg) { statusBar()->showMessage(msg, 8000); });
    connect(tab->model, &RemoteFsModel::connectionLost, this,
            [this, tab](const QString& msg) {
        tab->disconnectBtn->setEnabled(false);
        tab->pathEdit->setReadOnly(true);
        tab->pathEdit->setText("连接已断开，准备重连...");
        statusBar()->showMessage(
            QString("连接已断开：%1（自动重连中...）").arg(msg), 8000);
        reconnectTimer_->start(1500);
    });
    connect(tab->model, &RemoteFsModel::disconnected, this, [this, tab] {
        tab->upBtn->setEnabled(false);
        tab->refreshBtn->setEnabled(false);
        tab->pathEdit->setText("未连接");
    });

    const int idx = tabs_.size();
    tabs_.append(tab);
    remoteTabs_->addTab(page, QString("会话 %1").arg(idx + 1));
    remoteTabs_->setCurrentIndex(idx);
    return tab;
}

void MainWindow::onNewRemoteTab() {
    addRemoteTab();
}

void MainWindow::onTabClosed(int index) {
    if (tabs_.size() <= 1) return;   // keep at least one tab
    RemoteTab* t = tabs_[index];
    tabs_.removeAt(index);
    remoteTabs_->removeTab(index);   // deletes the page + its child widgets
    // model/proxy are MainWindow children (not page children): delete them
    // explicitly. Their destructors stop the worker thread cleanly.
    t->proxy->setSourceModel(nullptr);
    delete t->proxy;
    delete t->model;
    delete t;
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

// ---- remote pane (per-tab handlers) ---------------------------------------

void MainWindow::onHostKeyUnknown(RemoteTab& tab, const QString& fingerprint,
                                  const QString& host, quint16 port,
                                  const QString& keyHex) {
    tab.connectBtn->setEnabled(true);
    tab.disconnectBtn->setEnabled(false);
    tab.pathEdit->setReadOnly(true);
    tab.pathEdit->setText("未连接");
    const QString detail =
        QString("主机 %1:%2 的 SSH 密钥尚未受信任。\n\n"
                "指纹 (SHA256):\n%3\n\n"
                "请确认指纹与服务器一致后信任；若不一致请立即断开，"
                "可能存在中间人攻击。")
            .arg(host)
            .arg(port)
            .arg(fingerprint);
    const auto choice = QMessageBox::warning(
        this, "主机密钥确认", detail,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice == QMessageBox::Yes) {
        tab.model->trustHostKey(host, port, keyHex);
        tab.connectBtn->setEnabled(false);
        tab.pathEdit->setText("连接中...");
    }
}

void MainWindow::onRemoteContextMenu(RemoteTab& tab, const QPoint& pos) {
    if (!tab.model->isConnected()) return;
    const QModelIndex proxyIdx = tab.table->indexAt(pos);
    const QModelIndex idx = proxyIdx.isValid()
        ? tab.proxy->mapToSource(proxyIdx) : QModelIndex();

    QMenu menu(this);
    QAction* mkdirAction = menu.addAction("新建文件夹...");
    QAction* renameAction = menu.addAction("重命名...");
    QAction* removeAction = menu.addAction("删除...");
    menu.addSeparator();
    QAction* refreshAction = menu.addAction("刷新");
    if (!idx.isValid()) {
        renameAction->setEnabled(false);
        removeAction->setEnabled(false);
    }

    QAction* chosen = menu.exec(tab.table->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == mkdirAction) {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "新建文件夹", "文件夹名称:", QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty()) {
            tab.model->makeDir(name.trimmed().toStdString());
        }
    } else if (chosen == renameAction) {
        renameRemoteSelected();
    } else if (chosen == removeAction) {
        QModelIndexList rows = tab.table->selectionModel()->selectedRows(0);
        if (rows.isEmpty()) rows = {proxyIdx};
        QStringList names;
        QModelIndexList srcRows;
        for (const QModelIndex& r : rows) {
            const QModelIndex src = tab.proxy->mapToSource(r);
            if (!src.isValid()) continue;
            srcRows.push_back(src);
            names << QString::fromStdString(tab.model->nameAt(src));
        }
        if (srcRows.isEmpty()) return;
        const QString message = QString(
            "即将从远端删除 %1 个条目（目录将递归删除，不可恢复）：\n\n%2\n\n"
            "确定删除吗？")
            .arg(names.size())
            .arg(names.join('\n').left(400));
        if (QMessageBox::warning(this, "删除确认", message,
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes) {
            tab.model->removeEntries(srcRows);
        }
    } else if (chosen == refreshAction) {
        tab.model->refresh();
    }
}

void MainWindow::renameRemoteSelected() {
    RemoteTab* tab = currentTab();
    if (!tab || !tab->model->isConnected()) return;
    const QModelIndex proxyIdx = tab->table->currentIndex();
    if (!proxyIdx.isValid()) return;
    const QModelIndex idx = tab->proxy->mapToSource(proxyIdx);
    if (!idx.isValid()) return;
    bool ok = false;
    const QString oldName = QString::fromStdString(tab->model->nameAt(idx));
    const QString name = QInputDialog::getText(
        this, "重命名", "新名称:", QLineEdit::Normal, oldName, &ok);
    if (ok && !name.trimmed().isEmpty() && name != oldName) {
        tab->model->renameEntry(idx, name.trimmed().toStdString());
    }
}

// ---- drag & drop -----------------------------------------------------------

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Which remote tab (if any) does this viewport belong to?
    int tabIdx = -1;
    for (int i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->table->viewport() == watched) { tabIdx = i; break; }
    }
    const bool onRemote = tabIdx >= 0;
    const bool onLocal = watched == left_->viewport();

    if (event->type() == QEvent::DragEnter ||
        event->type() == QEvent::DragMove) {
        auto* de = static_cast<QDragEnterEvent*>(event);
        const QMimeData* m = de->mimeData();
        const bool ok =
            (onRemote && m->hasFormat("application/x-ftpflow-local-paths")) ||
            (onLocal && m->hasFormat("application/x-ftpflow-remote-paths"));
        if (ok) {
            de->acceptProposedAction();
            return true;   // swallow; we handle the drop ourselves
        }
        return false;
    }
    if (event->type() == QEvent::Drop) {
        auto* de = static_cast<QDropEvent*>(event);
        const QMimeData* m = de->mimeData();
        if (onRemote &&
            m->hasFormat("application/x-ftpflow-local-paths")) {
            const QStringList paths = QString::fromUtf8(
                m->data("application/x-ftpflow-local-paths"))
                .split('\n', Qt::SkipEmptyParts);
            de->acceptProposedAction();
            remoteTabs_->setCurrentIndex(tabIdx);   // target the dropped tab
            enqueueLocalToRemote(paths);
            return true;
        }
        if (onLocal &&
            m->hasFormat("application/x-ftpflow-remote-paths")) {
            const QStringList paths = QString::fromUtf8(
                m->data("application/x-ftpflow-remote-paths"))
                .split('\n', Qt::SkipEmptyParts);
            de->acceptProposedAction();
            enqueueRemoteToLocal(paths);
            return true;
        }
        return false;
    }
    return QMainWindow::eventFilter(watched, event);
}

// ---- transfer queue -----------------------------------------------------

// Shared helper: build a TransferRequest pre-filled with the current remote
// connection credentials (used by both the buttons and the drop handlers).
namespace {
TransferRequest makeRequest(const RemoteFsModel::ConnInfo& ci) {
    TransferRequest req;
    req.protocol = ci.isSftp ? BackendKind::Sftp : BackendKind::Ftp;
    req.ftp = ci.ftp;
    req.ssh = ci.ssh;
    req.pushCommandTemplate = ci.pushCmdTemplate;
    req.siteName = ci.siteName;
    if (ci.isSftp && !ci.knownHostsPath.empty()) {
        KnownHosts kh(ci.knownHostsPath);
        req.trustedHostKeyHex = kh.trustedHexFor(ci.ssh.host, ci.ssh.port);
    }
    return req;
}
}  // namespace

void MainWindow::enqueueLocalToRemote(const QStringList& localPaths) {
    RemoteFsModel* rm = currentRemote();
    if (!rm || !rm->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    if (localPaths.isEmpty()) return;
    const auto ci = rm->connInfo();
    const std::string remoteDir = rm->currentPath();
    for (const QString& localPath : localPaths) {
        const QFileInfo fi(localPath);
        if (!fi.exists()) continue;
        std::string name = localPath.toStdString();
        const std::size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);

        TransferRequest req = makeRequest(ci);
        req.direction = TransferDirection::Upload;
        req.localPath = localPath.toStdString();
        req.remotePath = (remoteDir.empty() || remoteDir == ".")
            ? name : remoteDir + "/" + name;

        if (fi.isDir()) {
            std::string err;
            queue_->enqueueTree(std::move(req), &err);
            if (!err.empty())
                statusBar()->showMessage(
                    QString::fromStdString("递归上传失败: " + err), 8000);
        } else {
            queue_->enqueue(std::move(req));
        }
    }
    transferDock_->show();
}

void MainWindow::enqueueRemoteToLocal(const QStringList& remotePaths) {
    RemoteFsModel* rm = currentRemote();
    if (!rm || !rm->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    if (remotePaths.isEmpty()) return;
    const auto ci = rm->connInfo();
    const std::string localDir = model_->currentDirFullPath();
    for (const QString& remotePath : remotePaths) {
        // A trailing slash marks a directory (set by the drag source).
        QString rp = remotePath;
        const bool isDir = rp.endsWith('/');
        if (isDir) rp.chop(1);
        const std::string rpath = rp.toStdString();
        const std::size_t slash = rpath.find_last_of('/');
        const std::string name = slash == std::string::npos
            ? rpath : rpath.substr(slash + 1);

        TransferRequest req = makeRequest(ci);
        req.direction = TransferDirection::Download;
        req.remotePath = rpath;
        req.localPath = localDir + "\\" + name;

        if (isDir) {
            std::string err;
            queue_->enqueueTree(std::move(req), &err);
            if (!err.empty())
                statusBar()->showMessage(
                    QString::fromStdString("递归下载失败: " + err), 8000);
        } else {
            queue_->enqueue(std::move(req));
        }
    }
    transferDock_->show();
}

void MainWindow::enqueueUploads() {
    RemoteFsModel* rm = currentRemote();
    if (!rm || !rm->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    const QModelIndexList rows = left_->selectionModel()->selectedRows(0);
    if (rows.isEmpty()) {
        statusBar()->showMessage("未选中本地条目", 4000);
        return;
    }
    QStringList paths;
    for (const QModelIndex& proxyIdx : rows) {
        const QModelIndex src = proxy_->mapToSource(proxyIdx);
        const QString p = model_->pathAt(src);
        if (!p.isEmpty()) paths << p;
    }
    enqueueLocalToRemote(paths);
}

void MainWindow::enqueueDownloads() {
    RemoteFsModel* rm = currentRemote();
    if (!rm || !rm->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    RemoteTab* tab = currentTab();
    const QModelIndexList rows = tab->table->selectionModel()->selectedRows(0);
    if (rows.isEmpty()) {
        statusBar()->showMessage("未选中远端条目", 4000);
        return;
    }
    QStringList paths;
    for (const QModelIndex& proxyIdx : rows) {
        const QModelIndex src = tab->proxy->mapToSource(proxyIdx);
        std::string p = rm->pathAt(src);
        if (p.empty()) continue;
        if (rm->isDir(src)) p += "/";
        paths << QString::fromStdString(p);
    }
    enqueueRemoteToLocal(paths);
}

void MainWindow::onTransferEvent(const TransferEvent& ev) {
    auto it = transferItems_.find(ev.jobId);
    if (it == transferItems_.end()) {
        auto* item = new QListWidgetItem;
        item->setData(Qt::UserRole, (qulonglong)ev.jobId);
        transferList_->addItem(item);
        it = transferItems_.emplace(ev.jobId, item).first;
        // Give the row a progress bar.
        auto* bar = new QProgressBar;
        bar->setRange(0, 1000);
        bar->setValue(0);
        bar->setTextVisible(true);
        bar->setFixedHeight(18);
        transferList_->setItemWidget(item, bar);

        JobUi ui;
        ui.jobId = ev.jobId;
        ui.direction = ev.direction == TransferDirection::Upload ? "上传"
                                                                 : "下载";
        ui.mode = ev.mode;
        jobUi_[ev.jobId] = ui;
    }
    refreshTransferItem(ev.jobId, &ev);

    // Uploads may have changed the remote directory: refresh on finish.
    if (ev.kind == TransferEvent::Kind::Finished) {
        RemoteFsModel* rm = currentRemote();
        if (rm && rm->isConnected()) rm->refresh();
    }
}

void MainWindow::refreshTransferItem(std::uint64_t jobId,
                                     const TransferEvent* ev) {
    auto it = transferItems_.find(jobId);
    if (it == transferItems_.end()) return;
    auto uiIt = jobUi_.find(jobId);
    if (uiIt == jobUi_.end()) return;
    JobUi& ui = uiIt->second;

    if (ev) {
        // Progress carries the effective mode and completion.
        ui.mode = ev->mode;
        ui.pct = ev->progress.bytesTotal > 0
            ? 100.0 * (double)ev->progress.bytesMoved /
                  (double)ev->progress.bytesTotal
            : 0.0;
        if (ev->kind == TransferEvent::Kind::Started) {
            ui.state = TransferState::Running;
            ui.name = ev->direction == TransferDirection::Upload
                ? QString::fromStdString(ev->localPath)
                : QString::fromStdString(ev->remotePath);
            const int slash = ui.name.lastIndexOf(QChar('/'));
            const int bslash = ui.name.lastIndexOf(QChar('\\'));
            const int cut = qMax(slash, bslash);
            if (cut >= 0) ui.name = ui.name.mid(cut + 1);
        }
        if (ev->kind == TransferEvent::Kind::Finished) {
            ui.state = ev->state;
            ui.paused = false;
        }
    }

    // Compose the title.
    QString text;
    const QString modeStr = ui.mode == TransferMode::Compress
        ? "压缩" : ui.mode == TransferMode::Ftz1 ? "FTZ" : "直传";
    if (ui.state == TransferState::Queued) {
        text = QString("[job %1] %2 · %3 · 排队中")
                   .arg((unsigned long long)jobId).arg(ui.name).arg(modeStr);
    } else if (ui.state == TransferState::Running) {
        if (ui.paused) {
            text = QString("[job %1] %2 · %3 · 已暂停 (%4%)")
                       .arg((unsigned long long)jobId).arg(ui.name)
                       .arg(modeStr).arg(ui.pct, 0, 'f', 1);
        } else {
            text = QString("[job %1] %2 · %3 · %4%")
                       .arg((unsigned long long)jobId).arg(ui.name)
                       .arg(modeStr).arg(ui.pct, 0, 'f', 1);
            if (ev && (ev->kind == TransferEvent::Kind::Progress ||
                       ev->kind == TransferEvent::Kind::Finished)) {
                const QString spd = humanSpeed(ev->progress.speedBytesPerSec);
                if (!spd.isEmpty())
                    text += "  " + spd;
            }
        }
    } else if (ui.state == TransferState::Done) {
        text = QString("[job %1] %2 · %3 · ✓ 完成")
                   .arg((unsigned long long)jobId).arg(ui.name).arg(modeStr);
    } else if (ui.state == TransferState::Failed) {
        text = QString("[job %1] %2 · ✗ %3")
                   .arg((unsigned long long)jobId).arg(ui.name)
                   .arg(ev ? QString::fromStdString(ev->error) : QString());
    } else if (ui.state == TransferState::Canceled) {
        text = QString("[job %1] %2 · 已取消")
                   .arg((unsigned long long)jobId).arg(ui.name);
    }
    it->second->setText(text);

    // Progress bar: color by direction/state, text shows percent.
    if (auto* bar = qobject_cast<QProgressBar*>(
            transferList_->itemWidget(it->second))) {
        bar->setValue((int)(ui.pct * 10.0));
        bar->setFormat(QString("%1%").arg(ui.pct, 0, 'f', 0));
        // Upload = teal, download = blue, done = green, fail = red,
        // paused = gray.
        QString chunk;
        if (ui.state == TransferState::Failed) chunk = "#f48771";
        else if (ui.state == TransferState::Done) chunk = "#4ec9b0";
        else if (ui.paused) chunk = "#9d9d9d";
        else if (ui.state == TransferState::Canceled) chunk = "#6a6a6a";
        else if (ui.direction == "下载") chunk = "#569cd6";
        else chunk = "#2ea8c0";
        bar->setStyleSheet(
            QString("QProgressBar::chunk { background-color: %1; }")
                .arg(chunk));
    }
}

// ---- transfer dock interactions -----------------------------------------

namespace {
// Collects the job ids of the currently selected transfer rows.
std::vector<std::uint64_t> selectedJobIds(QListWidget* list) {
    std::vector<std::uint64_t> ids;
    for (QListWidgetItem* item : list->selectedItems()) {
        ids.push_back((std::uint64_t)item->data(Qt::UserRole).toULongLong());
    }
    return ids;
}
}  // namespace

void MainWindow::onTransferContextMenu(const QPoint& pos) {
    const QModelIndex idx = transferList_->indexAt(pos);
    if (!idx.isValid()) return;

    QMenu menu(this);
    QAction* pauseAction = menu.addAction("暂停 / 继续");
    QAction* cancelAction = menu.addAction("取消");
    menu.addSeparator();
    QAction* clearAction = menu.addAction("清空已完成");
    QAction* clearAllAction = menu.addAction("清空全部");

    QAction* chosen =
        menu.exec(transferList_->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == pauseAction) onPauseSelected();
    else if (chosen == cancelAction) onCancelSelected();
    else if (chosen == clearAction) onClearFinished();
    else if (chosen == clearAllAction) onClearAll();
}

void MainWindow::onPauseSelected() {
    const auto ids = selectedJobIds(transferList_);
    if (ids.empty()) {
        statusBar()->showMessage("未选中传输任务", 4000);
        return;
    }
    for (std::uint64_t id : ids) {
        auto uiIt = jobUi_.find(id);
        if (uiIt == jobUi_.end()) continue;
        if (uiIt->second.state == TransferState::Running ||
            uiIt->second.state == TransferState::Queued) {
            if (queue_->isPaused(id)) {
                queue_->resume(id);
                uiIt->second.paused = false;
            } else {
                queue_->pause(id);
                uiIt->second.paused = true;
            }
            refreshTransferItem(id, nullptr);
        }
    }
}

void MainWindow::onCancelSelected() {
    const auto ids = selectedJobIds(transferList_);
    if (ids.empty()) {
        statusBar()->showMessage("未选中传输任务", 4000);
        return;
    }
    for (std::uint64_t id : ids) {
        queue_->cancel(id);
    }
}

void MainWindow::onClearFinished() {
    // Remove rows that have reached a terminal (non-running) state.
    std::vector<std::uint64_t> toRemove;
    for (auto& [id, item] : transferItems_) {
        auto uiIt = jobUi_.find(id);
        if (uiIt == jobUi_.end()) continue;
        const TransferState s = uiIt->second.state;
        if (s == TransferState::Done || s == TransferState::Failed ||
            s == TransferState::Canceled) {
            toRemove.push_back(id);
        }
    }
    for (std::uint64_t id : toRemove) {
        delete transferItems_[id];
        transferItems_.erase(id);
        jobUi_.erase(id);
    }
    if (transferItems_.empty()) transferDock_->hide();
}

void MainWindow::onClearAll() {
    // Cancel everything still pending/running, then drop every row.
    for (auto& [id, item] : transferItems_) {
        (void)item;
        queue_->cancel(id);
    }
    for (auto& [id, item] : transferItems_) {
        (void)id;
        delete item;
    }
    transferItems_.clear();
    jobUi_.clear();
    transferDock_->hide();
}

void MainWindow::refreshStatusBar() {
    const std::string rel = model_->currentDir();
    const QString where = rel.empty()
        ? QString("(root)")
        : QString::fromStdString(rel);
    const auto st = protocolEngineStatus();
    RemoteFsModel* rm = currentRemote();
    QString remote = (rm && rm->isConnected())
        ? QString::fromStdString(rm->currentPath())
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
