// ftpflow -- remote connection dialog implementation.
#include "connect_dialog.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

}  // namespace

ConnectDialog::ConnectDialog(QWidget* parent, SiteStore* store)
    : QDialog(parent), store_(store) {
    buildUi();
}

void ConnectDialog::buildUi() {
    setWindowTitle("连接远端");
    setMinimumWidth(460);

    auto* form = new QFormLayout;

    if (store_) {
        site_ = new QComboBox(this);
        site_->addItem("-- 手动输入 --");
        for (const SiteEntry& e : store_->sites()) {
            site_->addItem(e.name);
        }
        saveSiteBtn_ = new QPushButton("保存为站点...", this);
        auto* siteRow = new QWidget(this);
        auto* siteLay = new QHBoxLayout(siteRow);
        siteLay->setContentsMargins(0, 0, 0, 0);
        siteLay->addWidget(site_, 1);
        siteLay->addWidget(saveSiteBtn_);
        form->addRow("站点:", siteRow);
        connect(site_, &QComboBox::currentIndexChanged, this,
                &ConnectDialog::onSiteChosen);
        connect(saveSiteBtn_, &QPushButton::clicked, this,
                &ConnectDialog::onSaveSite);
    }

    proto_ = new QComboBox(this);
    proto_->addItem("SFTP (SSH)");
    proto_->addItem("FTP");
    form->addRow("协议:", proto_);

    host_ = new QLineEdit(this);
    host_->setPlaceholderText("主机名或 IP");
    form->addRow("主机:", host_);

    port_ = new QLineEdit(this);
    port_->setValidator(new QIntValidator(1, 65535, this));
    form->addRow("端口:", port_);

    user_ = new QLineEdit(this);
    form->addRow("用户名:", user_);

    password_ = new QLineEdit(this);
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText("SFTP 可留空改用密钥");
    form->addRow("密码:", password_);

    keyPath_ = new QLineEdit(this);
    keyPath_->setPlaceholderText("PEM 私钥路径（可选，SFTP）");
    browseKeyBtn_ = new QPushButton("浏览...", this);
    auto* keyRow = new QWidget(this);
    auto* keyLay = new QHBoxLayout(keyRow);
    keyLay->setContentsMargins(0, 0, 0, 0);
    keyLay->addWidget(keyPath_, 1);
    keyLay->addWidget(browseKeyBtn_);
    form->addRow("私钥:", keyRow);

    startDir_ = new QLineEdit(this);
    startDir_->setPlaceholderText("远端起始目录（SFTP 默认 \".\"，FTP 默认 \"/\"）");
    form->addRow("起始目录:", startDir_);

    pushCmd_ = new QLineEdit(this);
    pushCmd_->setPlaceholderText(
        "远端解压命令模板，{file}=目标路径；默认 \"zstd -d -c - -o {file}\"");
    form->addRow("解压命令:", pushCmd_);

    auto* ok = new QPushButton("连接", this);
    auto* cancel = new QPushButton("取消", this);
    ok->setDefault(true);
    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    btns->addWidget(ok);
    btns->addWidget(cancel);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addLayout(btns);

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

    onProtocolChanged(0);
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

void ConnectDialog::onSiteChosen(int index) {
    if (!store_ || index <= 0) return;   // 0 = manual entry
    const QString name = site_->itemText(index);
    for (const SiteEntry& e : store_->sites()) {
        if (e.name != name) continue;
        loadSite(store_->withPassword(e));
        return;
    }
}

void ConnectDialog::onSaveSite() {
    if (!store_) return;
    if (host_->text().trimmed().isEmpty() ||
        user_->text().trimmed().isEmpty()) {
        host_->setFocus();
        return;
    }
    bool ok = false;
    QString name = site_->currentIndex() > 0
        ? site_->currentText()
        : host_->text().trimmed();
    name = QInputDialog::getText(this, "保存站点", "站点名称:", QLineEdit::Normal,
                                 name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    SiteEntry e;
    e.name = name;
    e.isSftp = proto_->currentIndex() == 0;
    e.host = host_->text().trimmed();
    e.port = port_->text().toInt();
    e.user = user_->text().trimmed();
    e.keyPath = keyPath_->text().trimmed();
    e.startDir = startDir_->text().trimmed();
    e.pushCmd = pushCmd_->text().trimmed();
    store_->upsert(e);
    if (store_->save() && !password_->text().isEmpty()) {
        // Password round-trips through the Windows Credential Manager;
        // a write failure only means it will not auto-fill next time.
        SiteStore::writePassword(name, password_->text());
    }
    // Refresh the combo with the new entry and keep it selected.
    const int cur = site_->currentIndex();
    site_->blockSignals(true);
    site_->clear();
    site_->addItem("-- 手动输入 --");
    for (const SiteEntry& s : store_->sites()) site_->addItem(s.name);
    const int idx = site_->findText(name);
    site_->setCurrentIndex(idx > 0 ? idx : cur);
    site_->blockSignals(false);
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
    if (site_ && site_->currentIndex() > 0)
        ci.siteName = site_->currentText().toStdString();
    // Strict host-key verification against the persistent known_hosts store.
    ci.knownHostsPath = ftpflow::KnownHosts::defaultPath();
    return ci;
}

}  // namespace ftpflow
