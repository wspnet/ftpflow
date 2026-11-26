// ftpflow -- main window: dual-pane workbench with remote browsing and a
// live transfer queue dock.
#pragma once

#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <vector>

#include <QList>
#include <QPoint>
#include <QPointer>
#include <QVector>

#include "ftpflow/transfer_queue.hpp"
#include "entry_sort_proxy.hpp"
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
class QComboBox;
class QSplitter;
class QTabWidget;
class QTimer;
class QIcon;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QAbstractItemView;
class QAbstractItemModel;
class QStyledItemDelegate;

namespace ftpflow {

class RemoteShell;
class TerminalWidget;
class TitleBar;
class AuthClient;
struct AuthResult;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    // Frameless window: route WM_NCHITTEST so the window can still be
    // resized by dragging its edges (the native frame is gone).
    bool nativeEvent(const QByteArray& eventType, void* message,
                     qintptr* result) override;
    // Apply Windows 11 native rounded corners once the native window exists.
    void showEvent(QShowEvent* event) override;

private slots:
    void onOpen();
    void onUp();
    void onActivated(const QModelIndex& idx);
    void onVerdictsReady();
    void renameRemoteSelected();   // F2 inline rename of the current row
    void onNewRemoteTab();
    void onTabClosed(int index);
    void onTabBarClicked(int index);   // intercept the trailing "+" tab
    void onTabMoved(int from, int to); // keep the "+" tab pinned to the end

    // Transfer dock interactions.
    void onTransferContextMenu(const QPoint& pos);
    void onToggleProcessQueue();  // FileZilla global pause/process toggle
    void onCancelSelected();
    void onRetrySelected();   // re-enqueue failed/canceled/skipped rows
    void onRetryAllFailed();  // reset & re-enqueue every failed/canceled/skipped row
    void onRemoveSelected();  // FileZilla "remove selected" (drop rows)
    void onClearFinished();
    void onClearAll();
    // Emergency brake (Ctrl+Shift+X): stop ALL jobs at once, drop every
    // queue row and truncate the journal so a restart stays clean.
    void onEmergencyStop();
    // Refresh the "(n)" counts in the three bucket tab titles.
    void updateTransferTabCounts();
    // Append a timestamped line to the bottom message log (FileZilla-style).
    void appendLog(const QString& line);
    // Show/hide the transfer queue dock (toggled from the status-bar button
    // and the Ctrl+J shortcut).
    void toggleTransferDock();

    // Membership / account entry in the bottom-left corner (WorkBuddy-style).
    void onAccountMenu();      // pop the account menu from the corner button
    void onLogin();            // show the login dialog
    void onOpenMemberCenter(); // placeholder for member center / upgrade
    void onLogout();           // clear the local session and refresh the UI
    void onSwitchAccount();    // log out to guest, then open login/register
    void onToggleRequirePassword(bool on);  // "require password every launch"
    void onStartupUnlock();    // deferred startup verification (after show)
    void onUnlockReloginFinished(const ftpflow::AuthResult& r);
    void refreshAccountButton();
    // Two-way site sync after login: push local sites to the cloud, then
    // pull the cloud copy back and merge it into the local store.
    void startSiteSync();
    // Push the current local site list straight to the cloud (local is the
    // authoritative source) -- used after local add/edit/delete edits.
    void pushSitesNow();
    void onSyncFinished(bool ok, const QString& message,
                        const QString& sitesJson);
    void onFetchFinished(bool ok, const QString& message,
                         const QString& sitesJson);

    // ---- Identity-scoped site lists --------------------------------------
    // Load `scope` into siteStore_ (guest = "" / member = "m_..."). No site
    // list is loaded until the identity is known; this is what keeps a
    // member's sites invisible to a guest.
    void switchSiteScope(const QString& scope);
    // Shared login/register completion: migrate guest sites into the member
    // account, switch to that member's private scope, persist the session.
    void applyLoginResult(const ftpflow::AuthResult& r,
                          const QString& baseUrl,
                          const QString& password);
    // Clear the in-memory session + its QSettings keys. Scoped site files
    // are intentionally left on disk so a later login restores them.
    void clearMemberSession();
    // Startup lock outcome: drop the saved session and stay on the (empty)
    // guest list.
    void useGuestFromLock();

private:
    // One remote session per tab (XTerminal-style multi-tab). Each tab owns
    // its own RemoteFsModel + table + address bar + nav/connect buttons.
    struct RemoteTab {
        QWidget* page = nullptr;   // the tab's page widget (identity anchor)
        RemoteFsModel* model = nullptr;
        EntrySortProxyModel* proxy = nullptr;
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
    // Sync the path bar with the current directory and persist it so the
    // next launch restores it.
    void persistLocalDir();
    void refreshStatusBar();
    // Restore in-flight transfers from the durable journal. Deferred to after
    // the event loop starts so restored jobs never run while the MainWindow
    // is still constructing (worker threads would otherwise emit transfer
    // events into a half-built UI).
    void restoreJournal();

