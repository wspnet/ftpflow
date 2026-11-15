// ftpflow -- main window: dual-pane workbench, local pane fully wired.
#include "mainwindow.hpp"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableView>
#include <QVBoxLayout>
#include <QWidget>

#include "ftpflow/protocol_status.hpp"
#include "ftpflow/version.hpp"

namespace ftpflow {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setWindowTitle(QString("ftpflow %1 - file transfer workbench")
                       .arg(versionString()));
}

void MainWindow::buildUi() {
    // --- top bar: path input + open + up -------------------------------
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

    // --- dual pane -------------------------------------------------------
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // Left: local filesystem table.
    auto* leftPane = new QWidget(splitter);
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
    // Uniform fixed rows: keeps the 100k-row paint budget honest.
    left_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    left_->verticalHeader()->setDefaultSectionSize(22);
    left_->setShowGrid(false);
    left_->setAlternatingRowColors(true);
    left_->setSelectionBehavior(QAbstractItemView::SelectRows);
    left_->setSortingEnabled(true);
    left_->horizontalHeader()->setStretchLastSection(false);
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

    // Right: remote placeholder until the FTP/SFTP backends land (M1).
    right_ = new QStackedWidget(splitter);
    auto* remotePlaceholder = new QLabel(
        "远端（M1：FTP / SFTP 连接面板）", right_);
    remotePlaceholder->setAlignment(Qt::AlignCenter);
    right_->addWidget(remotePlaceholder);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    auto* central = new QWidget(this);
    auto* centralLay = new QVBoxLayout(central);
    centralLay->setContentsMargins(0, 0, 0, 0);
    centralLay->setSpacing(0);
    centralLay->addWidget(top);
    centralLay->addWidget(splitter, 1);
    setCentralWidget(central);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);

    // Engines summary so the user sees what is linked at a glance.
    const auto st = protocolEngineStatus();
    statusLabel_->setText(QString("engines: ftp=%1 ssh=%2 zstd=%3 "
                                  "exec-compression=%4")
                              .arg(st.ftpAvailable).arg(st.sshAvailable)
                              .arg(st.zstdAvailable)
                              .arg(st.execChannelCompression));

    // Default: open the user's home directory.
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

void MainWindow::refreshStatusBar() {
    // Show the current dir relative to the bound root + row count.
    const std::string rel = model_->currentDir();
    const QString where = rel.empty()
        ? QString("(root)")
        : QString::fromStdString(rel);
    const auto st = protocolEngineStatus();
    statusLabel_->setText(QString("%1 | %2 行 | engines: ftp=%3 ssh=%4 "
                                  "zstd=%5 exec=%6")
                              .arg(where)
                              .arg(model_->rowCount({}))
                              .arg(st.ftpAvailable).arg(st.sshAvailable)
                              .arg(st.zstdAvailable)
                              .arg(st.execChannelCompression));
}

}  // namespace ftpflow
