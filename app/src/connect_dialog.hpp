// ftpflow -- remote connection dialog (FTP / SFTP) with saved sites.
#pragma once

#include <QtWidgets/QDialog>

#include "remote_fs_model.hpp"
#include "site_store.hpp"

class QComboBox;
class QAction;
class QFileDialog;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
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

    // True when "确定" was chosen: the site was saved and the dialog closed
    // WITHOUT requesting a connection (FileZilla Site Manager "OK").
    bool saveOnly() const { return saveOnly_; }

protected:
    void showEvent(QShowEvent* event) override;   // native rounded corners

private slots:
    void onProtocolChanged(int index);
    void onBrowseKey();
    void onSiteChosen(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void onFilterChanged(const QString& text);
    void onSaveSite();
    void onDeleteSite();
    void onExportSites();
    void onImportSites();
    void onTreeContextMenu(const QPoint& pos);
    // Commit an inline rename of a site / folder node (no modal dialog).
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    // Parse pasted connection text (url / ip / ftp 账号 / 密码) in the note
    // box and auto-fill the matching form fields.
    void onSmartFill();

private:
    void buildUi();
    void rebuildSiteTree(const QString& filter = QString());
    // Current year-month as YYYYMM (e.g. 202611, 202701).
    static QString currentYearMonth();
    // Clear the form for a brand-new site (optionally pre-assigning a group).
    void resetFormForNewSite(const QString& group = QString());
    // Duplicate a site under a unique "<name> 副本" name (carries password).
    void duplicateSite(const QString& siteName);
    // Focus a site leaf by name (used after duplicate to reveal the copy).
    void selectSiteInTree(const QString& name);
    // Validate host/user/port; fills an empty port with the protocol default.
    // Returns true when ready to connect/save; otherwise focuses the first
    // bad field and shows the reason in the dialog's error label.
    bool validateForm();

    SiteStore* store_ = nullptr;
    QString editingName_;   // name of the site currently loaded ("" = new site)
    bool saveOnly_ = false;  // set by "确定" (save & close, no connect)
    QLabel* errorLabel_ = nullptr;   // inline validation feedback (red)
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPushButton* deleteSiteBtn_ = nullptr;
    QComboBox* proto_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* host_ = nullptr;
    QLineEdit* port_ = nullptr;
    QLineEdit* user_ = nullptr;
    QLineEdit* password_ = nullptr;
    QPlainTextEdit* note_ = nullptr;
    QLineEdit* keyPath_ = nullptr;
    QLineEdit* startDir_ = nullptr;
    QLineEdit* pushCmd_ = nullptr;
    QLineEdit* group_ = nullptr;
    QComboBox* encoding_ = nullptr;
    QPushButton* browseKeyBtn_ = nullptr;
    QPushButton* smartFillBtn_ = nullptr;
    QAction* revealPwAct_ = nullptr;   // password show/hide toggle
};

}  // namespace ftpflow
