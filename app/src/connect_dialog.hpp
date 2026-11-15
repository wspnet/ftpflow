// ftpflow -- remote connection dialog (FTP / SFTP) with saved sites.
#pragma once

#include <QtWidgets/QDialog>

#include "remote_fs_model.hpp"
#include "site_store.hpp"

class QComboBox;
class QFileDialog;
class QLineEdit;

namespace ftpflow {

class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    // `store` may be null (no persistence; pure manual entry).
    ConnectDialog(QWidget* parent = nullptr, SiteStore* store = nullptr);

    // The result once accept()ed.
    RemoteFsModel::ConnInfo connInfo() const;

    // Fills the form from a saved site (password comes from Credential
    // Manager).
    void loadSite(const SiteEntry& e);

private slots:
    void onProtocolChanged(int index);
    void onBrowseKey();
    void onSiteChosen(int index);
    void onSaveSite();

private:
    void buildUi();

    SiteStore* store_ = nullptr;
    QComboBox* site_ = nullptr;
    QPushButton* saveSiteBtn_ = nullptr;
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