    // Per-tab remote event handlers (called via tab-capturing lambdas so a
    // background tab's events hit the right tab).
    void onRemoteContextMenu(RemoteTab& tab, const QPoint& pos);
    void onHostKeyUnknown(RemoteTab& tab, const QString& fingerprint,
                          const QString& host, quint16 port,
                          const QString& keyHex);
    void onRemoteCommand(RemoteTab& tab);   // open the server-command panel
    void onRemoteTerminal(RemoteTab& tab);  // toggle the interactive terminal
    // Text-file editing: open a local file in the system editor, or (remote)
    // download to a temp file, open the editor, then push back on close.
    void editLocalFile(const QString& localPath);
    void editRemoteFile(RemoteTab& tab, const QString& remotePath);
    void onEditDownloaded(const QString& remotePath, const QString& localPath);
    void onEditUploaded(const QString& remotePath);

    // Local pane context menu (right-click on the local file list).
    void onLocalContextMenu(const QPoint& pos);
    // F2 inline rename of the current local row.
    void renameLocalSelected();
    // Delete (keyboard) / "删除..." (context menu) for the focused pane.
    // Shared so both entry points show the same confirm dialog.
    void deleteLocalSelected();
    void deleteRemoteSelected(RemoteTab& tab);

    // Drag & drop: panes accept the other's drops (local->remote = upload,
    // remote->local = download). Handled here rather than in the models so
    // the transfer queue stays in one place.
    bool eventFilter(QObject* watched, QEvent* event) override;
    // Inline rename (Explorer-style): the name cell becomes a QLineEdit.
    // F2, the context menu and a slow second click on an already-selected
    // single name all funnel through beginInlineRename().
    bool beginInlineRename(QAbstractItemView* view,
                           const QModelIndex& proxyIdx);
    // "Slow second click" detection: a plain click on the single selected
    // name arms a short timer; a quick second click (double-click) or a drag
    // cancels it, otherwise the timer opens the inline editor.
    void cancelPendingRename();
    void onRenameClickTimeout();
    // Rubber-band "drag to select" for the two file panes (see eventFilter).
    void applyRubberSelection(const QPoint& pos);
    void endRubberSelection();
    void handleLocalDrop(const QStringList& remotePaths);
    void handleRemoteDrop(const QStringList& localPaths);

    // Transfer queue plumbing (events arrive on worker threads).
    void onTransferEvent(const TransferEvent& ev);
    // The list backing the currently visible bucket tab.
    QListWidget* currentTransferList();
    // Drop-target variants: enqueue a specific set of paths (transfers are
    // driven entirely by drag & drop).
    void enqueueLocalToRemote(const QStringList& localPaths);
    void enqueueRemoteToLocal(const QStringList& remotePaths);
    // Folder enqueue that walks the tree on a worker thread: a large remote
    // directory would otherwise freeze the UI. Completion reports through
    // the status bar + message log (verb = "递归上传" / "递归下载").
    void enqueueTreeAsync(TransferRequest req, const QString& verb);
    // Decides whether a folder should ship as one compressed archive. Returns
    // true when the user opts in (prompted only when the folder is large
    // enough to matter); small folders return false silently (straight
    // per-file transfer, no compression).
    bool askFolderArchive(const QString& folderPath);

    LocalFsModel* model_ = nullptr;
    EntrySortProxyModel* proxy_ = nullptr;
    QTableView* left_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;

