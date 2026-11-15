// ftpflow -- remote connection dialog implementation.
#include "connect_dialog.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstdio>

#include "ftpflow/known_hosts.hpp"
#include <fstream>

namespace ftpflow {
namespace {

std::string readWholeFileStr(const QString& path) {
    std::ifstream f(path.toStdString(), std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

// Tree item roles carrying the selected site name.
constexpr int kSiteNameRole = Qt::UserRole + 1;
constexpr int kIsGroupRole = Qt::UserRole + 2;

}  // namespace

ConnectDialog::ConnectDialog(QWidget* parent, SiteStore* store)
    : QDialog(parent), store_(store) {
    buildUi();
}

void ConnectDialog::buildUi() {
    setWindowTitle("连接远端");
    setMinimumWidth(760);
    setMinimumHeight(520);

    // ---- left column: searchable site tree (XTerminal-style grouping) ----
    auto* left = new QWidget(this);
    auto* leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);

    filter_ = new QLineEdit(left);
    filter_->setPlaceholderText("搜索站点（按名称/主机过滤）...");
    filter_->setClearButtonEnabled(true);

    tree_ = new QTreeWidget(left);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);

    auto* siteTools = new QHBoxLayout;
    auto* exportBtn = new QPushButton("导出", left);
    auto* importBtn = new QPushButton("导入", left);
    deleteSiteBtn_ = new QPushButton("删除站点", left);
    deleteSiteBtn_->setProperty("danger", true);
    siteTools->addWidget(exportBtn);
    siteTools->addWidget(importBtn);
    siteTools->addStretch(1);
    siteTools->addWidget(deleteSiteBtn_);

    leftLay->addWidget(filter_);
    leftLay->addWidget(tree_, 1);
    leftLay->addLayout(siteTools);

    // ---- right column: manual connection form ----------------------------
    auto* right = new QWidget(this);
    auto* form = new QFormLayout(right);
    form->setContentsMargins(12, 0, 0, 0);

    proto_ = new QComboBox(right);
    proto_->addItem("SFTP (SSH)");
    proto_->addItem("FTP");
    form->addRow("协议:", proto_);

    host_ = new QLineEdit(right);
    host_->setPlaceholderText("主机名或 IP");
    form->addRow("主机:", host_);

    port_ = new QLineEdit(right);
    port_->setValidator(new QIntValidator(1, 65535, right));
    form->addRow("端口:", port_);

    user_ = new QLineEdit(right);
    form->addRow("用户名:", user_);

    password_ = new QLineEdit(right);
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText("SFTP 可留空改用密钥");
    form->addRow("密码:", password_);

    keyPath_ = new QLineEdit(right);
    keyPath_->setPlaceholderText("PEM 私钥路径（可选，SFTP）");
    browseKeyBtn_ = new QPushButton("浏览...", right);
    auto* keyRow = new QWidget(right);
    auto* keyLay = new QHBoxLayout(keyRow);
    keyLay->setContentsMargins(0, 0, 0, 0);
    keyLay->addWidget(keyPath_, 1);
    keyLay->addWidget(browseKeyBtn_);
    form->addRow("私钥:", keyRow);

    startDir_ = new QLineEdit(right);
    startDir_->setPlaceholderText("远端起始目录（SFTP 默认 \".\"，FTP 默认 \"/\"）");
    form->addRow("起始目录:", startDir_);

    pushCmd_ = new QLineEdit(right);
    pushCmd_->setPlaceholderText(
        "远端解压命令模板，{file}=目标路径；默认 \"zstd -d -c - -o {file}\"");
    form->addRow("解压命令:", pushCmd_);

    group_ = new QLineEdit(right);
    group_->setPlaceholderText("分组名（如 \"生产\" / \"测试\"，可留空）");
    form->addRow("分组:", group_);

    auto* ok = new QPushButton("连接", right);
    ok->setProperty("accent", true);
    auto* cancel = new QPushButton("取消", right);
    ok->setDefault(true);
    saveSiteBtn_ = new QPushButton("保存为站点...", right);
    auto* btns = new QHBoxLayout;
    btns->addWidget(saveSiteBtn_);
    btns->addStretch(1);
    btns->addWidget(ok);
    btns->addWidget(cancel);

    // ---- assemble split layout -------------------------------------------
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({260, 500});

    auto* root = new QVBoxLayout(this);
    root->addWidget(splitter);
    root->addLayout(btns);

