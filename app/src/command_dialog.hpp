// ftpflow -- server command panel.
//
// A dialog that runs shell commands on the remote host over an SSH exec
// channel, reusing the active tab's connection. Supports a simple command
// history (up/down), live stdout/stderr, and per-run exit status.

#pragma once

#include <QDialog>

#include <QStringList>

#include "remote_fs_model.hpp"

class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;

namespace ftpflow {

class RemoteCommand;

class CommandDialog : public QDialog {
    Q_OBJECT
public:
    explicit CommandDialog(const RemoteFsModel::ConnInfo& ci,
                           QWidget* parent = nullptr);

private slots:
    void onRun();
    void onOutput(const QString& text);
    void onFinished(int exitCode, const QString& message);

private:
    void appendOutput(const QString& text, bool isError = false);

    RemoteFsModel::ConnInfo conn_;
    QLineEdit* input_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* runBtn_ = nullptr;

    RemoteCommand* cmd_ = nullptr;
    QStringList history_;
    int historyPos_ = -1;
};

}  // namespace ftpflow