    // Rubber-band drag-to-select state for the two file panes. Both panes
    // have dragEnabled(true), so a left-drag on an already-selected row (or
    // a mostly horizontal drag) stays a native file drag; a vertical drag
    // starting elsewhere sweeps a selection instead.
    // QPointer: the frame is parented to a pane's viewport, so closing that
    // tab destroys it -- the pointer must null itself out.
    QPointer<QWidget> rubberBand_;
    QAbstractItemView* rubberView_ = nullptr;
    QSortFilterProxyModel* rubberProxy_ = nullptr;
    QAbstractItemModel* rubberSource_ = nullptr;
    QPoint rubberOrigin_;
    QVector<int> rubberBaseRows_;   // rows selected when the drag began
    Qt::KeyboardModifiers rubberMods_ = Qt::NoModifier;
    bool rubberCandidate_ = false;  // press awaiting the drag threshold
    bool rubberActive_ = false;     // frame showing; we own the mouse moves

    // Shared inline-rename delegate + slow-second-click state.
    QStyledItemDelegate* nameDelegate_ = nullptr;
    QTimer* renameClickTimer_ = nullptr;
    QAbstractItemView* renameView_ = nullptr;
    int renameRow_ = -1;             // proxy row armed for a slow click
    QPoint renamePress_;             // press position (drag cancels rename)
    bool renameCandidate_ = false;   // press was on the selected name cell

    TitleBar* titleBar_ = nullptr;   // custom frameless title bar

    QTabWidget* remoteTabs_ = nullptr;
    QList<RemoteTab*> tabs_;   // stable pointers; lambdas capture RemoteTab*
    int newTabIndex_ = -1;     // index of the trailing "+" tab (pinned last)
    QWidget* plusPage_ = nullptr;  // page widget of the "+" tab
    bool syncingTabs_ = false;     // re-entrancy guard for onTabMoved
    void addNewTabButton();    // append the "+" tab; re-adds it after removals
    QIcon makePlusIcon() const; // draw a crisp "+" glyph for the tab
    QIcon makeCloseIcon() const; // draw a thin theme-aware "x" close glyph
    // Builds a new tab (UI + signal wiring) and returns it.
    RemoteTab* addRemoteTab();
    RemoteTab* currentTab();
    const RemoteTab* currentTab() const;
    RemoteFsModel* currentRemote() const;
    // Sync the window title (and taskbar label) to the current tab's name.
    void syncWindowTitle();

    SiteStore siteStore_;

    QDockWidget* transferDock_ = nullptr;
    QTabWidget* transferTabs_ = nullptr;
    QListWidget* queueList_ = nullptr;    // "列队的文件": Queued + Running
    QListWidget* failedList_ = nullptr;   // "传输失败": Failed + Canceled
    QListWidget* doneList_ = nullptr;     // "成功的传输": Done
    QPlainTextEdit* logView_ = nullptr;   // FileZilla-style message log
    QPushButton* pauseBtn_ = nullptr;     // FileZilla "process queue" toggle
    QPushButton* cancelBtn_ = nullptr;
    QPushButton* clearDoneBtn_ = nullptr;
    QPushButton* clearAllBtn_ = nullptr;
    QPushButton* emergencyBtn_ = nullptr;  // one-click "stop everything now"
    QSpinBox* workersSpin_ = nullptr;
    QComboBox* conflictCombo_ = nullptr;   // same-name conflict policy
    QComboBox* rateLimitCombo_ = nullptr;  // global transfer speed limit

    QLabel* statusLabel_ = nullptr;
    // Busy indicator for async remote operations (recursive delete/move).
    // Without it a long recursive delete looks like the app is doing nothing.
    // The bar is only created on first use and stays hidden otherwise.
    QProgressBar* opBusy_ = nullptr;
    int opBusyCount_ = 0;   // concurrent ops across tabs
    void beginRemoteOp(const QString& message);
    void updateRemoteOp(const QString& message);
    void endRemoteOp(const QString& message, bool ok);

    // Account / membership entry (bottom-left corner button + its menu).
    QToolButton* accountBtn_ = nullptr;
    QString memberName_;   // logged-in display name; empty = not logged in
    QString memberLevel_;  // e.g. "免费版" / "会员"; empty if not logged in
    QString memberToken_;  // JWT from the member API; empty = not logged in
    AuthClient* syncClient_ = nullptr;  // long-lived client for site sync
    QString memberBaseUrl_;  // API base URL from the last successful login

