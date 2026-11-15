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
#include "site_store.hpp"

class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QTableView;
class QSortFilterProxyModel;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;

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
    void onRemoteOpFailed(const QString& msg);
    void onRemoteContextMenu(const QPoint& pos);
    void onConnectionLost(const QString& msg);
    void onHostKeyUnknown(const QString& fingerprint, const QString& host,
                          quint16 port, const QString& keyHex);
    void renameRemoteSelected();   // F2 inline rename of the current row

    // Transfer dock interactions.
    void onTransferContextMenu(const QPoint& pos);
    void onPauseSelected();
    void onCancelSelected();
    void onClearFinished();
    void onClearAll();

private:
    void buildUi();
    void showRoot(const std::string& root);
    void refreshStatusBar();

    // Drag & drop: panes accept the other's drops (local->remote = upload,
    // remote->local = download). Handled here rather than in the models so
    // the transfer queue stays in one place.
    bool eventFilter(QObject* watched, QEvent* event) override;
    void handleLocalDrop(const QStringList& remotePaths);
    void handleRemoteDrop(const QStringList& localPaths);

    // Transfer queue plumbing (events arrive on worker threads).
    void onTransferEvent(const TransferEvent& ev);
    void enqueueUploads();
    void enqueueDownloads();
    // Drop-target variants: enqueue a specific set of paths.
    void enqueueLocalToRemote(const QStringList& localPaths);
    void enqueueRemoteToLocal(const QStringList& remotePaths);

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

    SiteStore siteStore_;

    QDockWidget* transferDock_ = nullptr;
    QListWidget* transferList_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QPushButton* clearDoneBtn_ = nullptr;
    QSpinBox* workersSpin_ = nullptr;

    QLabel* statusLabel_ = nullptr;

    // One-shot reconnect attempt after the remote transport drops.
    QTimer* reconnectTimer_ = nullptr;

    std::unique_ptr<TransferQueue> queue_;
    std::map<std::uint64_t, QListWidgetItem*> transferItems_;

    // Per-job UI metadata carried on the list item (UserRole) and a
    // side table for the paused/pending title suffix.
    struct JobUi {
        std::uint64_t jobId = 0;
        QString name;
        QString direction;       // "上传" / "下载"
        TransferMode mode = TransferMode::Store;
        TransferState state = TransferState::Queued;
        double pct = 0.0;
        bool paused = false;
    };
    std::map<std::uint64_t, JobUi> jobUi_;
    void refreshTransferItem(std::uint64_t jobId, const TransferEvent* ev);
};

}  // namespace ftpflow
