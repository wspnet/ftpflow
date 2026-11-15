// ftpflow -- remote connection dialog (FTP / SFTP).
#pragma once

#include <QtWidgets/QDialog>

#include "remote_fs_model.hpp"

class QComboBox;
class QFileDialog;
class QLineEdit;

namespace ftpflow {

class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectDialog(QWidget* parent = nullptr);

    // The result once accept()ed.
    RemoteFsModel::ConnInfo connInfo() const;

private slots:
    void onProtocolChanged(int index);
    void onBrowseKey();

private:
    void buildUi();

    QComboBox* proto_ = nullptr;
    QLineEdit* host_ = nullptr;
    QLineEdit* port_ = nullptr;
    QLineEdit* user_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* keyPath_ = nullptr;
    QLineEdit* startDir_ = nullptr;
    QLineEdit* pushCmd_ = nullptr;
    QPushButton* browseKeyBtn_ = nullptr;
};

}  // namespace ftpflow