    // ---- Identity / session state ----------------------------------------
    // The site list is scoped per identity. memberScope_ == "" means the
    // guest list is loaded; "m_..." means a member's private list.
    QString memberScope_;
    QString memberUsername_;   // stable account id (for re-verification)
    int memberUserId_ = 0;
    // "每次启动要求验证密码" -- a member-only preference. While locked, the
    // guest list is loaded and no member site data is touched.
    bool requirePasswordOnStartup_ = false;
    bool sessionLocked_ = false;
    bool journalRestored_ = false;
    // Guest sites (with passwords) captured at login, awaiting upload into
    // this member's cloud slot. Cleared only after a successful push.
    QString pendingGuestSitesJson_;
    // Scope a pending fetch was issued for; late replies from a session we
    // already left are discarded instead of merging into the wrong list.
    QString pendingFetchScope_;
    // Suppress the debounced push while a remote merge is being applied.
    bool suppressSiteSync_ = false;

    // Debounced cloud sync after local site edits (add/edit/delete/import).
    QTimer* siteSyncTimer_ = nullptr;

    // One-shot reconnect attempt after the remote transport drops.
    QTimer* reconnectTimer_ = nullptr;

    // Debounced remote-list refresh after uploads finish. Instead of listing
    // the server once per completed file (N LIST requests for a batch of N),
    // every Finished upload into the currently shown directory just (re)arms
    // this single-shot timer; one refresh runs after the burst settles.
    QTimer* remoteRefreshTimer_ = nullptr;

    // Debounced tab-count refresh: with a huge restored queue, scanning all
    // jobUi_ entries on every Queued event is quadratic. One timer run after
    // a burst keeps the counters correct without freezing the UI.
    QTimer* transferCountTimer_ = nullptr;
    // Bumped by the emergency brake. A folder scan still running at that
    // moment must not re-enqueue its results after the stop.
    quint64 queueStopEpoch_ = 0;
    // Cooperative abort flags handed to background folder scans; the brake
    // flips every live flag so a late scan result cannot refill the queue.
    std::vector<std::shared_ptr<std::atomic<bool>>> treeAbortFlags_;

    // Set once any upload has run in the current queue batch; cleared when
    // the queue drains. Drives the "refresh once when everything finished"
    // step so download-only batches skip the needless LIST.
    bool uploadsSeen_ = false;

    // In-flight remote text-file edit: editor process + the temp staging file
    // and remote path to push back once the editor exits.
    struct PendingEdit {
        QProcess* proc = nullptr;   // the system editor (tracked for exit)
        QString localPath;          // temp staging file on disk
        QString remotePath;         // remote path to upload back to
        QString remoteHost;         // user@host label for the confirm dialog
        QString remoteDir;          // remote parent directory for the dialog
    };
    QList<PendingEdit> pendingEdits_;

    std::unique_ptr<TransferQueue> queue_;
    // In-flight folder-scan workers (enqueueTreeAsync). Declared after
    // queue_ so the futures' blocking destructors run BEFORE the queue is
    // torn down -- a walk in progress always sees a live queue_. UI thread
    // only: tasks never touch this vector themselves.
    std::vector<std::future<void>> treeWorkers_;
    std::map<std::uint64_t, QListWidgetItem*> transferItems_;

    // Per-job UI metadata carried on the list item (UserRole) and a
    // side table for the paused/pending title suffix.
    struct JobUi {
        std::uint64_t jobId = 0;
        QString name;            // file/dir base name
        QString fullPath;        // complete source path (FileZilla-style)
        QString targetPath;      // destination directory / full path
        QString direction;       // "上传" / "下载"
        QString error;           // last failure text (survives the final refresh)
        TransferMode mode = TransferMode::Store;
        TransferState state = TransferState::Queued;
        double pct = 0.0;
        double speedBps = 0.0;   // latest instantaneous speed (for ETA)
        qint64 startedAtMs = 0;  // monotonic clock when the job went Running
        bool paused = false;
    };
    std::map<std::uint64_t, JobUi> jobUi_;
    // Drop every queue row + its per-row widget in one go (emergency stop /
    // clear all). Late terminal events for removed rows are ignored.
    void clearTransferRows();
    void refreshTransferItem(std::uint64_t jobId, const TransferEvent* ev);
    // Move a job's row into the bucket matching its current state; returns
    // the list that now owns the item (nullptr if the job is unknown).
    QListWidget* placeTransferItem(std::uint64_t jobId,
                                   QListWidgetItem* item);
};

}  // namespace ftpflow
