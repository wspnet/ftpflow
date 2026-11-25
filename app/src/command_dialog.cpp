// ftpflow -- server command panel.
#include "command_dialog.hpp"

#include "dialog_title_bar.hpp"

#include <QColor>
#include <QHBoxLayout>
#include <QKeyEvent>
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
    const QString title = QString("服务器命令 - %1")
                              .arg(QString::fromStdString(conn_.ssh.host));
    // Frameless dialog with a custom title bar matching the main window.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setWindowTitle(title);
    resize(720, 480);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* titleBar = new DialogTitleBar(this);
    titleBar->setTitle(title);
    titleBar->attachWindow(this);
    lay->addWidget(titleBar);

    auto* body = new QWidget(this);
    auto* bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(10, 10, 10, 10);
    lay->addWidget(body, 1);

    auto* hint = new QLabel(
        QString("在 %1@%2:%3 上执行命令（复用当前 SFTP 连接）")
            .arg(QString::fromStdString(conn_.ssh.user))
            .arg(QString::fromStdString(conn_.ssh.host))
            .arg(conn_.ssh.port),
        body);
    hint->setObjectName("paneTitle");
    bodyLay->addWidget(hint);

    // Output area.
    output_ = new QPlainTextEdit(body);
    output_->setReadOnly(true);
    output_->setPlaceholderText("命令输出将显示在这里...");
    bodyLay->addWidget(output_, 1);

    // Input row.
    auto* inputRow = new QHBoxLayout;
    input_ = new QLineEdit(body);
    input_->setPlaceholderText("输入 shell 命令，回车执行（如 ls -la /tmp）");
    runBtn_ = new QPushButton("执行", body);
    runBtn_->setProperty("accent", true);
    stopBtn_ = new QPushButton("停止", body);
    stopBtn_->setEnabled(false);
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(runBtn_);
    inputRow->addWidget(stopBtn_);
    bodyLay->addLayout(inputRow);

    status_ = new QLabel("就绪", body);
    bodyLay->addWidget(status_);

    connect(runBtn_, &QPushButton::clicked, this, &CommandDialog::onRun);
    connect(stopBtn_, &QPushButton::clicked, this, &CommandDialog::onStop);
    connect(input_, &QLineEdit::returnPressed, this, &CommandDialog::onRun);
    // Editing the line by hand exits history-browsing mode.
    connect(input_, &QLineEdit::textEdited, this, [this](const QString&) {
        historyPos_ = -1;
    });
    // Up/down browse command history.
    input_->installEventFilter(this);

    input_->setFocus();
}

bool CommandDialog::eventFilter(QObject* watched, QEvent* ev) {
    if (watched == input_ && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        const bool up = ke->key() == Qt::Key_Up;
        const bool down = ke->key() == Qt::Key_Down;
        if ((up || down) && !history_.isEmpty()) {
            if (historyPos_ == -1) {
                if (up) {
                    // Entering the history: stash the current draft first.
                    historyDraft_ = input_->text();
                    historyPos_ = history_.size() - 1;
                } else {
                    return true;   // Down on a fresh line: nothing below
                }
            } else if (up) {
                if (historyPos_ > 0) --historyPos_;
            } else {
                if (historyPos_ + 1 < history_.size()) {
                    ++historyPos_;
                } else {
                    // Past the newest entry: restore the saved draft.
                    historyPos_ = -1;
                    input_->setText(historyDraft_);
                    return true;
                }
            }
            input_->setText(history_[historyPos_]);
            return true;
        }
    }
    return QDialog::eventFilter(watched, ev);
}

CommandDialog::~CommandDialog() {
    // Make sure a still-running command (tail -f, top, ...) cannot block
    // ~RemoteCommand's thread join and freeze the UI while we close.
    if (cmd_) cmd_->stop();
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
    stopBtn_->setEnabled(true);

    cmd_ = new RemoteCommand(conn_, this);
    connect(cmd_, &RemoteCommand::output, this, &CommandDialog::onOutput);
    connect(cmd_, &RemoteCommand::finished, this, &CommandDialog::onFinished);
    cmd_->start(cmd);
}

void CommandDialog::onStop() {
    if (!cmd_ || !cmd_->isRunning()) return;
    cmd_->stop();
    stopBtn_->setEnabled(false);
    status_->setText("正在停止...");
}

void CommandDialog::onOutput(const QString& text) {
    appendOutput(text);
}

void CommandDialog::onFinished(int exitCode, const QString& message) {
    runBtn_->setEnabled(true);
    stopBtn_->setEnabled(false);
    if (message == QStringLiteral("命令已中止")) {
        appendOutput("\n" + message + "\n", true);
        status_->setText("已中止");
    } else if (!message.isEmpty()) {
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
