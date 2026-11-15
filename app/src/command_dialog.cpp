// ftpflow -- server command panel.
#include "command_dialog.hpp"

#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

#include "remote_command.hpp"

namespace ftpflow {

CommandDialog::CommandDialog(const RemoteFsModel::ConnInfo& ci, QWidget* parent)
    : QDialog(parent), conn_(ci) {
    setWindowTitle(QString("服务器命令 - %1")
                       .arg(QString::fromStdString(conn_.ssh.host)));
    resize(720, 480);

    auto* lay = new QVBoxLayout(this);

    auto* hint = new QLabel(
        QString("在 %1@%2:%3 上执行命令（复用当前 SFTP 连接）")
            .arg(QString::fromStdString(conn_.ssh.user))
            .arg(QString::fromStdString(conn_.ssh.host))
            .arg(conn_.ssh.port),
        this);
    hint->setObjectName("paneTitle");
    lay->addWidget(hint);

    // Output area.
    output_ = new QPlainTextEdit(this);
    output_->setReadOnly(true);
    output_->setPlaceholderText("命令输出将显示在这里...");
    lay->addWidget(output_, 1);

    // Input row.
    auto* inputRow = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setPlaceholderText("输入 shell 命令，回车执行（如 ls -la /tmp）");
    runBtn_ = new QPushButton("执行", this);
    runBtn_->setProperty("accent", true);
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(runBtn_);
    lay->addLayout(inputRow);

    status_ = new QLabel("就绪", this);
    lay->addWidget(status_);

    connect(runBtn_, &QPushButton::clicked, this, &CommandDialog::onRun);
    connect(input_, &QLineEdit::returnPressed, this, &CommandDialog::onRun);
    // Up/down browse command history.
    connect(input_, &QLineEdit::textEdited, this, [this](const QString&) {
        historyPos_ = -1;
    });

    input_->setFocus();
}

void CommandDialog::onRun() {
    const QString cmd = input_->text().trimmed();
    if (cmd.isEmpty()) return;
    if (cmd_ && cmd_->isRunning()) return;  // one command at a time

    // Record history (skip consecutive duplicates).
    if (history_.isEmpty() || history_.last() != cmd) history_.append(cmd);
    historyPos_ = -1;

    appendOutput(QString("$ %1\n").arg(cmd));
    status_->setText("执行中...");
    runBtn_->setEnabled(false);

    cmd_ = new RemoteCommand(conn_, this);
    connect(cmd_, &RemoteCommand::output, this, &CommandDialog::onOutput);
    connect(cmd_, &RemoteCommand::finished, this, &CommandDialog::onFinished);
    cmd_->start(cmd);
}

void CommandDialog::onOutput(const QString& text) {
    appendOutput(text);
}

void CommandDialog::onFinished(int exitCode, const QString& message) {
    runBtn_->setEnabled(true);
    if (!message.isEmpty()) {
        appendOutput("\n" + message + "\n", true);
        status_->setText("执行失败");
    } else {
        appendOutput(QString("\n[退出码: %1]\n\n").arg(exitCode));
        status_->setText(exitCode == 0 ? "执行成功"
                                       : QString("完成（退出码 %1）").arg(exitCode));
    }
    cmd_->deleteLater();
    cmd_ = nullptr;
    input_->setFocus();
}

void CommandDialog::appendOutput(const QString& text, bool isError) {
    QTextCursor c = output_->textCursor();
    c.movePosition(QTextCursor::End);
    if (isError) {
        QTextCharFormat fmt;
        fmt.setForeground(QColor(0xf4, 0x87, 0x71));  // danger
        c.insertText(text, fmt);
    } else {
        c.insertText(text);
    }
    output_->setTextCursor(c);
    output_->ensureCursorVisible();
}

}  // namespace ftpflow
