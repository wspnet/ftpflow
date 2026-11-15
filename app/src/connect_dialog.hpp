// ftpflow -- remote connection dialog (FTP / SFTP) with saved sites.
#pragma once

#include <QtWidgets/QDialog>

#include "remote_fs_model.hpp"
#include "site_store.hpp"

class QComboBox;
class QFileDialog;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

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
    void onSiteChosen(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void onFilterChanged(const QString& text);
    void onSaveSite();
    void onDeleteSite();
    void onExportSites();
    void onImportSites();

private:
    void buildUi();
    void rebuildSiteTree(const QString& filter = QString());
    // Append a site leaf under its group top-level node (created on demand).
    QTreeWidgetItem* ensureGroup(const QString& group);

    SiteStore* store_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPushButton* saveSiteBtn_ = nullptr;
    QPushButton* deleteSiteBtn_ = nullptr;
    QComboBox* proto_ = nullptr;
    QLineEdit* host_ = nullptr;
    QLineEdit* port_ = nullptr;
    QLineEdit* user_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* keyPath_ = nullptr;
    QLineEdit* startDir_ = nullptr;
    QLineEdit* pushCmd_ = nullptr;
    QLineEdit* group_ = nullptr;
    QPushButton* browseKeyBtn_ = nullptr;
};

}  // namespace ftpflow
