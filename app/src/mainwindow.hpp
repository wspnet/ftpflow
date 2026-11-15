// ftpflow -- main window: dual-pane workbench with remote browsing and a
// live transfer queue dock.
#pragma once

#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>

#include <map>
#include <memory>

#include "ftpflow/transfer_queue.hpp"
#include "local_fs_model.hpp"
#include "remote_fs_model.hpp"

class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QTableView;
class QSortFilterProxyModel;
class QLabel;
class QPushButton;

namespace ftpflow {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onOpen();
    void onUp();
    void onActivated(const QModelIndex& idx);
    void onVerdictsReady();
    void onConnect();
    void onDisconnect();
    void onRemoteUp();
    void onRemoteRefresh();
    void onRemoteActivated(const QModelIndex& idx);
    void onRemoteConnected();
    void onRemoteConnectFailed(const QString& msg);
    void onRemoteBrowseFailed(const QString& msg);

private:
    void buildUi();
    void showRoot(const std::string& root);
    void refreshStatusBar();

    // Transfer queue plumbing (events arrive on worker threads).
    void onTransferEvent(const TransferEvent& ev);
    void enqueueUploads();
    void enqueueDownloads();

    LocalFsModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* left_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;

    RemoteFsModel* remote_ = nullptr;
    QSortFilterProxyModel* remoteProxy_ = nullptr;
    QTableView* rightTable_ = nullptr;
    QLineEdit* remotePathEdit_ = nullptr;
    QPushButton* connectBtn_ = nullptr;
    QPushButton* disconnectBtn_ = nullptr;

    QDockWidget* transferDock_ = nullptr;
    QListWidget* transferList_ = nullptr;

    QLabel* statusLabel_ = nullptr;

    std::unique_ptr<TransferQueue> queue_;
    std::map<std::uint64_t, QListWidgetItem*> transferItems_;
};

}  // namespace ftpflow
