// ftpflow -- remote connection dialog implementation.
#include "connect_dialog.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <cstdio>
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

ConnectDialog::ConnectDialog(QWidget* parent) : QDialog(parent) {
    buildUi();
}

void ConnectDialog::buildUi() {
    setWindowTitle("连接远端");
    setMinimumWidth(460);

    auto* form = new QFormLayout;

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
    return ci;
}

}  // namespace ftpflow
