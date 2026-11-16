// ftpflow -- main window: dual-pane workbench with remote browsing and a
// live transfer queue dock.
#pragma once

#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>

#include <map>
#include <memory>
#include <vector>

#include <QList>
#include <QVector>

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
class QSplitter;
class QTabWidget;
class QTimer;

namespace ftpflow {

class RemoteShell;
class TerminalWidget;

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
    void renameRemoteSelected();   // F2 inline rename of the current row
    void onNewRemoteTab();
    void onTabClosed(int index);

    // Transfer dock interactions.
    void onTransferContextMenu(const QPoint& pos);
    void onPauseSelected();
    void onCancelSelected();
    void onClearFinished();
    void onClearAll();

private:
    // One remote session per tab (XTerminal-style multi-tab). Each tab owns
    // its own RemoteFsModel + table + address bar + nav/connect buttons.
    struct RemoteTab {
        RemoteFsModel* model = nullptr;
        QSortFilterProxyModel* proxy = nullptr;
        QTableView* table = nullptr;
        QLineEdit* pathEdit = nullptr;
        QPushButton* connectBtn = nullptr;
        QPushButton* disconnectBtn = nullptr;
        QPushButton* upBtn = nullptr;
        QPushButton* refreshBtn = nullptr;
        QPushButton* cmdBtn = nullptr;    // open the server-command panel
        QPushButton* termBtn = nullptr;   // toggle the interactive terminal
        // Interactive terminal (vertical split below the file list).
        QSplitter* termSplitter = nullptr;
        TerminalWidget* terminal = nullptr;
        RemoteShell* shell = nullptr;
        bool termVisible = false;
    };

    void buildUi();
    void showRoot(const std::string& root);
    void refreshStatusBar();

    // Per-tab remote event handlers (called via tab-capturing lambdas so a
    // background tab's events hit the right tab).
    void onRemoteContextMenu(RemoteTab& tab, const QPoint& pos);
    void onHostKeyUnknown(RemoteTab& tab, const QString& fingerprint,
                          const QString& host, quint16 port,
                          const QString& keyHex);
    void onRemoteCommand(RemoteTab& tab);   // open the server-command panel
    void onRemoteTerminal(RemoteTab& tab);  // toggle the interactive terminal

    // Drag & drop: panes accept the other's drops (local->remote = upload,
    // remote->local = download). Handled here rather than in the models so
    // the transfer queue stays in one place.
    bool eventFilter(QObject* watched, QEvent* event) override;
    void handleLocalDrop(const QStringList& remotePaths);
    void handleRemoteDrop(const QStringList& localPaths);

    // Transfer queue plumbing (events arrive on worker threads).
    void onTransferEvent(const TransferEvent& ev);
    // Drop-target variants: enqueue a specific set of paths (transfers are
    // driven entirely by drag & drop).
    void enqueueLocalToRemote(const QStringList& localPaths);
    void enqueueRemoteToLocal(const QStringList& remotePaths);

    LocalFsModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* left_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;

    QTabWidget* remoteTabs_ = nullptr;
    QList<RemoteTab*> tabs_;   // stable pointers; lambdas capture RemoteTab*
    // Builds a new tab (UI + signal wiring) and returns it.
    RemoteTab* addRemoteTab();
    RemoteTab* currentTab();
    const RemoteTab* currentTab() const;
    RemoteFsModel* currentRemote() const;

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