    // ---- wiring ----------------------------------------------------------
    connect(filter_, &QLineEdit::textChanged, this,
            &ConnectDialog::onFilterChanged);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            &ConnectDialog::onSiteChosen);
    connect(saveSiteBtn_, &QPushButton::clicked, this,
            &ConnectDialog::onSaveSite);
    connect(deleteSiteBtn_, &QPushButton::clicked, this,
            &ConnectDialog::onDeleteSite);
    connect(exportBtn, &QPushButton::clicked, this,
            &ConnectDialog::onExportSites);
    connect(importBtn, &QPushButton::clicked, this,
            &ConnectDialog::onImportSites);
    connect(ok, &QPushButton::clicked, this, [this] {
        if (host_->text().trimmed().isEmpty() ||
            user_->text().trimmed().isEmpty()) {
            host_->setFocus();
            return;
        }
        accept();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(proto_, &QComboBox::currentIndexChanged, this,
            &ConnectDialog::onProtocolChanged);
    connect(browseKeyBtn_, &QPushButton::clicked, this,
            &ConnectDialog::onBrowseKey);

    rebuildSiteTree();
    onProtocolChanged(0);
}

void ConnectDialog::rebuildSiteTree(const QString& filter) {
    if (!store_ || !tree_) return;
    tree_->clear();

    const QString needle = filter.trimmed();
    auto matches = [&](const SiteEntry& e) {
        if (needle.isEmpty()) return true;
        return e.name.contains(needle, Qt::CaseInsensitive) ||
               e.host.contains(needle, Qt::CaseInsensitive) ||
               e.user.contains(needle, Qt::CaseInsensitive);
    };

    // Group top-level nodes first, then an "未分组" node for the rest.
    for (const QString& g : store_->groups()) {
        QTreeWidgetItem* groupNode = nullptr;
        for (const SiteEntry& e : store_->sitesInGroup(g)) {
            if (!matches(e)) continue;
            if (!groupNode) {
                groupNode = new QTreeWidgetItem(tree_);
                groupNode->setText(0, g);
                groupNode->setData(0, kIsGroupRole, true);
                groupNode->setFlags(groupNode->flags() & ~Qt::ItemIsSelectable);
            }
            auto* item = new QTreeWidgetItem(groupNode);
            item->setText(0, e.name);
            item->setToolTip(0, QString("%1@%2:%3")
                .arg(e.user, e.host).arg(e.port));
            item->setData(0, kSiteNameRole, e.name);
            item->setData(0, kIsGroupRole, false);
        }
        if (groupNode) groupNode->setExpanded(true);
    }
    // Ungrouped sites.
    QTreeWidgetItem* ungrouped = nullptr;
    for (const SiteEntry& e : store_->sitesInGroup(QString())) {
        if (!matches(e)) continue;
        if (!ungrouped) {
            ungrouped = new QTreeWidgetItem(tree_);
            ungrouped->setText(0, "未分组");
            ungrouped->setData(0, kIsGroupRole, true);
            ungrouped->setFlags(ungrouped->flags() & ~Qt::ItemIsSelectable);
        }
        auto* item = new QTreeWidgetItem(ungrouped);
        item->setText(0, e.name);
        item->setToolTip(0, QString("%1@%2:%3")
            .arg(e.user, e.host).arg(e.port));
        item->setData(0, kSiteNameRole, e.name);
        item->setData(0, kIsGroupRole, false);
    }
    if (ungrouped) ungrouped->setExpanded(true);
    // When filtering, expand everything so matches are visible.
    if (!needle.isEmpty()) tree_->expandAll();
}

void ConnectDialog::onFilterChanged(const QString& text) {
    rebuildSiteTree(text);
}

void ConnectDialog::onSiteChosen(QTreeWidgetItem* current,
                                 QTreeWidgetItem* /*previous*/) {
    if (!store_ || !current) return;
    if (current->data(0, kIsGroupRole).toBool()) return;
    const QString name = current->data(0, kSiteNameRole).toString();
    for (const SiteEntry& e : store_->sites()) {
        if (e.name != name) continue;
        loadSite(store_->withPassword(e));
        return;
    }
}

void ConnectDialog::onProtocolChanged(int index) {
    const bool sftp = index == 0;
    port_->setText(sftp ? "22" : "21");
    keyPath_->setEnabled(sftp);
    browseKeyBtn_->setEnabled(sftp);
    pushCmd_->setEnabled(sftp);
    if (sftp && startDir_->text().isEmpty()) startDir_->setText(".");
    if (!sftp && startDir_->text() == ".") startDir_->setText("/");
}

void ConnectDialog::onBrowseKey() {
    const QString path = QFileDialog::getOpenFileName(
        this, "选择 PEM 私钥", {}, "密钥文件 (*)");
    if (!path.isEmpty()) keyPath_->setText(path);
}

void ConnectDialog::onSaveSite() {
    if (!store_) return;
    if (host_->text().trimmed().isEmpty() ||
        user_->text().trimmed().isEmpty()) {
        host_->setFocus();
        return;
    }
    bool ok = false;
    const QString suggested = host_->text().trimmed();
    const QString name = QInputDialog::getText(
        this, "保存站点", "站点名称:", QLineEdit::Normal, suggested, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    SiteEntry e;
    e.name = name.trimmed();
    e.isSftp = proto_->currentIndex() == 0;
    e.host = host_->text().trimmed();
    e.port = port_->text().toInt();
    e.user = user_->text().trimmed();
    e.keyPath = keyPath_->text().trimmed();
    e.startDir = startDir_->text().trimmed();
    e.pushCmd = pushCmd_->text().trimmed();
    e.group = group_->text().trimmed();
    store_->upsert(e);
    if (store_->save() && !password_->text().isEmpty()) {
        SiteStore::writePassword(e.name, password_->text());
    }
    rebuildSiteTree(filter_ ? filter_->text() : QString());
}

void ConnectDialog::onDeleteSite() {
    if (!store_ || !tree_) return;
    const auto* cur = tree_->currentItem();
    if (!cur || cur->data(0, kIsGroupRole).toBool()) return;
    const QString name = cur->data(0, kSiteNameRole).toString();
    const auto ans = QMessageBox::question(
        this, "删除站点",
        QString("确定删除站点 \"%1\" 吗？（密码凭证也会一并删除）").arg(name));
    if (ans != QMessageBox::Yes) return;
    store_->remove(name);
    SiteStore::erasePassword(name);
    store_->save();
    rebuildSiteTree(filter_ ? filter_->text() : QString());
}

void ConnectDialog::onExportSites() {
    if (!store_) return;
    const QString path = QFileDialog::getSaveFileName(
        this, "导出站点", "ftpflow-sites.json", "JSON (*.json)");
    if (path.isEmpty()) return;
    QString err;
    if (store_->exportTo(path, &err)) {
        QMessageBox::information(this, "导出成功",
            QString("已导出 %1 个站点到 %2（密码不包含在导出文件中）")
                .arg(store_->sites().size()).arg(path));
    } else {
        QMessageBox::warning(this, "导出失败", err);
    }
}

void ConnectDialog::onImportSites() {
    if (!store_) return;
    const QString path = QFileDialog::getOpenFileName(
        this, "导入站点", {}, "JSON (*.json)");
    if (path.isEmpty()) return;
    QString err;
    if (store_->importFrom(path, &err)) {
        store_->save();
        rebuildSiteTree(filter_ ? filter_->text() : QString());
        QMessageBox::information(this, "导入成功",
            QString("已导入站点（同名站点已覆盖），共 %1 个").arg(store_->sites().size()));
    } else {
        QMessageBox::warning(this, "导入失败", err);
    }
}

void ConnectDialog::loadSite(const SiteEntry& e) {
    proto_->setCurrentIndex(e.isSftp ? 0 : 1);
    host_->setText(e.host);
    port_->setText(QString::number(e.port));
    user_->setText(e.user);
    password_->setText(e.password);
    keyPath_->setText(e.keyPath);
    startDir_->setText(e.startDir);
    pushCmd_->setText(e.pushCmd);
    group_->setText(e.group);
}

RemoteFsModel::ConnInfo ConnectDialog::connInfo() const {
    RemoteFsModel::ConnInfo ci;
    ci.isSftp = proto_->currentIndex() == 0;
    ci.ftp.host = host_->text().trimmed().toStdString();
    ci.ftp.port = static_cast<std::uint16_t>(port_->text().toInt());
    ci.ftp.user = user_->text().trimmed().toStdString();
    ci.ftp.password = password_->text().toStdString();
    ci.ftp.initialPath =
        startDir_->text().isEmpty() ? "/" : startDir_->text().toStdString();

    ci.ssh.host = ci.ftp.host;
    ci.ssh.port = ci.ftp.port;
    ci.ssh.user = ci.ftp.user;
    ci.ssh.password = ci.ftp.password;
    if (!keyPath_->text().trimmed().isEmpty()) {
        ci.ssh.privateKeyPem =
            readWholeFileStr(keyPath_->text().trimmed());
    }
    ci.startDir = startDir_->text().isEmpty()
        ? (ci.isSftp ? "." : "/")
        : startDir_->text().toStdString();
    ci.pushCmdTemplate = pushCmd_->text().toStdString();
    // Saved-site reference for credential recovery on restore.
    if (tree_ && tree_->currentItem() &&
        !tree_->currentItem()->data(0, kIsGroupRole).toBool()) {
        ci.siteName = tree_->currentItem()->data(0, kSiteNameRole)
                          .toString().toStdString();
    }
    // Strict host-key verification against the persistent known_hosts store.
    ci.knownHostsPath = ftpflow::KnownHosts::defaultPath();
    return ci;
}

}  // namespace ftpflow
