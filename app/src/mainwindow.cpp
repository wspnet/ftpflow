// ftpflow -- main window implementation.
#include "mainwindow.hpp"

#include <QAction>
#include <QWidgetAction>
#include <QApplication>
#include <QAbstractItemView>
#include <QAbstractProxyModel>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRubberBand>
#include <QScrollBar>
#include <QSet>
#include <QProgressBar>
#include <QPainter>
#include <QProcess>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include "command_dialog.hpp"
#include "auth_dialog.hpp"
#include "auth_client.hpp"
#include "session_unlock_dialog.hpp"
#include "connect_dialog.hpp"
#include "entry_sort.hpp"
#include "name_edit_delegate.hpp"
#include "remote_shell.hpp"
#include "terminal_widget.hpp"
#include "title_bar.hpp"
#include "ui_util.hpp"
#include "ftpflow/known_hosts.hpp"
#include "ftpflow/protocol_status.hpp"
#include "ftpflow/text_codec.hpp"
#include "ftpflow/transfer_journal.hpp"
#include "ftpflow/tree_walk.hpp"
#include "ftpflow/version.hpp"

#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
// Windows 11 native rounded corners (DWM). The enum/constant may be absent
// from older SDK headers, so define them locally when needed.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#endif

namespace ftpflow {

namespace {

// ---- Identity / session helpers ------------------------------------------

// A stable, filesystem-safe scope key for a member account. userId is
// preferred; the username hash covers deployments that don't return an id
// and keeps two accounts with the same numeric id (different backends) apart.
QString memberScopeFor(int userId, const QString& username) {
    const QString id = QString::number(userId) + ":" +
                       username.trimmed().toLower();
    const QByteArray digest =
        QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256);
    const QString hex = QString::fromLatin1(digest.left(6).toHex());
    const QString uid = userId > 0 ? QString("u%1_").arg(userId) : QString();
    return QStringLiteral("m_") + uid + hex;
}

// Sessions saved by builds predating identity scopes had no scope key.
// Derive a private one so their sites never land in the guest file.
QString legacyMemberScope(const QString& displayName,
                          const QString& username, int userId) {
    const QString key = username.trimmed().isEmpty() ? displayName : username;
    return QStringLiteral("m_legacy_") + memberScopeFor(userId, key).mid(2);
}

// Offline verifier for "require password on every launch". This is a UI
// lock, not encryption (the JWT is still in QSettings): salt + iterated
// SHA-256 makes an offline dictionary attack on the saved value expensive.
QString passwordVerifier(const QString& salt, const QString& username,
                         const QString& password) {
    QByteArray d = (salt + "|" + username.trimmed().toLower() + "|" +
                    password).toUtf8();
    for (int i = 0; i < 2048; ++i)
        d = QCryptographicHash::hash(d, QCryptographicHash::Sha256);
    return QString::fromLatin1(d.toHex());
}

// Delete confirmation for the file panes. "Yes" is both the default action
// and the focused widget, so pressing Enter confirms the deletion directly
// (no need to Tab to the button or reach for the mouse first).
bool confirmDeletion(QWidget* parent, const QString& message) {
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("删除确认"));
    box.setText(message);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::Yes);
    if (QAbstractButton* yes = box.button(QMessageBox::Yes))
        yes->setFocus();
    box.exec();
    return box.clickedButton() == box.button(QMessageBox::Yes);
}

QString humanSpeed(double bps) {
    if (bps <= 0) return {};
    if (bps < 1024.0) return QString::number(bps, 'f', 0) + " B/s";
    if (bps < 1024.0 * 1024.0)
        return QString::number(bps / 1024.0, 'f', 1) + " KiB/s";
    return QString::number(bps / (1024.0 * 1024.0), 'f', 2) + " MiB/s";
}

// "HH:MM:SS" for elapsed/remaining time, collapsing to "MM:SS" under an hour
// and "Ns" under a minute (FileZilla-style compact ETA).
QString humanDuration(qint64 sec) {
    if (sec < 0) sec = 0;
    if (sec < 60) return QString("%1秒").arg(sec);
    const qint64 m = sec / 60, s = sec % 60;
    if (m < 60) return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
    const qint64 h = m / 60, mm = m % 60;
    return QString("%1:%2:%3").arg(h).arg(mm, 2, 10, QChar('0'))
                              .arg(s, 2, 10, QChar('0'));
}

std::string readKeyFile(const QString& path) {
    // Shared Unicode-safe reader (openLocalFile path conversion).
    return ftpflow::readWholeFileUtf8(path.toStdString());
}

// Whether a path is an editable text file, judged by extension against a
// whitelist of common text/code/config types. Binary files (images, archives,
// executables, office docs, ...) are excluded so "edit" is only offered where
// it makes sense.
bool isTextFile(const QString& path) {
    static const QSet<QString> kTextExt = {
        "txt", "log",  "md",  "markdown", "ini", "conf", "config", "cfg", "cnf",
        "json", "xml",  "yaml", "yml",   "toml", "html", "htm", "xhtml",
        "css",  "js",   "mjs", "ts",    "jsx",  "tsx",  "vue", "php",
        "py",   "sh",   "bash", "bat",  "cmd",  "ps1",  "rb",  "pl",
        "go",   "rs",   "java", "c",    "h",    "cpp",  "hpp", "cc",
        "cs",   "sql",  "lua", "r",    "env",  "gitignore", "dockerfile",
        "properties", "csv", "tsv",    "svg",  "sass", "scss", "less",
    };
    const int dot = path.lastIndexOf(QChar('.'));
    if (dot < 0) return false;
    const QString ext = path.mid(dot + 1).toLower();
    return kTextExt.contains(ext);
}

// Circular avatar glyph for the account button: a green head with a friendly
// smiling face, matching the WorkBuddy account avatar look.
QIcon makeAvatarIcon(int size = 18) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    const qreal s = size;
    // Green circle face.
    p.setBrush(QColor("#3fb96b"));
    p.drawEllipse(QRectF(0, 0, s, s));
    // Eyes (white dots).
    p.setBrush(QColor("#ffffff"));
    const qreal ex = s * 0.30, ey = s * 0.36, er = s * 0.075;
    p.drawEllipse(QRectF(ex - er, ey - er, er * 2, er * 2));
    p.drawEllipse(QRectF(s - ex - er, ey - er, er * 2, er * 2));
    // Smile (white arc).
    QPen smile(QColor("#ffffff"), s * 0.07);
    smile.setCapStyle(Qt::RoundCap);
    p.setPen(smile);
    p.setBrush(Qt::NoBrush);
    QRectF mouth(s * 0.32, s * 0.48, s * 0.36, s * 0.28);
    p.drawArc(mouth, 200 * 16, 140 * 16);
    return QIcon(pm);
}

// Last path segment, handling both '/' and '\\' separators.
QString baseNameOf(const QString& path) {
    const int slash = path.lastIndexOf(QChar('/'));
    const int bslash = path.lastIndexOf(QChar('\\'));
    const int cut = qMax(slash, bslash);
    if (cut < 0) return path;
    return path.mid(cut + 1);
}

// Parent directory of a path (either separator style), or empty for a bare
// filename. Used to show a FileZilla-style destination directory.
QString parentDirOf(const QString& path) {
    const int slash = path.lastIndexOf(QChar('/'));
    const int bslash = path.lastIndexOf(QChar('\\'));
    const int cut = qMax(slash, bslash);
    if (cut < 0) return {};
    if (cut == 0) return path.left(1);   // "/x" -> "/"
    return path.left(cut);
}

// A restored batch can contain tens of thousands of files. Creating a rich
// QWidget (two labels + progress bar + layout) for EVERY row is what makes
// the UI unresponsive; only the first rows of each bucket get one. Active
// (Running) jobs always get a widget, and rows beyond the cap still show
// their full path/status as plain two-line text.
constexpr int kMaxRichTransferRows = 200;

// Per-row widget for the transfer queue. FileZilla-style: a bold full path
// (with a direction arrow) on the first line, dim destination/status/speed
// metadata on the second, and a progress bar at the bottom.
class TransferRowWidget : public QWidget {
public:
    explicit TransferRowWidget(QWidget* parent = nullptr) : QWidget(parent) {
        // Let mouse clicks fall through to the QListWidgetItem so the row can
        // be selected (pause/cancel/retry act on the selection). Without this
        // the text-selectable labels below swallow clicks and the row never
        // gets selected, which made "暂停" appear to do nothing.
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 4, 8, 4);
        lay->setSpacing(2);
        pathLabel_ = new QLabel(this);
        QFont pf = pathLabel_->font();
        pf.setBold(true);
        pathLabel_->setFont(pf);
        pathLabel_->setWordWrap(true);
        metaLabel_ = new QLabel(this);
        QFont mf = metaLabel_->font();
        mf.setPointSizeF(mf.pointSizeF() * 0.9);
        metaLabel_->setFont(mf);
        metaLabel_->setStyleSheet("color: #9d9d9d;");
        metaLabel_->setWordWrap(true);
        bar_ = new QProgressBar(this);
        bar_->setRange(0, 1000);
        bar_->setValue(0);
        bar_->setTextVisible(true);
        bar_->setFixedHeight(16);
        lay->addWidget(pathLabel_);
        lay->addWidget(metaLabel_);
        lay->addWidget(bar_);
    }

    void setPath(const QString& text) { pathLabel_->setText(text); }
    void setMeta(const QString& text) { metaLabel_->setText(text); }
    void setShowProgress(bool show) {
        bar_->setVisible(show);
        if (!show) {
            // Collapse the space the bar would occupy so rows in the
            // finished/failed buckets stay compact.
            bar_->setFixedHeight(0);
        } else {
            bar_->setFixedHeight(16);
        }
    }
    QProgressBar* bar() { return bar_; }

private:
    QLabel* pathLabel_ = nullptr;
    QLabel* metaLabel_ = nullptr;
    QProgressBar* bar_ = nullptr;
};

}  // namespace

// Forward declarations: the login/scope-switch code above needs these
// serializers, whose definitions live further down with the sync handlers.
static QString serializeSitesForSync(const ftpflow::SiteStore& store);
static int mergeDownloadedSites(ftpflow::SiteStore& store,
                                const QString& payload,
                                bool overwriteExisting);

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Frameless window with a custom title bar (XTerminal-style). Keep the
    // native resize capability via WM_NCHITTEST in nativeEvent().
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);

    // The shared inline-rename delegate and the "slow second click" timer
    // have to exist before buildUi() wires the two file panes to them.
    nameDelegate_ = new NameEditDelegate(this);
    renameClickTimer_ = new QTimer(this);
    renameClickTimer_->setSingleShot(true);
    // Wait slightly longer than a double-click: a quick second click means
    // "open folder", a single slow click on the selected name means "rename".
    renameClickTimer_->setInterval(QApplication::doubleClickInterval() + 80);
    connect(renameClickTimer_, &QTimer::timeout, this,
            &MainWindow::onRenameClickTimeout);

    buildUi();
    syncWindowTitle();

    // ---- Identity / session restore (BEFORE loading any site list) ------
    // The site list is scoped per identity: a guest must never be handed a
    // member's sites, and each member has a private list. When startup
    // verification is enabled we load the (empty) guest list and keep the
    // member session locked until the password is confirmed.
    QSettings settings;
    requirePasswordOnStartup_ =
        settings.value("member/requirePasswordOnStartup", false).toBool();

    const QString savedName = settings.value("member/name").toString();
    const QString savedLevel = settings.value("member/level").toString();
    const QString savedToken = settings.value("member/token").toString();
    const QString savedBase = settings.value("member/baseUrl").toString();
    const QString savedUser = settings.value("member/username").toString();
    const int savedUid = settings.value("member/userId", 0).toInt();
    QString savedScope = settings.value("member/scope").toString();

    const bool haveSession = !savedToken.isEmpty() && !savedName.isEmpty();
    const bool legacyUpgrade = haveSession && savedScope.isEmpty();
    if (legacyUpgrade) {
        // A session saved by a build without identity scopes: give it its
        // own private scope so member sites never land in the guest file.
        savedScope = legacyMemberScope(savedName, savedUser, savedUid);
        settings.setValue("member/scope", savedScope);
    }

    if (haveSession && requirePasswordOnStartup_) {
        // Locked: stay on the guest list, keep all member fields empty, and
        // defer the verification dialog until the window is visible.
        sessionLocked_ = true;
        memberScope_.clear();
        switchSiteScope(QString());
    } else if (haveSession) {
        memberName_ = savedName;
        memberLevel_ = savedLevel;
        memberToken_ = savedToken;
        memberBaseUrl_ = savedBase;
        memberUsername_ = savedUser;
        memberUserId_ = savedUid;
        memberScope_ = savedScope;
        switchSiteScope(savedScope);
    } else {
        memberScope_.clear();
        switchSiteScope(QString());
    }
    refreshAccountButton();

    // One-time upgrade from builds without identity scopes: those kept the
    // whole (logged-in) list in the guest file. Fold it into this member's
    // private scope and empty the guest list, exactly like a fresh login --
    // otherwise the first launch after upgrading would look like data loss.
    // Skipped while locked: then the guest file stays intact until the user
    // verifies, so choosing "guest" can still keep their own sites.
    if (legacyUpgrade && !sessionLocked_) {
        SiteStore guest;
        guest.setScope(QString());
        guest.load();
        if (!guest.sites().isEmpty() || !guest.folders().isEmpty()) {
            pendingGuestSitesJson_ = serializeSitesForSync(guest);
            guest.clearAll();
            // Persist immediately into the member scope so the data survives
            // even if the app closes before the deferred cloud sync runs.
            suppressSiteSync_ = true;
            mergeDownloadedSites(siteStore_, pendingGuestSitesJson_,
                                 /*overwriteExisting=*/true);
            siteStore_.save();
            suppressSiteSync_ = false;
        }
    }

    // Long-lived auth client for post-login site sync. Owned by the window so
    // it outlives the (modal) login dialog which destroys its own client.
    syncClient_ = new AuthClient(this);
    connect(syncClient_, &AuthClient::syncFinished, this,
            &MainWindow::onSyncFinished);
    connect(syncClient_, &AuthClient::fetchFinished, this,
            &MainWindow::onFetchFinished);
    connect(syncClient_, &AuthClient::loginFinished, this,
            &MainWindow::onUnlockReloginFinished);
    connect(syncClient_, &AuthClient::requestFailed, this,
            [this](const QString& what) {
                statusBar()->showMessage(
                    QString("会员服务请求失败：%1").arg(what), 5000);
            });

    // Debounce site edits: a burst of add/edit/delete/import operations is
    // collapsed into one cloud sync shortly after the last change. Only fires
    // when logged in (token present) and not while a session is locked.
    siteSyncTimer_ = new QTimer(this);
    siteSyncTimer_->setSingleShot(true);
    siteSyncTimer_->setInterval(1200);   // 1.2s debounce
    connect(siteSyncTimer_, &QTimer::timeout, this, [this] {
        if (!memberToken_.isEmpty() && !sessionLocked_) pushSitesNow();
    });
    connect(&siteStore_, &SiteStore::sitesChanged, this, [this] {
        if (memberToken_.isEmpty() || sessionLocked_ || suppressSiteSync_)
            return;
        siteSyncTimer_->start();
    });

    // A restored (unlocked) member session also syncs on launch so cloud
    // changes made on another device show up without a manual re-login.
    if (haveSession && !sessionLocked_) {
        QTimer::singleShot(900, this, [this] {
            if (!memberToken_.isEmpty() && !sessionLocked_) startSiteSync();
        });
    }
    if (sessionLocked_)
        QTimer::singleShot(0, this, &MainWindow::onStartupUnlock);

    // Default concurrency is 5 simultaneous transfers (FileZilla-style).
    // The user's last choice is persisted in QSettings; a fresh install
    // starts at 5. Clamp out-of-range values written by older builds.
    int workers = QSettings().value("transfer/workers", 5).toInt();
    if (workers < 1 || workers > 64) workers = 5;
    queue_ = std::make_unique<TransferQueue>(
        static_cast<unsigned>(workers), 100);
    if (workersSpin_) {
        // Block signals: applying the saved value during construction must
        // not flash a status message or rewrite the setting.
        const bool blocked = workersSpin_->blockSignals(true);
        workersSpin_->setValue(static_cast<int>(queue_->workerCount()));
        workersSpin_->blockSignals(blocked);
    }
    queue_->setEventCallback([this](const TransferEvent& ev) {
        // Worker thread -> UI thread.
        QMetaObject::invokeMethod(
            this, [this, ev] { onTransferEvent(ev); }, Qt::QueuedConnection);
    });

    // One count refresh after a burst of transfer events. Counting all rows
    // on each of 17,000 Queued events is O(n^2) and froze the window.
    transferCountTimer_ = new QTimer(this);
    transferCountTimer_->setSingleShot(true);
    transferCountTimer_->setInterval(150);
    connect(transferCountTimer_, &QTimer::timeout, this,
            &MainWindow::updateTransferTabCounts);

    // Durable journal: resume in-flight transfers after a restart. The
    // journal drops credentials; re-supply them from the saved site. This is
    // deferred to the event loop (restoreJournal) so restored jobs never
    // start while the window is still under construction.
    queue_->setJournal(TransferJournal::defaultPath());
    QTimer::singleShot(0, this, [this] { restoreJournal(); });

    // One-shot reconnect after a transport drop.
    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, [this] {
        RemoteTab* t = currentTab();
        if (!t) return;
        const auto ci = t->model->connInfo();
        t->pathEdit->setText("重连中...");
        t->model->connectTo(ci);
    });

    // Debounced remote-list refresh: uploads into the visible directory
    // re-arm this timer; only one LIST runs after the burst of Finished
    // events settles (250 ms of quiet). Avoids one server round-trip per
    // file in a large batch.
    remoteRefreshTimer_ = new QTimer(this);
    remoteRefreshTimer_->setSingleShot(true);
    remoteRefreshTimer_->setInterval(250);
    connect(remoteRefreshTimer_, &QTimer::timeout, this, [this] {
        RemoteFsModel* rm = currentRemote();
        if (rm && rm->isConnected()) rm->refresh();
    });
}

void MainWindow::restoreJournal() {
    // While a member session is locked we are on the guest (empty) list, so
    // restored jobs would have no credentials. Defer until the lock resolves.
    if (sessionLocked_ || journalRestored_) return;
    journalRestored_ = true;
    // The user may have hit the emergency brake while the startup unlock
    // dialog was open; in that case the journal was cleared on purpose.
    if (queueStopEpoch_ != 0) {
        queue_->clearJournal();
        return;
    }

    // Never silently auto-resume a huge batch: that is exactly what made a
    // mistaken drag of 17,000 files impossible to stop. Ask first, with the
    // safe "pause" choice as the default (Enter).
    std::string countErr;
    const std::size_t pendingJobs = queue_->pendingJournalCount(&countErr);
    if (!countErr.empty()) {
        statusBar()->showMessage(
            QString("读取传输记录失败：%1").arg(QString::fromStdString(countErr)),
            8000);
        return;
    }
    if (pendingJobs == 0) return;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("恢复传输队列"));
    box.setText(QStringLiteral("上次退出时有 %1 个未完成的传输任务。")
                    .arg(pendingJobs));
    box.setInformativeText(QStringLiteral(
        "任务很多时直接继续会让界面很卡。\n"
        "「暂停恢复」先把任务列出来但不传输；"
        "「丢弃并清空」彻底删除这些记录，下次启动不再出现。"));
    QPushButton* resumeBtn =
        box.addButton(QStringLiteral("恢复并继续"), QMessageBox::AcceptRole);
    QPushButton* pausedBtn =
        box.addButton(QStringLiteral("暂停恢复"), QMessageBox::ActionRole);
    QPushButton* discardBtn =
        box.addButton(QStringLiteral("丢弃并清空"), QMessageBox::DestructiveRole);
    box.setDefaultButton(pausedBtn);   // Enter = safest choice
    Q_UNUSED(resumeBtn);
    box.exec();
    QAbstractButton* chosen = box.clickedButton();
    const bool startPaused = (chosen == pausedBtn);
    if (chosen == discardBtn) {
        queue_->clearJournal();
        statusBar()->showMessage(
            QStringLiteral("已丢弃 %1 个旧传输任务，下次启动不会再恢复")
                .arg(pendingJobs), 8000);
        appendLog(QStringLiteral("已丢弃 %1 个旧传输任务").arg(pendingJobs));
        return;
    }
    if (startPaused) {
        queue_->pauseAll();
        if (pauseBtn_) pauseBtn_->setChecked(true);
    }

    // Fail closed up front: when the known_hosts store exists but cannot be
    // read, restored SFTP jobs would silently fall back to trust-on-first-
    // use. Refuse to restore at all in that case.
    {
        KnownHosts kh(KnownHosts::defaultPath());
        std::vector<KnownHostEntry> probe;
        std::string kerr;
        if (!kh.load(probe, &kerr)) {
            statusBar()->showMessage(
                QString("传输恢复已跳过：无法读取 known_hosts（%1）")
                    .arg(QString::fromStdString(kerr)),
                8000);
            return;
        }
    }
    std::string terr;
    const std::size_t restored = queue_->restore(&terr,
        [this](TransferRequest& r) {
            if (r.siteName.empty()) return;
            const QString name = QString::fromStdString(r.siteName);
            for (const SiteEntry& e : siteStore_.sites()) {
                if (e.name != name) continue;
                const SiteEntry full = siteStore_.withPassword(e);
                r.ssh.host = e.host.toStdString();
                r.ssh.port = (std::uint16_t)e.port;
                r.ssh.user = e.user.toStdString();
                r.ssh.password = full.password.toStdString();
                r.ssh.sftpCommand = e.isSftp ? e.sftpCmd.toStdString()
                                             : std::string();
                if (!e.keyPath.isEmpty())
                    r.ssh.privateKeyPem = readKeyFile(e.keyPath);
                r.ftp.host = r.ssh.host;
                r.ftp.port = r.ssh.port;
                r.ftp.user = r.ssh.user;
                r.ftp.password = r.ssh.password;
                r.pushCommandTemplate = e.pushCmd.toStdString();
                // Re-supply strict host-key verification from known_hosts.
                if (r.protocol == BackendKind::Sftp) {
                    KnownHosts kh(KnownHosts::defaultPath());
                    r.trustedHostKeyHex =
                        kh.trustedHexFor(r.ssh.host, r.ssh.port);
                }
                break;
            }
        });
    if (restored > 0) {
        statusBar()->showMessage(
            QString("已恢复 %1 个未完成的传输任务%2")
                .arg(restored)
                .arg(startPaused ? QStringLiteral("（已暂停，点「处理队列」开始）")
                                 : QString()), 8000);
        appendLog(QString("已恢复 %1 个传输任务%2")
                      .arg(restored)
                      .arg(startPaused ? QStringLiteral("（已暂停）")
                                       : QString()));
    }
    if (!terr.empty()) {
        statusBar()->showMessage(
            QString("传输恢复失败: %1").arg(QString::fromStdString(terr)),
            8000);
    }
}

MainWindow::~MainWindow() {
    // Join any in-flight folder scans first: their bodies use queue_ and
    // post completion callbacks to `this`. Future destructors block until
    // each task returns, so nothing outlives the queue or the window.
    treeWorkers_.clear();
    // The queue's callback captures `this`; destroy it before the widgets.
    queue_.reset();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
#ifdef Q_OS_WIN
    // Windows 11 native rounded corners (DWM). Harmless no-op on Windows 10
    // and earlier, where the attribute is unsupported and the OS ignores it.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    const DWM_WINDOW_CORNER_PREFERENCE pref =
        static_cast<DWM_WINDOW_CORNER_PREFERENCE>(DWMWCP_ROUND);
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref,
                          sizeof(pref));
#endif
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message,
                             qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCHITTEST) {
            // Let the OS treat the outer ~6px band as a resize border so the
            // frameless window stays resizable by dragging its edges.
            const LONG border = 6;
            RECT rc;
            GetWindowRect(msg->hwnd, &rc);
            const LONG x = GET_X_LPARAM(msg->lParam);
            const LONG y = GET_Y_LPARAM(msg->lParam);

            const bool left = x < rc.left + border;
            const bool right = x >= rc.right - border;
            const bool top = y < rc.top + border;
            const bool bottom = y >= rc.bottom - border;

            if (top && left) *result = HTTOPLEFT;
            else if (top && right) *result = HTTOPRIGHT;
            else if (bottom && left) *result = HTBOTTOMLEFT;
            else if (bottom && right) *result = HTBOTTOMRIGHT;
            else if (left) *result = HTLEFT;
            else if (right) *result = HTRIGHT;
            else if (top) *result = HTTOP;
            else if (bottom) *result = HTBOTTOM;
            else return false;   // default handling (title bar drag etc.)
            return true;
        }
    }
#else
    (void)eventType; (void)message; (void)result;
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::buildUi() {
    // --- local pane (left) --------------------------------------------------
    auto* leftPane = new QWidget(this);
    auto* leftLay = new QVBoxLayout(leftPane);
    leftLay->setContentsMargins(8, 8, 8, 8);
    leftLay->setSpacing(6);
    auto* leftTitle = new QLabel("本地", leftPane);
    leftTitle->setObjectName("paneTitle");

    // Local path bar lives inside the local pane (not a global top bar):
    // path input + browse + up, matching the remote pane's own address bar.
    auto* localBar = new QWidget(leftPane);
    auto* localBarLay = new QHBoxLayout(localBar);
    localBarLay->setContentsMargins(6, 4, 6, 4);
    pathEdit_ = new QLineEdit(localBar);
    pathEdit_->setPlaceholderText("本地目录（输入路径回车，或点浏览）");
    auto* browseBtn = new QPushButton("浏览...", localBar);
    auto* upBtn = new QPushButton("向上", localBar);
    localBarLay->addWidget(pathEdit_, 1);
    localBarLay->addWidget(browseBtn);
    localBarLay->addWidget(upBtn);

    connect(browseBtn, &QPushButton::clicked, this, &MainWindow::onOpen);
    connect(upBtn, &QPushButton::clicked, this, &MainWindow::onUp);
    connect(pathEdit_, &QLineEdit::returnPressed, this, [this] {
        showRoot(pathEdit_->text().toStdString());
    });
    model_ = new LocalFsModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(kDirFirstSortRole);
    left_ = new QTableView(leftPane);
    left_->setModel(proxy_);
    left_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    left_->verticalHeader()->setDefaultSectionSize(22);
    left_->setShowGrid(false);
    left_->setAlternatingRowColors(true);
    left_->setSelectionBehavior(QAbstractItemView::SelectRows);
    left_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Renames are inline (F2 / context menu / slow second click), so Qt's
    // automatic edit triggers must stay off: double-click keeps opening a
    // folder instead of starting an edit.
    left_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    left_->setItemDelegateForColumn(0, nameDelegate_);
    left_->setSortingEnabled(true);
    left_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    left_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    left_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    left_->setColumnWidth(1, 90);
    left_->setColumnWidth(2, 150);
    // 隐藏"压缩判定"列（本地列表只展示名称/大小/修改时间）
    left_->hideColumn(LocalFsModel::ColVerdict);
    left_->verticalHeader()->hide();
    left_->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    leftLay->addWidget(leftTitle);
    leftLay->addWidget(localBar);
    leftLay->addWidget(left_, 1);
    // Drag & drop: local pane is a drag source (uploads) and drop target
    // (downloads from remote).
    left_->setDragEnabled(true);
    left_->setAcceptDrops(true);
    left_->setDragDropMode(QAbstractItemView::DragDrop);
    left_->setDefaultDropAction(Qt::CopyAction);
    left_->viewport()->installEventFilter(this);
    left_->installEventFilter(this);   // Backspace -> up one directory
    left_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(left_, &QTableView::customContextMenuRequested, this,
            &MainWindow::onLocalContextMenu);
    auto* localRenameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), left_);
    localRenameShortcut->setContext(Qt::WidgetShortcut);
    connect(localRenameShortcut, &QShortcut::activated, this,
            &MainWindow::renameLocalSelected);
    connect(left_, &QTableView::doubleClicked, this,
            [this](const QModelIndex& idx) {
                onActivated(proxy_->mapToSource(idx));
            });
    connect(model_, &LocalFsModel::verdictsReady, this,
            &MainWindow::onVerdictsReady);
    // Surface local delete/rename/move/mkdir failures (previously silent).
    connect(model_, &LocalFsModel::operationError, this,
            [this](const QString& msg) {
                statusBar()->showMessage(msg, 8000);
                appendLog(QString("本地操作失败：%1").arg(msg));
            });

    // --- remote pane (right): tabbed, one session per tab ------------------
    auto* rightPane = new QWidget(this);
    auto* rightLay = new QVBoxLayout(rightPane);
    rightLay->setContentsMargins(8, 8, 8, 8);
    rightLay->setSpacing(6);
    auto* rightTitle = new QLabel("远端", rightPane);
    rightTitle->setObjectName("paneTitle");
    remoteTabs_ = new QTabWidget(rightPane);
    remoteTabs_->setDocumentMode(true);
    // Custom close buttons are added per session tab (a thin "x" glyph via
    // setTabButton), so the built-in closable buttons stay off to avoid the
    // default style's invisible/boxy icon.
    remoteTabs_->setTabsClosable(false);
    remoteTabs_->setMovable(true);
    // Suppress the tab bar's base line (the 1px rule drawn under the empty
    // tab-strip area) so no bright line shows beneath the "+" button.
    remoteTabs_->tabBar()->setDrawBase(false);
    // A trailing "+" tab acts as the new-session button. It sits right after
    // the last session tab (Chrome style), can't be closed or moved, and
    // clicking it opens a fresh session tab.
    connect(remoteTabs_, &QTabWidget::tabBarClicked, this,
            &MainWindow::onTabBarClicked);
    connect(remoteTabs_->tabBar(), &QTabBar::tabMoved, this,
            &MainWindow::onTabMoved);
    rightLay->addWidget(rightTitle);
    rightLay->addWidget(remoteTabs_, 1);
    connect(remoteTabs_, &QTabWidget::tabCloseRequested, this,
            &MainWindow::onTabClosed);
    // Keep the window/taskbar title in lock-step with the active session tab.
    connect(remoteTabs_, &QTabWidget::currentChanged, this,
            [this](int) { syncWindowTitle(); });
    addNewTabButton();   // "+" tab pinned to the end
    addRemoteTab();      // initial session tab

    // --- splitters -------------------------------------------------------------
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);

    // --- transfer dock (bottom) --------------------------------------------------
    transferDock_ = new QDockWidget("传输队列", this);
    auto* dockBody = new QWidget(transferDock_);
    auto* dockLay = new QVBoxLayout(dockBody);
    dockLay->setContentsMargins(4, 4, 4, 4);

    // Three buckets: queued/running, failed/canceled, and completed. A job's
    // row moves between buckets as its state changes.
    transferTabs_ = new QTabWidget(dockBody);
    transferTabs_->setDocumentMode(true);
    transferTabs_->tabBar()->setDrawBase(false);
    auto makeBucket = [&](const QString& title) {
        auto* list = new QListWidget(transferTabs_);
        list->setAlternatingRowColors(true);
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        list->setContextMenuPolicy(Qt::CustomContextMenu);
        transferTabs_->addTab(list, title);
        return list;
    };
    queueList_ = makeBucket("列队的文件");
    failedList_ = makeBucket("传输失败");
    doneList_ = makeBucket("成功的传输");

    // FileZilla-style message log: a read-only, monospaced transcript of
    // connection/transfer/error events with timestamps, living as a fourth
    // tab alongside the transfer buckets.
    logView_ = new QPlainTextEdit(transferTabs_);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(5000);   // bound memory on long sessions
    QFont lf = logView_->font();
    lf.setFamily("Consolas");
    lf.setPointSizeF(lf.pointSizeF() * 0.95);
    logView_->setFont(lf);
    logView_->setStyleSheet("color: #d4d4d4;");
    transferTabs_->addTab(logView_, "消息日志");
    dockLay->addWidget(transferTabs_, 1);

    auto* dockBar = new QWidget(dockBody);
    auto* dockBarLay = new QHBoxLayout(dockBar);
    dockBarLay->setContentsMargins(0, 2, 0, 0);
    dockBarLay->setSpacing(4);

    // FileZilla-style: the leading "process queue" toggle owns the whole
    // queue. Checked = paused, unchecked = processing. It drives pauseAll/
    // resumeAll on the queue rather than a per-job flag.
    pauseBtn_ = new QPushButton("处理队列", dockBar);
    pauseBtn_->setCheckable(true);
    pauseBtn_->setChecked(false);
    pauseBtn_->setToolTip(
        "开始/暂停整个传输队列（FileZilla 处理队列）。\n"
        "暂停时所有排队和传输中的文件都会停在当前进度。");
    cancelBtn_ = new QPushButton("取消", dockBar);
    cancelBtn_->setToolTip("取消选中的传输任务");
    clearDoneBtn_ = new QPushButton("移除", dockBar);
    clearDoneBtn_->setToolTip("移除已完成的传输记录");
    clearAllBtn_ = new QPushButton("清空全部", dockBar);
    clearAllBtn_->setToolTip("取消并清空整个传输队列");
    emergencyBtn_ = new QPushButton("紧急停止", dockBar);
    emergencyBtn_->setObjectName("emergencyStopBtn");
    emergencyBtn_->setStyleSheet(
        "QPushButton#emergencyStopBtn { color: #ff8a80; font-weight: bold; }");
    emergencyBtn_->setToolTip(
        "立即停止所有正在传输和排队的文件，并清空恢复记录。\n"
        "快捷键 Ctrl+Shift+X（队列面板隐藏时也有效）。");
    dockBarLay->addWidget(pauseBtn_);
    dockBarLay->addWidget(cancelBtn_);
    dockBarLay->addWidget(clearDoneBtn_);
    dockBarLay->addWidget(clearAllBtn_);
    dockBarLay->addWidget(emergencyBtn_);
    dockBarLay->addStretch(1);
    // Concurrent worker count (live: applies to the queue immediately).
    dockBarLay->addWidget(new QLabel("并发:", dockBar));
    workersSpin_ = new QSpinBox(dockBar);
    workersSpin_->setRange(1, 64);
    workersSpin_->setValue(5);   // default; ctor applies the persisted value
    workersSpin_->setToolTip(
        "同时传输的 worker 数（1-64）。调小会让多余 worker 跑完当前文件后退出。");
    dockBarLay->addWidget(workersSpin_);
    // Same-name destination policy. Applied to newly enqueued transfers.
    dockBarLay->addWidget(new QLabel("同名文件:", dockBar));
    conflictCombo_ = new QComboBox(dockBar);
    conflictCombo_->addItem("智能（不同才覆盖）", (int)ConflictPolicy::Smart);
    conflictCombo_->addItem("覆盖", (int)ConflictPolicy::Overwrite);
    conflictCombo_->addItem("跳过", (int)ConflictPolicy::Skip);
    conflictCombo_->setCurrentIndex(0);   // Smart by default
    conflictCombo_->setToolTip(
        "遇到同名文件时如何处理：\n"
        "· 智能：大小或修改时间不同才覆盖，完全相同则跳过\n"
        "· 覆盖：始终覆盖同名文件\n"
        "· 跳过：始终跳过已存在的同名文件");
    dockBarLay->addWidget(conflictCombo_);
    // Global transfer speed limit (FileZilla "速度限制"). 0 = unlimited.
    dockBarLay->addWidget(new QLabel("限速:", dockBar));
    rateLimitCombo_ = new QComboBox(dockBar);
    rateLimitCombo_->addItem("不限速", 0ULL);
    rateLimitCombo_->addItem("100 KB/s", 100ULL * 1024);
    rateLimitCombo_->addItem("256 KB/s", 256ULL * 1024);
    rateLimitCombo_->addItem("512 KB/s", 512ULL * 1024);
    rateLimitCombo_->addItem("1 MB/s", 1024ULL * 1024);
    rateLimitCombo_->addItem("2 MB/s", 2ULL * 1024 * 1024);
    rateLimitCombo_->addItem("5 MB/s", 5ULL * 1024 * 1024);
    rateLimitCombo_->setCurrentIndex(0);   // unlimited by default
    rateLimitCombo_->setToolTip(
        "全局传输限速（FileZilla 速度限制）。\n"
        "对上传和下载同时生效；不限速为 0。");
    dockBarLay->addWidget(rateLimitCombo_);
    dockLay->addWidget(dockBar);

    transferDock_->setWidget(dockBody);
    // Custom title bar: themed title + a close "x" button (the stock
    // QDockWidget close icon is disabled in the global stylesheet).
    {
        auto* dockTitle = new QWidget(transferDock_);
        dockTitle->setObjectName("dockTitleBar");
        auto* dockTitleLay = new QHBoxLayout(dockTitle);
        dockTitleLay->setContentsMargins(4, 2, 4, 2);
        dockTitleLay->setSpacing(4);
        auto* dockTitleLabel = new QLabel("传输队列", dockTitle);
        dockTitleLabel->setObjectName("titleBarText");
        auto* dockCloseBtn = new QToolButton(dockTitle);
        dockCloseBtn->setObjectName("dockCloseBtn");
        dockCloseBtn->setAutoRaise(true);
        dockCloseBtn->setCursor(Qt::PointingHandCursor);
        dockCloseBtn->setToolTip("关闭传输队列面板");
        dockCloseBtn->setIcon(makeCloseIcon());
        dockCloseBtn->setIconSize(QSize(16, 16));
        dockTitleLay->addWidget(dockTitleLabel, 1);
        dockTitleLay->addWidget(dockCloseBtn);
        transferDock_->setTitleBarWidget(dockTitle);
        connect(dockCloseBtn, &QToolButton::clicked,
                transferDock_, &QDockWidget::close);
    }
    transferDock_->setFeatures(QDockWidget::DockWidgetMovable |
                               QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, transferDock_);
    transferDock_->hide();   // appears with the first job

    connect(pauseBtn_, &QPushButton::clicked, this,
            &MainWindow::onToggleProcessQueue);
    connect(cancelBtn_, &QPushButton::clicked, this,
            &MainWindow::onCancelSelected);
    connect(clearDoneBtn_, &QPushButton::clicked, this,
            &MainWindow::onClearFinished);
    connect(clearAllBtn_, &QPushButton::clicked, this,
            &MainWindow::onClearAll);
    connect(emergencyBtn_, &QPushButton::clicked, this,
            &MainWindow::onEmergencyStop);
    // Application-wide so it still works when the queue dock is hidden or
    // focus sits in a file pane / terminal. This is the "I can't stop it!"
    // escape hatch; no confirmation dialog -- it must be instant.
    auto* stopShortcut = new QShortcut(QKeySequence("Ctrl+Shift+X"), this);
    stopShortcut->setContext(Qt::ApplicationShortcut);
    connect(stopShortcut, &QShortcut::activated, this,
            &MainWindow::onEmergencyStop);
    connect(workersSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        if (!queue_) return;   // UI is still being built
        queue_->setWorkers(static_cast<unsigned>(v));
        QSettings().setValue("transfer/workers", v);
        statusBar()->showMessage(
            QString("并发 worker 数已设为 %1").arg(v), 3000);
    });
    connect(rateLimitCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) {
        const auto bps = rateLimitCombo_->currentData().toULongLong();
        queue_->setRateLimit(bps);
        statusBar()->showMessage(
            bps == 0 ? "已取消传输限速"
                     : QString("全局限速已设为 %1").arg(
                           rateLimitCombo_->currentText()),
            3000);
    });
    for (QListWidget* list :
         {queueList_, failedList_, doneList_}) {
        connect(list, &QListWidget::customContextMenuRequested, this,
                &MainWindow::onTransferContextMenu);
    }

    auto* central = new QWidget(this);
    auto* centralLay = new QVBoxLayout(central);
    centralLay->setContentsMargins(0, 0, 0, 0);
    centralLay->setSpacing(0);
    centralLay->addWidget(splitter, 1);

    // Custom title bar sits above the whole workspace; the frameless window
    // is moved/resized through it (drag) and the native edge handling.
    titleBar_ = new TitleBar(this);
    titleBar_->attachWindow(this);

    auto* shell = new QWidget(this);
    auto* shellLay = new QVBoxLayout(shell);
    shellLay->setContentsMargins(0, 0, 0, 0);
    shellLay->setSpacing(0);
    shellLay->addWidget(titleBar_);
    shellLay->addWidget(central, 1);
    setCentralWidget(shell);

    // Membership / account entry pinned to the bottom-left corner
    // (WorkBuddy-style): an avatar + label button that pops a menu.
    accountBtn_ = new QToolButton(this);
    accountBtn_->setObjectName("accountBtn");
    accountBtn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    accountBtn_->setPopupMode(QToolButton::InstantPopup);
    accountBtn_->setCursor(Qt::PointingHandCursor);
    connect(accountBtn_, &QToolButton::clicked, this,
            &MainWindow::onAccountMenu);
    statusBar()->addWidget(accountBtn_, 0);

    // Transfer queue toggle: lets the user re-open the dock after it was
    // auto-hidden or closed, without starting a new transfer.
    auto* transferBtn = new QToolButton(this);
    transferBtn->setObjectName("accountBtn");
    transferBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    transferBtn->setText("传输队列");
    transferBtn->setCursor(Qt::PointingHandCursor);
    transferBtn->setToolTip("显示/隐藏传输队列 (Ctrl+J)");
    connect(transferBtn, &QToolButton::clicked, this,
            &MainWindow::toggleTransferDock);
    statusBar()->addWidget(transferBtn, 0);

    // Ctrl+J toggles the transfer queue as well.
    auto* transferShortcut = new QShortcut(QKeySequence("Ctrl+J"), this);
    connect(transferShortcut, &QShortcut::activated, this,
            &MainWindow::toggleTransferDock);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);

    const auto st = protocolEngineStatus();
    statusLabel_->setText(QString("engines: ftp=%1 ssh=%2 zstd=%3 "
                                  "exec-compression=%4")
                              .arg(st.ftpAvailable).arg(st.sshAvailable)
                              .arg(st.zstdAvailable)
                              .arg(st.execChannelCompression));

    // Restore the last browsed local directory; fall back to the home dir.
    QSettings settings;
    const QString savedDir = settings.value("local/lastDir").toString();
    QString home = qEnvironmentVariable("USERPROFILE");
    if (!savedDir.isEmpty() && QFileInfo(savedDir).isDir()) {
        home = savedDir;
    }
    if (!home.isEmpty()) {
        pathEdit_->setText(home);
        showRoot(home.toStdString());
    }
}

// ---- remote tabs -----------------------------------------------------------

MainWindow::RemoteTab* MainWindow::currentTab() {
    const int i = remoteTabs_->currentIndex();
    if (i < 0 || i >= tabs_.size()) return nullptr;
    return tabs_[i];
}

const MainWindow::RemoteTab* MainWindow::currentTab() const {
    const int i = remoteTabs_->currentIndex();
    if (i < 0 || i >= tabs_.size()) return nullptr;
    return tabs_[i];
}

RemoteFsModel* MainWindow::currentRemote() const {
    const RemoteTab* t = currentTab();
    return t ? t->model : nullptr;
}

void MainWindow::syncWindowTitle() {
    const QString app = QString("ftpflow %1").arg(versionString());
    QString title;
    // Prefer the current tab's label (site name / host once connected), so the
    // taskbar shows exactly which session is active -- FileZilla-style.
    const int idx = remoteTabs_ ? remoteTabs_->currentIndex() : -1;
    if (idx >= 0 && idx < remoteTabs_->count() &&
        idx != newTabIndex_) {
        const QString tabText = remoteTabs_->tabText(idx).trimmed();
        // A fresh tab is still "会话 N" (not connected): keep the neutral
        // brand title instead of echoing a meaningless placeholder.
        if (!tabText.isEmpty() && !tabText.startsWith(QStringLiteral("会话")))
            title = QString("%1 - %2").arg(tabText, app);
    }
    if (title.isEmpty()) title = QString("%1 - 文件传输工作台").arg(app);

    setWindowTitle(title);
    if (titleBar_) titleBar_->setTitle(title);
}

MainWindow::RemoteTab* MainWindow::addRemoteTab() {
    auto* tab = new RemoteTab;
    auto* page = new QWidget(remoteTabs_);
    tab->page = page;
    auto* pageLay = new QVBoxLayout(page);
    pageLay->setContentsMargins(0, 0, 0, 0);

    // Address bar + nav/connect buttons.
    auto* remoteBar = new QWidget(page);
    auto* remoteBarLay = new QHBoxLayout(remoteBar);
    remoteBarLay->setContentsMargins(2, 0, 2, 0);
    tab->pathEdit = new QLineEdit(remoteBar);
    tab->pathEdit->setPlaceholderText("未连接（点“连接...”打开远端）");
    tab->pathEdit->setReadOnly(true);
    tab->pathEdit->setClearButtonEnabled(true);
    tab->connectBtn = new QPushButton("连接...", remoteBar);
    tab->disconnectBtn = new QPushButton("断开", remoteBar);
    tab->upBtn = new QPushButton("向上", remoteBar);
    tab->refreshBtn = new QPushButton("刷新", remoteBar);
    tab->cmdBtn = new QPushButton("命令", remoteBar);
    tab->cmdBtn->setToolTip("在服务器上执行 shell 命令（复用当前连接）");
    tab->termBtn = new QPushButton("终端", remoteBar);
    tab->termBtn->setToolTip("打开交互式终端（SSH shell，支持 vi/top/颜色）");
    tab->disconnectBtn->setEnabled(false);
    tab->upBtn->setEnabled(false);
    tab->refreshBtn->setEnabled(false);
    tab->cmdBtn->setEnabled(false);
    tab->termBtn->setEnabled(false);
    remoteBarLay->addWidget(tab->pathEdit, 1);
    remoteBarLay->addWidget(tab->connectBtn);
    remoteBarLay->addWidget(tab->disconnectBtn);
    remoteBarLay->addWidget(tab->upBtn);
    remoteBarLay->addWidget(tab->refreshBtn);
    remoteBarLay->addWidget(tab->cmdBtn);
    remoteBarLay->addWidget(tab->termBtn);

    // Model/proxy are children of MainWindow (not the page) so they survive
    // independently; onTabClosed() deletes them explicitly.
    tab->model = new RemoteFsModel(this);
    tab->proxy = new QSortFilterProxyModel(this);
    tab->proxy->setSourceModel(tab->model);
    tab->proxy->setSortRole(kDirFirstSortRole);
    tab->table = new QTableView(page);
    tab->table->setModel(tab->proxy);
    tab->table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    tab->table->verticalHeader()->setDefaultSectionSize(22);
    tab->table->setShowGrid(false);
    tab->table->setAlternatingRowColors(true);
    tab->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    tab->table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Inline rename only (see the local pane); double-click still browses.
    tab->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tab->table->setItemDelegateForColumn(0, nameDelegate_);
    tab->table->setSortingEnabled(true);
    tab->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    // Give the size and modified-time columns a fixed, visible width instead
    // of letting the name column swallow the whole width.
    tab->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    tab->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    tab->table->setColumnWidth(1, 90);
    tab->table->setColumnWidth(2, 150);
    tab->table->verticalHeader()->hide();
    tab->table->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    pageLay->addWidget(remoteBar);

    // Vertical split: file list on top, interactive terminal below (hidden
    // until the user opens it).
    tab->termSplitter = new QSplitter(Qt::Vertical, page);
    tab->termSplitter->setChildrenCollapsible(false);
    tab->termSplitter->addWidget(tab->table);
    tab->terminal = new TerminalWidget(tab->termSplitter);
    tab->terminal->hide();
    tab->termSplitter->addWidget(tab->terminal);
    tab->termSplitter->setStretchFactor(0, 1);
    tab->termSplitter->setStretchFactor(1, 0);
    tab->termSplitter->setSizes({400, 160});
    pageLay->addWidget(tab->termSplitter, 1);

    // Drag & drop + F2 rename + context menu (per tab).
    tab->table->setDragEnabled(true);
    tab->table->setAcceptDrops(true);
    tab->table->setDragDropMode(QAbstractItemView::DragDrop);
    tab->table->setDefaultDropAction(Qt::CopyAction);
    tab->table->setContextMenuPolicy(Qt::CustomContextMenu);
    tab->table->viewport()->installEventFilter(this);
    tab->table->installEventFilter(this);   // Backspace -> up one directory
    auto* renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), tab->table);
    renameShortcut->setContext(Qt::WidgetShortcut);
    connect(renameShortcut, &QShortcut::activated, this,
            &MainWindow::renameRemoteSelected);

    // Wire this tab's widgets/models. Lambdas capture the stable `tab`
    // pointer so every signal targets the right tab, even a background one.
    connect(tab->pathEdit, &QLineEdit::returnPressed, this, [this, tab] {
        if (!tab->model->isConnected()) return;
        const QString p = tab->pathEdit->text().trimmed();
        if (p.isEmpty()) return;
        tab->model->enterDir(p.toStdString());
    });
    connect(tab->connectBtn, &QPushButton::clicked, this, [this, tab] {
        ConnectDialog dlg(this, &siteStore_);
        if (dlg.exec() != QDialog::Accepted) return;
        if (dlg.saveOnly()) return;   // "确定": saved & closed, no connection
        tab->connectBtn->setEnabled(false);
        tab->pathEdit->setText("连接中...");
        tab->model->connectTo(dlg.connInfo());
    });
    connect(tab->disconnectBtn, &QPushButton::clicked, this, [this, tab] {
        reconnectTimer_->stop();
        tab->model->disconnect();
        tab->connectBtn->setEnabled(true);
        tab->disconnectBtn->setEnabled(false);
        tab->pathEdit->setReadOnly(true);
        tab->pathEdit->setText("未连接");
    });
    connect(tab->upBtn, &QPushButton::clicked, this, [this, tab] {
        tab->model->goUp();
        tab->pathEdit->setText(QString::fromStdString(tab->model->currentPath()));
    });
    connect(tab->refreshBtn, &QPushButton::clicked, this, [this, tab] {
        tab->model->refresh();
    });
    connect(tab->cmdBtn, &QPushButton::clicked, this, [this, tab] {
        onRemoteCommand(*tab);
    });
    connect(tab->termBtn, &QPushButton::clicked, this, [this, tab] {
        onRemoteTerminal(*tab);
    });
    // Terminal I/O wiring: keyboard -> shell, shell output -> terminal.
    connect(tab->terminal, &TerminalWidget::dataReady, this,
            [tab](const QByteArray& d) {
                if (tab->shell) tab->shell->write(d);
            });
    connect(tab->terminal, &TerminalWidget::sizeChanged, this,
            [tab](int cols, int rows) {
                if (tab->shell) tab->shell->resize(cols, rows);
            });
    // OSC 0/2 title from the remote shell updates the tab label.
    connect(tab->terminal, &TerminalWidget::titleChanged, this,
            [this, tab](const QString& t) {
                const QString title = t.trimmed();
                if (title.isEmpty()) return;
                const int tabIdx = tabs_.indexOf(tab);
                if (tabIdx >= 0) remoteTabs_->setTabText(tabIdx, title);
                syncWindowTitle();
            });
    connect(tab->table, &QTableView::doubleClicked, this, [this, tab](
            const QModelIndex& i) {
        if (!i.isValid() || !tab->model->isConnected()) return;
        const QModelIndex src = tab->proxy->mapToSource(i);
        if (tab->model->isDir(src)) {
            tab->model->enterDir(tab->model->pathAt(src));
            tab->pathEdit->setText(
                QString::fromStdString(tab->model->currentPath()));
        }
    });
    connect(tab->table, &QTableView::customContextMenuRequested, this,
            [this, tab](const QPoint& pos) {
                onRemoteContextMenu(*tab, pos);
            });

    connect(tab->model, &RemoteFsModel::connected, this, [this, tab] {
        reconnectTimer_->stop();
        tab->connectBtn->setEnabled(false);
        tab->disconnectBtn->setEnabled(true);
        tab->upBtn->setEnabled(true);
        tab->refreshBtn->setEnabled(true);
        // Command execution + terminal only apply to SSH (FTP has no shell).
        tab->cmdBtn->setEnabled(tab->model->connInfo().isSftp);
        tab->termBtn->setEnabled(tab->model->connInfo().isSftp);
        tab->pathEdit->setReadOnly(false);
        tab->pathEdit->setText(QString::fromStdString(tab->model->currentPath()));
        // Rename the tab: prefer the saved site name, fall back to the
        // host (IP), appending the port only when it differs from default.
        const auto ci = tab->model->connInfo();
        const std::string& hostStr = ci.isSftp ? ci.ssh.host : ci.ftp.host;
        const int port = ci.isSftp ? ci.ssh.port : ci.ftp.port;
        const int defaultPort = ci.isSftp ? 22 : 21;
        QString title;
        if (!ci.siteName.empty()) {
            title = QString::fromStdString(ci.siteName);
        } else {
            title = QString::fromStdString(hostStr);
            if (port != defaultPort) title += QString(":%1").arg(port);
        }
        const int tabIdx = tabs_.indexOf(tab);
        if (tabIdx >= 0) remoteTabs_->setTabText(tabIdx, title);
        syncWindowTitle();
        refreshStatusBar();
        appendLog(QString("已连接到 %1").arg(title));
    });
    connect(tab->model, &RemoteFsModel::connectFailed, this,
            [this, tab](const QString& msg) {
        tab->connectBtn->setEnabled(true);
        tab->disconnectBtn->setEnabled(false);
        tab->pathEdit->setReadOnly(true);
        tab->pathEdit->setText("未连接");
        statusBar()->showMessage(msg, 8000);
        appendLog(QString("连接失败：%1").arg(msg));
        // Surface the message log so the user sees why it failed.
        if (transferDock_) { transferDock_->show(); transferDock_->raise(); }
        transferTabs_->setCurrentIndex(3);
    });
    connect(tab->model, &RemoteFsModel::hostKeyUnknown, this,
            [this, tab](const QString& fp, const QString& host, quint16 port,
                        const QString& keyHex) {
        onHostKeyUnknown(*tab, fp, host, port, keyHex);
    });
    connect(tab->model, &RemoteFsModel::browseFailed, this,
            [this](const QString& msg) { statusBar()->showMessage(msg, 8000); });
    connect(tab->model, &RemoteFsModel::opStarted, this,
            [this](const QString& msg) { beginRemoteOp(msg); });
    connect(tab->model, &RemoteFsModel::opProgress, this,
            [this](const QString& msg) { updateRemoteOp(msg); });
    connect(tab->model, &RemoteFsModel::opFinished, this,
            [this](const QString& msg) { endRemoteOp(msg, true); });
    connect(tab->model, &RemoteFsModel::opFailed, this,
            [this](const QString& msg) { endRemoteOp(msg, false); });
    connect(tab->model, &RemoteFsModel::connectionLost, this,
            [this, tab](const QString& msg) {
        tab->disconnectBtn->setEnabled(false);
        tab->pathEdit->setReadOnly(true);
        tab->pathEdit->setText("连接已断开，准备重连...");
        statusBar()->showMessage(
            QString("连接已断开：%1（自动重连中...）").arg(msg), 8000);
        appendLog(QString("连接已断开：%1（自动重连中...）").arg(msg));
        reconnectTimer_->start(1500);
    });
    connect(tab->model, &RemoteFsModel::disconnected, this, [this, tab] {
        tab->upBtn->setEnabled(false);
        tab->refreshBtn->setEnabled(false);
        tab->cmdBtn->setEnabled(false);
        tab->termBtn->setEnabled(false);
        tab->pathEdit->setText("未连接");
        // Tear down any open terminal shell.
        if (tab->shell) { tab->shell->stop(); }
    });

    const int idx = tabs_.size();
    tabs_.append(tab);
    // Insert before the trailing "+" tab so it always stays last. `idx`
    // equals the "+" tab's current index because every session tab occupies
    // indices 0..tabs_.size()-1 and the "+" tab sits at the end.
    remoteTabs_->insertTab(idx, page, QString("会话 %1").arg(idx + 1));
    newTabIndex_ = remoteTabs_->count() - 1;
    // Attach a thin theme-aware "x" close button (Chrome-style). It lives on
    // the tab bar so it moves with the tab when tabs are dragged.
    auto* closeBtn = new QToolButton(remoteTabs_->tabBar());
    closeBtn->setIcon(makeCloseIcon());
    closeBtn->setIconSize(QSize(16, 16));
    closeBtn->setAutoRaise(true);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setToolTip("关闭会话");
    closeBtn->setObjectName("tabCloseBtn");
    connect(closeBtn, &QToolButton::clicked, this,
            [this, guard = QPointer<QWidget>(page)] {
        if (!guard) return;   // page already gone
        const int i = remoteTabs_->indexOf(guard.data());
        if (i >= 0) onTabClosed(i);
    });
    remoteTabs_->tabBar()->setTabButton(idx, QTabBar::RightSide, closeBtn);
    remoteTabs_->setCurrentIndex(idx);
    return tab;
}

void MainWindow::onNewRemoteTab() {
    addRemoteTab();
}

// The trailing "+" tab is the new-session button. Clicking it opens a fresh
// session and keeps focus off the empty "+" page.
void MainWindow::onTabBarClicked(int index) {
    if (index != newTabIndex_) return;   // a normal session tab
    // Defer so the tab widget finishes its own bookkeeping first; then open a
    // new session and focus it instead of the empty "+" page.
    QTimer::singleShot(0, this, &MainWindow::onNewRemoteTab);
}

// Keep the "+" tab pinned to the last position even if the user drags tabs
// around (movable is enabled), and keep `tabs_` in the same order as the tab
// bar -- currentIndex() is used to index `tabs_`, so a drag that reorders the
// bar without reordering `tabs_` would make every action (F2 / disconnect /
// retry / drops) hit the WRONG session. After any move: re-anchor the "+" tab,
// then rebuild `tabs_` in visual order (identity via each tab's page widget).
void MainWindow::onTabMoved(int /*from*/, int /*to*/) {
    if (syncingTabs_) return;   // moveTab() below re-enters this handler
    syncingTabs_ = true;
    const int count = remoteTabs_->count();
    // Locate the "+" page and re-anchor it last.
    int plusIdx = -1;
    for (int i = 0; i < count; ++i) {
        if (remoteTabs_->widget(i) == plusPage_) { plusIdx = i; break; }
    }
    if (plusIdx >= 0 && plusIdx != count - 1) {
        remoteTabs_->tabBar()->moveTab(plusIdx, count - 1);
        plusIdx = count - 1;
    }
    newTabIndex_ = plusIdx;
    // Rebuild tabs_ in visual order (session tabs only, "+" skipped).
    QList<RemoteTab*> order;
    order.reserve(tabs_.size());
    for (int i = 0; i < count; ++i) {
        if (i == plusIdx) continue;
        QWidget* w = remoteTabs_->widget(i);
        for (RemoteTab* t : tabs_) {
            if (t->page == w) { order.append(t); break; }
        }
    }
    if (order.size() == tabs_.size()) tabs_ = order;
    syncingTabs_ = false;
}

void MainWindow::onTabClosed(int index) {
    // A pending slow-click rename may target this tab's view; drop it before
    // the view is deleted so the timer never touches a dangling pointer.
    cancelPendingRename();
    if (index == newTabIndex_) return;   // never close the "+" tab
    if (tabs_.size() <= 1) return;   // keep at least one tab
    RemoteTab* t = tabs_[index];
    tabs_.removeAt(index);
    // removeTab() only detaches the page -- it does NOT delete it (the old
    // comment here claimed otherwise and every closed tab leaked its whole
    // widget tree). Tear down in dependency order: the page (and its views)
    // must die BEFORE the models those views point at.
    remoteTabs_->removeTab(index);
    if (t->shell) { t->shell->stop(); delete t->shell; t->shell = nullptr; }
    delete t->page;
    t->page = nullptr;
    t->proxy->setSourceModel(nullptr);
    delete t->proxy;
    delete t->model;
    delete t;
    // The "+" tab is always the last one; recompute instead of arithmetic.
    newTabIndex_ = remoteTabs_->count() - 1;
}

void MainWindow::addNewTabButton() {
    // A trailing "+" tab acts as the new-session button: it follows the last
    // session tab (Chrome style), drawn as a compact square with a small
    // glyph and no text.
    auto* page = new QWidget(remoteTabs_);
    plusPage_ = page;
    newTabIndex_ = remoteTabs_->addTab(page, makePlusIcon(), QString());
    remoteTabs_->setTabToolTip(newTabIndex_, "新建会话标签");
    // The "+" tab is a button, not a session: hide its close button. Its
    // compact square look (with a gap before it) is styled in theme.hpp via
    // QTabBar::tab:last.
    remoteTabs_->tabBar()->setTabButton(newTabIndex_, QTabBar::RightSide,
                                        nullptr);
}

QIcon MainWindow::makePlusIcon() const {
    // Draw a crisp "+" glyph sized for the tab bar and theme-aware (the
    // light/dark tint is baked into the theme's palette text color).
    const int s = 16;   // device-independent logical px; Qt scales for DPI
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c = remoteTabs_->palette().color(QPalette::Text);
    QPen pen(c, 2.0, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    const qreal m = 4.0;          // margin
    const qreal mid = s / 2.0;
    p.drawLine(QPointF(mid, m), QPointF(mid, s - m));     // vertical
    p.drawLine(QPointF(m, mid), QPointF(s - m, mid));     // horizontal
    p.end();
    return QIcon(pm);
}

QIcon MainWindow::makeCloseIcon() const {
    // A thin "x" close glyph, theme-aware like the reference Chrome tabs:
    // a muted cross with no background box (the hover highlight is drawn by
    // the button's own QSS background).
    const int s = 16;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c = remoteTabs_->palette().color(QPalette::Text);
    QColor col = c;
    col.setAlpha(170);   // slightly muted, still clearly visible
    QPen pen(col, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    const qreal m = 4.5;
    p.drawLine(QPointF(m, m), QPointF(s - m, s - m));
    p.drawLine(QPointF(s - m, m), QPointF(m, s - m));
    p.end();
    return QIcon(pm);
}

void MainWindow::persistLocalDir() {
    // Keep the path bar in sync with the current directory, and persist it
    // so the next launch restores the last browsed location.
    pathEdit_->setText(QString::fromStdString(model_->currentDirFullPath()));
    QSettings().setValue("local/lastDir",
                         QString::fromStdString(model_->currentDirFullPath()));
}

void MainWindow::showRoot(const std::string& root) {
    std::string err;
    if (!model_->openRoot(root, &err)) {
        statusBar()->showMessage(
            QString("打开失败: %1").arg(QString::fromStdString(err)), 5000);
        return;
    }
    // openRoot cleared currentDir, so currentDirFullPath() == root; the
    // path bar is synced + persisted by persistLocalDir() below.
    persistLocalDir();
    refreshStatusBar();
}

void MainWindow::onOpen() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, "选择本地目录", pathEdit_->text());
    if (!dir.isEmpty()) {
        showRoot(dir.toStdString());
    }
}

void MainWindow::onUp() {
    if (model_->goUp()) {
        persistLocalDir();
        refreshStatusBar();
    }
}

void MainWindow::onActivated(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    if (model_->isParentRow(idx)) {
        if (model_->goUp()) {
            persistLocalDir();
            refreshStatusBar();
        }
        return;
    }
    if (model_->isDir(idx)) {
        model_->enterDir(model_->relativeAt(idx));
        persistLocalDir();
        refreshStatusBar();
        return;
    }
    // A plain file: open it with the OS default association.
    const QString path = model_->pathAt(idx);
    if (!path.isEmpty()) {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
            statusBar()->showMessage("无法打开文件: " + path, 5000);
        }
    }
}

void MainWindow::onVerdictsReady() {
    refreshStatusBar();
}

// ---- remote pane (per-tab handlers) ---------------------------------------

void MainWindow::onHostKeyUnknown(RemoteTab& tab, const QString& fingerprint,
                                  const QString& host, quint16 port,
                                  const QString& keyHex) {
    tab.connectBtn->setEnabled(true);
    tab.disconnectBtn->setEnabled(false);
    tab.pathEdit->setReadOnly(true);
    tab.pathEdit->setText("未连接");
    // A key listed as @revoked must never be offered for acceptance.
    {
        KnownHosts kh(KnownHosts::defaultPath());
        if (kh.isRevoked(host.toStdString(), port,
                         KnownHosts::hexDecode(keyHex.toStdString()))) {
            QMessageBox::critical(
                this, "主机密钥已吊销",
                QString("主机 %1:%2 的密钥已被标记为吊销（@revoked），"
                        "已拒绝连接。\n\n如需重新信任，请先从 known_hosts "
                        "中移除该吊销记录。\n\n指纹 (SHA256):\n%3")
                    .arg(host)
                    .arg(port)
                    .arg(fingerprint));
            return;
        }
    }
    const QString detail =
        QString("主机 %1:%2 的 SSH 密钥尚未受信任。\n\n"
                "指纹 (SHA256):\n%3\n\n"
                "请确认指纹与服务器一致后信任；若不一致请立即断开，"
                "可能存在中间人攻击。")
            .arg(host)
            .arg(port)
            .arg(fingerprint);
    const auto choice = QMessageBox::warning(
        this, "主机密钥确认", detail,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice == QMessageBox::Yes) {
        tab.model->trustHostKey(host, port, keyHex);
        tab.connectBtn->setEnabled(false);
        tab.pathEdit->setText("连接中...");
    }
}

void MainWindow::onRemoteCommand(RemoteTab& tab) {
    if (!tab.model->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    const auto ci = tab.model->connInfo();
    if (!ci.isSftp) {
        statusBar()->showMessage("命令执行仅支持 SSH/SFTP 连接", 4000);
        return;
    }
    CommandDialog dlg(ci, this);
    dlg.exec();
}

void MainWindow::onRemoteTerminal(RemoteTab& tab) {
    if (!tab.model->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    const auto ci = tab.model->connInfo();
    if (!ci.isSftp) {
        statusBar()->showMessage("交互式终端仅支持 SSH/SFTP 连接", 4000);
        return;
    }

    if (tab.termVisible) {
        // Collapse the terminal.
        tab.termVisible = false;
        tab.terminal->hide();
        tab.termBtn->setText("终端");
        if (tab.shell) { tab.shell->stop(); }
        return;
    }

    // Expand and (re)start the shell.
    tab.termVisible = true;
    tab.terminal->show();
    tab.termBtn->setText("关闭终端");
    tab.termSplitter->setSizes({tab.termSplitter->height() / 2,
                                tab.termSplitter->height() / 2});

    // (Re)create the shell for this connection.
    if (tab.shell) {
        tab.shell->disconnect();
        tab.shell->deleteLater();
        tab.shell = nullptr;
    }
    tab.shell = new RemoteShell(ci, this);
    // NB: capture the tab as a POINTER. Capturing `RemoteTab&` by value
    // would copy the struct, so `tp->shell = nullptr` below would only
    // clear the copy and leave a dangling shell pointer in the real tab
    // (crash on the next terminal open).
    RemoteTab* tp = &tab;
    RemoteShell* shell = tab.shell;
    connect(shell, &RemoteShell::output, tab.terminal,
            &TerminalWidget::feed);
    connect(shell, &RemoteShell::finished, this,
            [this, tp, shell](const QString& msg) {
                if (!msg.isEmpty()) {
                    statusBar()->showMessage(msg, 8000);
                }
                if (tp->shell == shell) {
                    // The live shell ended; collapse the terminal view.
                    tp->termBtn->setText("终端");
                    tp->termVisible = false;
                    tp->terminal->hide();
                    tp->shell = nullptr;
                }
                // Always dispose of the shell that emitted this signal
                // (safe to call deleteLater more than once).
                shell->deleteLater();
            });
    tab.shell->start(tab.terminal->columns(), tab.terminal->rows());
    tab.terminal->setFocus();
}

void MainWindow::onRemoteContextMenu(RemoteTab& tab, const QPoint& pos) {
    if (!tab.model->isConnected()) return;
    const QModelIndex proxyIdx = tab.table->indexAt(pos);
    const QModelIndex idx = proxyIdx.isValid()
        ? tab.proxy->mapToSource(proxyIdx) : QModelIndex();

    QMenu menu(this);
    QAction* downloadAction = menu.addAction("下载到本地");
    QAction* editAction = menu.addAction("编辑");
    menu.addSeparator();
    QAction* mkdirAction = menu.addAction("新建文件夹...");
    QAction* renameAction = menu.addAction("重命名");
    QAction* removeAction = menu.addAction("删除...");
    menu.addSeparator();
    QAction* copyPathAction = menu.addAction("复制路径");
    QAction* copyNameAction = menu.addAction("复制文件名");
    menu.addSeparator();
    QAction* refreshAction = menu.addAction("刷新");

    const bool onEntry = idx.isValid();
    const bool isParent = idx.isValid() && tab.model->isParentRow(idx);
    if (!onEntry || isParent) {
        downloadAction->setEnabled(false);
        renameAction->setEnabled(false);
        removeAction->setEnabled(false);
        copyPathAction->setEnabled(false);
        copyNameAction->setEnabled(false);
    }
    // Edit: only a single, non-directory text file.
    editAction->setEnabled(onEntry && !isParent &&
        !tab.model->isDir(idx) &&
        isTextFile(QString::fromStdString(tab.model->nameAt(idx))));

    QAction* chosen = menu.exec(tab.table->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == downloadAction) {
        // Collect the selected remote paths and hand them to the queue.
        QModelIndexList rows = tab.table->selectionModel()->selectedRows(0);
        if (rows.isEmpty()) rows = {proxyIdx};
        QStringList paths;
        for (const QModelIndex& r : rows) {
            const QModelIndex src = tab.proxy->mapToSource(r);
            if (!src.isValid()) continue;
            const QString p = QString::fromStdString(tab.model->pathAt(src));
            if (p.isEmpty()) continue;
            // Directories need a trailing slash so the enqueue path knows to
            // treat them as folders (matches the drag-source convention).
            paths << (tab.model->isDir(src) ? p + '/' : p);
        }
        if (!paths.isEmpty()) {
            // Make this tab current so enqueueRemoteToLocal targets the
            // right session, then hand off to the transfer queue.
            const int ti = tabs_.indexOf(&tab);
            if (ti >= 0) remoteTabs_->setCurrentIndex(ti);
            enqueueRemoteToLocal(paths);
        }
    } else if (chosen == editAction) {
        const QString rp = QString::fromStdString(tab.model->pathAt(idx));
        editRemoteFile(tab, rp);
    } else if (chosen == mkdirAction) {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "新建文件夹", "文件夹名称:", QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty()) {
            tab.model->makeDir(name.trimmed().toStdString());
        }
    } else if (chosen == renameAction) {
        // Inline edit exactly the row that was right-clicked.
        beginInlineRename(tab.table, proxyIdx);
    } else if (chosen == removeAction) {
        // Fall back to the clicked row when nothing is selected, so the
        // "删除..." entry works on a plain right-click too.
        if (tab.table->selectionModel()->selectedRows(0).isEmpty())
            tab.table->selectionModel()->select(proxyIdx,
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
        deleteRemoteSelected(tab);
    } else if (chosen == copyPathAction) {
        if (idx.isValid()) {
            QApplication::clipboard()->setText(
                QString::fromStdString(tab.model->pathAt(idx)));
        }
    } else if (chosen == copyNameAction) {
        if (idx.isValid()) {
            QApplication::clipboard()->setText(
                QString::fromStdString(tab.model->nameAt(idx)));
        }
    } else if (chosen == refreshAction) {
        tab.model->refresh();
    }
}

// Every rename entry point (F2, context menu, slow second click on a
// selected name) ends up here. The name cell itself becomes a QLineEdit --
// there is no modal "new name" dialog anywhere in the app.
bool MainWindow::beginInlineRename(QAbstractItemView* view,
                                   const QModelIndex& proxyIdx) {
    cancelPendingRename();
    if (!view || !view->model() || !proxyIdx.isValid()) return false;
    const QModelIndex nameIdx = view->model()->index(proxyIdx.row(), 0);
    // The models expose EditRole only for real entries in the name column;
    // the synthetic ".." row and the size/date columns are therefore skipped.
    if (!nameIdx.isValid() ||
        !(nameIdx.flags() & Qt::ItemIsEditable) ||
        !nameIdx.data(Qt::EditRole).isValid()) {
        statusBar()->showMessage(QStringLiteral("当前条目不能重命名"), 3000);
        return false;
    }
    view->setCurrentIndex(nameIdx);
    view->scrollTo(nameIdx);
    // edit() consults editTriggers on some Qt builds. The panes keep all
    // automatic triggers off (so double-click still opens a folder), so
    // temporarily allow them for this one programmatic edit and restore
    // immediately: the editor is created synchronously, no events run in
    // between.
    const QAbstractItemView::EditTriggers saved = view->editTriggers();
    view->setEditTriggers(QAbstractItemView::AllEditTriggers);
    // Qt 6's public edit(index) slot returns void (and state() is
    // protected), so confirm the inline editor was actually created.
    view->edit(nameIdx);
    view->setEditTriggers(saved);
    if (!view->viewport()->findChild<QLineEdit*>()) {
        statusBar()->showMessage(QStringLiteral("无法开始重命名"), 3000);
        return false;
    }
    return true;
}

void MainWindow::cancelPendingRename() {
    if (renameClickTimer_) renameClickTimer_->stop();
    renameView_ = nullptr;
    renameRow_ = -1;
    renameCandidate_ = false;
}

void MainWindow::onRenameClickTimeout() {
    QAbstractItemView* view = renameView_;
    const int row = renameRow_;
    cancelPendingRename();
    if (!view || row < 0) return;
    // A remote tab can be closed while the timer is pending. Verify the view
    // is still one of ours BEFORE touching any of its methods (dangling
    // pointer otherwise).
    bool alive = (view == left_);
    for (RemoteTab* t : tabs_) {
        if (t && t->table == view) { alive = true; break; }
    }
    if (!alive || !view->isVisible() || !view->model() ||
        !view->selectionModel())
        return;
    const QModelIndex idx = view->model()->index(row, 0);
    if (!idx.isValid() || !view->selectionModel()->isSelected(idx) ||
        view->selectionModel()->selectedRows(0).size() != 1)
        return;
    beginInlineRename(view, idx);
}

void MainWindow::renameRemoteSelected() {
    RemoteTab* tab = currentTab();
    if (!tab || !tab->model->isConnected()) return;
    const QModelIndex proxyIdx = tab->table->currentIndex();
    if (!proxyIdx.isValid()) return;
    beginInlineRename(tab->table, proxyIdx);
}

// ---- text-file editing ----------------------------------------------------

void MainWindow::editLocalFile(const QString& localPath) {
    if (localPath.isEmpty()) return;
    // Local files are already on disk: hand the path to the OS so it opens
    // the user's default editor for that file type.
    const QUrl url = QUrl::fromLocalFile(QDir::toNativeSeparators(localPath));
    if (!QDesktopServices::openUrl(url)) {
        statusBar()->showMessage("无法打开编辑器", 4000);
    }
}

void MainWindow::editRemoteFile(RemoteTab& tab, const QString& remotePath) {
    if (remotePath.isEmpty()) return;
    // Stage a temp copy next to the original name so the editor shows a
    // recognizable filename (and syntax highlighting picks the right type).
    const QString base = baseNameOf(remotePath);
    const QString dir = QDir::temp().filePath("ftpflow-edit");
    QDir().mkpath(dir);
    const QString local = dir + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces)
                          + '-' + base;

    const auto ci = tab.model->connInfo();
    const QString user = QString::fromStdString(ci.isSftp ? ci.ssh.user
                                                          : ci.ftp.user);
    const QString host = QString::fromStdString(ci.isSftp ? ci.ssh.host
                                                          : ci.ftp.host);
    const QString userAtHost = user.isEmpty() ? host
                                              : user + '@' + host;
    const QString remoteDir = parentDirOf(remotePath);

    // When the download lands, open the default editor and watch for its exit
    // so we can prompt the user whether to push the edits back. The connection
    // is one-shot: without it, re-editing the same remote path accumulates
    // stale lambdas whose captured (already-deleted) temp path then fires on
    // the next download, launching the editor at a file that no longer exists
    // ("Windows cannot find the file").
    connect(tab.model, &RemoteFsModel::editDownloaded, this,
            [this, local, remotePath, userAtHost, remoteDir](
                const QString& rp, const QString& /*lp*/) {
                if (rp != remotePath) return;
                auto* proc = new QProcess(this);
                PendingEdit pe;
                pe.proc = proc;
                pe.localPath = local;
                pe.remotePath = remotePath;
                pe.remoteHost = userAtHost;
                pe.remoteDir = remoteDir;
                pendingEdits_.append(pe);

                connect(proc, &QProcess::finished, this,
                        [this, remotePath](int, QProcess::ExitStatus) {
                            onEditUploaded(remotePath);
                        });
                // Windows: launch through cmd so the child process lives as
                // long as the associated editor (we can then detect its exit).
#ifdef Q_OS_WIN
                proc->start("cmd", {"/c", "start", "/wait", "",
                                    QDir::toNativeSeparators(local)});
#else
                proc->start("xdg-open", {local});
#endif
                statusBar()->showMessage(
                    QString("正在编辑 %1（关闭编辑器后确认是否回传）")
                        .arg(remotePath), 6000);
            },
            Qt::SingleShotConnection);

    tab.model->downloadFile(remotePath.toStdString(), local.toStdString());
}

void MainWindow::onEditDownloaded(const QString& /*remotePath*/,
                                  const QString& /*localPath*/) {
    // Reserved; the per-file connect() in editRemoteFile drives the flow.
}

void MainWindow::onEditUploaded(const QString& remotePath) {
    for (int i = 0; i < pendingEdits_.size(); ++i) {
        if (pendingEdits_[i].remotePath != remotePath) continue;
        PendingEdit pe = pendingEdits_.takeAt(i);
        if (pe.proc) pe.proc->deleteLater();

        // Ask the user (FileZilla-style) whether to push the edits back,
        // with an option to drop the local temp copy.
        RemoteTab* tab = currentTab();
        const bool canUpload = tab && tab->model->isConnected();

        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("文件已改变"));
        box.setText(QStringLiteral("之前打开的文件被修改。"));
        box.setInformativeText(
            QStringLiteral("文件名： %1\n服务器： %2\n远程路径： %3")
                .arg(baseNameOf(pe.remotePath), pe.remoteHost, pe.remoteDir));

        auto* delLocal = new QCheckBox(
            QStringLiteral("结束编辑并删除本地文件"), &box);
        delLocal->setChecked(false);
        box.setCheckBox(delLocal);

        auto* yesBtn = box.addButton(QStringLiteral("是(&Y)"),
                                     QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("否(&N)"), QMessageBox::RejectRole);
        box.setDefaultButton(yesBtn);
        box.exec();

        const bool doUpload = (box.clickedButton() == yesBtn);
        const bool doDelete = delLocal->isChecked();

        if (doUpload && canUpload) {
            const QString rp = pe.remotePath;
            const QString lp = pe.localPath;
            connect(tab->model, &RemoteFsModel::editUploaded, this,
                    [this, rp](const QString& done) {
                        if (done != rp) return;
                        statusBar()->showMessage(
                            QString("已保存 %1").arg(rp), 4000);
                        if (RemoteTab* t = currentTab()) t->model->refresh();
                    },
                    Qt::SingleShotConnection);
            tab->model->uploadFile(lp.toStdString(), rp.toStdString());
        } else if (doUpload && !canUpload) {
            statusBar()->showMessage("会话已断开，未能回传编辑内容", 5000);
        }

        // The temp staging copy is always cleaned up. If the user asked to
        // delete it and we are not uploading, drop it right away; otherwise
        // give the async upload a short grace period so the file is still
        // available when the queue reads it. Only the single temp file is
        // removed -- the shared ftpflow-edit dir is left in place (other
        // edits may still be staging files there).
        const QString tmp = pe.localPath;
        const int delay = (doDelete && !doUpload) ? 0 : 5000;
        QTimer::singleShot(delay, this, [tmp] {
            QFile::remove(tmp);
        });
        return;
    }
}

void MainWindow::onLocalContextMenu(const QPoint& pos) {
    const QModelIndex proxyIdx = left_->indexAt(pos);
    const QModelIndex idx = proxyIdx.isValid()
        ? proxy_->mapToSource(proxyIdx) : QModelIndex();

    QMenu menu(this);
    QAction* uploadAction = menu.addAction("上传到远端");
    QAction* editAction = menu.addAction("编辑");
    menu.addSeparator();
    QAction* mkdirAction = menu.addAction("新建文件夹...");
    QAction* renameAction = menu.addAction("重命名");
    QAction* removeAction = menu.addAction("删除...");
    menu.addSeparator();
    QAction* copyPathAction = menu.addAction("复制路径");
    QAction* copyNameAction = menu.addAction("复制文件名");
    QAction* revealAction = menu.addAction("在资源管理器中打开");
    menu.addSeparator();
    QAction* refreshAction = menu.addAction("刷新");

    const bool onEntry = idx.isValid();
    const bool isParent = idx.isValid() && model_->isParentRow(idx);
    if (!onEntry || isParent) {
        uploadAction->setEnabled(false);
        renameAction->setEnabled(false);
        removeAction->setEnabled(false);
        copyPathAction->setEnabled(false);
        copyNameAction->setEnabled(false);
    }
    // Upload requires a connected remote session.
    if (!currentRemote() || !currentRemote()->isConnected()) {
        uploadAction->setEnabled(false);
    }
    // Edit: only a single, non-directory text file.
    editAction->setEnabled(onEntry && !isParent &&
        !model_->isDir(idx) && isTextFile(model_->pathAt(idx)));

    QAction* chosen = menu.exec(left_->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == editAction) {
        editLocalFile(model_->pathAt(idx));
    } else if (chosen == uploadAction) {
        QModelIndexList rows = left_->selectionModel()->selectedRows(0);
        if (rows.isEmpty()) rows = {proxyIdx};
        QStringList paths;
        for (const QModelIndex& r : rows) {
            const QModelIndex src = proxy_->mapToSource(r);
            if (!src.isValid()) continue;
            const QString p = model_->pathAt(src);
            if (!p.isEmpty()) paths << p;
        }
        if (!paths.isEmpty()) enqueueLocalToRemote(paths);
    } else if (chosen == mkdirAction) {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, "新建文件夹", "文件夹名称:", QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty()) {
            model_->makeDir(name.trimmed().toStdString());
        }
    } else if (chosen == renameAction) {
        // Inline edit exactly the row that was right-clicked.
        beginInlineRename(left_, proxyIdx);
    } else if (chosen == removeAction) {
        // Fall back to the clicked row when nothing is selected.
        if (left_->selectionModel()->selectedRows(0).isEmpty())
            left_->selectionModel()->select(proxyIdx,
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
        deleteLocalSelected();
    } else if (chosen == copyPathAction) {
        if (idx.isValid()) {
            QApplication::clipboard()->setText(model_->pathAt(idx));
        }
    } else if (chosen == copyNameAction) {
        if (idx.isValid()) {
            // The display name is in column 0; derive it from the path.
            const QString p = model_->pathAt(idx);
            const int slash = p.lastIndexOf(QChar('/'));
            const int bslash = p.lastIndexOf(QChar('\\'));
            const int cut = qMax(slash, bslash);
            QApplication::clipboard()->setText(
                cut >= 0 ? p.mid(cut + 1) : p);
        }
    } else if (chosen == revealAction) {
        // Open Explorer on the selected file (selected) or the current dir.
        const QString target = idx.isValid() && !model_->isParentRow(idx)
            ? model_->pathAt(idx)
            : QString::fromStdString(model_->currentDirFullPath());
        if (!target.isEmpty()) {
#ifdef Q_OS_WIN
            // /select highlights the file; fall back to the directory itself
            // when targeting a folder or an empty selection.
            if (idx.isValid() && !model_->isDir(idx)) {
                QProcess::startDetached("explorer.exe",
                                        {"/select,", QDir::toNativeSeparators(target)});
            } else {
                QDesktopServices::openUrl(
                    QUrl::fromLocalFile(QDir::toNativeSeparators(target)));
            }
#else
            QDesktopServices::openUrl(QUrl::fromLocalFile(target));
#endif
        }
    } else if (chosen == refreshAction) {
        model_->refresh();
    }
}

void MainWindow::renameLocalSelected() {
    const QModelIndex proxyIdx = left_->currentIndex();
    if (!proxyIdx.isValid()) return;
    beginInlineRename(left_, proxyIdx);
}

// ---- remote operation feedback ---------------------------------------------
// Recursive deletes / moves run on the model's worker thread and can take a
// while. Without a visible busy state the window looks dead, so every op
// shows a status-bar message plus a small indeterminate progress bar until
// opFinished / opFailed arrives.

void MainWindow::beginRemoteOp(const QString& message) {
    ++opBusyCount_;
    if (!opBusy_) {
        opBusy_ = new QProgressBar(this);
        opBusy_->setRange(0, 0);          // indeterminate "working" bar
        opBusy_->setTextVisible(false);
        opBusy_->setFixedWidth(110);
        opBusy_->setFixedHeight(12);
        opBusy_->setObjectName("opBusyBar");
        opBusy_->setToolTip("远程操作进行中…");
        statusBar()->addPermanentWidget(opBusy_);
    }
    opBusy_->show();
    statusBar()->showMessage(message);
}

void MainWindow::updateRemoteOp(const QString& message) {
    if (opBusyCount_ <= 0) return;
    statusBar()->showMessage(message);
}

void MainWindow::endRemoteOp(const QString& message, bool ok) {
    if (opBusyCount_ > 0) --opBusyCount_;
    if (opBusyCount_ == 0 && opBusy_) opBusy_->hide();
    if (message.isEmpty()) return;
    if (ok) {
        statusBar()->showMessage(message, 5000);
        appendLog(message);
    } else {
        statusBar()->showMessage(message, 10000);
        appendLog(QString("操作失败：%1").arg(message));
    }
}

void MainWindow::deleteLocalSelected() {
    QModelIndexList rows = left_->selectionModel()->selectedRows(0);
    QStringList names;
    QModelIndexList srcRows;
    for (const QModelIndex& r : rows) {
        const QModelIndex src = proxy_->mapToSource(r);
        if (!src.isValid()) continue;
        if (model_->isParentRow(src)) continue;   // never delete ".."
        srcRows.push_back(src);
        names << model_->pathAt(src);
    }
    if (srcRows.isEmpty()) {
        statusBar()->showMessage("未选中本地条目", 3000);
        return;
    }
    const QString message = QString(
        "即将从本地删除 %1 个条目（文件夹将递归删除，不可恢复）：\n\n%2\n\n"
        "确定删除吗？")
        .arg(names.size())
        .arg(names.join('\n').left(400));
    if (confirmDeletion(this, message)) {
        // Synchronous recursive delete: show a wait cursor (the local pane
        // freezes while the tree is removed, so the cursor is the feedback).
        statusBar()->showMessage(
            QString("正在删除 %1 个本地条目…").arg(srcRows.size()));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const int removed = model_->removeEntries(srcRows);
        QApplication::restoreOverrideCursor();
        statusBar()->showMessage(
            QString("已删除 %1 个本地条目").arg(removed), 5000);
    }
}

void MainWindow::deleteRemoteSelected(RemoteTab& tab) {
    QModelIndexList rows = tab.table->selectionModel()->selectedRows(0);
    QStringList names;
    QModelIndexList srcRows;
    for (const QModelIndex& r : rows) {
        const QModelIndex src = tab.proxy->mapToSource(r);
        if (!src.isValid()) continue;
        if (tab.model->isParentRow(src)) continue;   // never delete ".."
        srcRows.push_back(src);
        names << QString::fromStdString(tab.model->nameAt(src));
    }
    if (srcRows.isEmpty()) {
        statusBar()->showMessage("未选中远端条目", 3000);
        return;
    }
    const QString message = QString(
        "即将从远端删除 %1 个条目（目录将递归删除，不可恢复）：\n\n%2\n\n"
        "确定删除吗？")
        .arg(names.size())
        .arg(names.join('\n').left(400));
    if (confirmDeletion(this, message)) {
        tab.model->removeEntries(srcRows);
    }
}

// ---- drag & drop -----------------------------------------------------------

namespace {

// The ".." row is not a real entry: delete/drag already skip it, so it must
// not show up in a rubber-band selection either.
bool isParentEntry(const QAbstractItemModel* source,
                   const QModelIndex& src) {
    if (auto* lm = qobject_cast<const LocalFsModel*>(source))
        return lm->isParentRow(src);
    if (auto* rm = qobject_cast<const RemoteFsModel*>(source))
        return rm->isParentRow(src);
    return false;
}

// Row under a viewport point, tolerating gaps and points above/below the
// list so a drag ending in empty space still selects a sensible range.
int rowAtPoint(QAbstractItemView* view, const QPoint& p) {
    const QModelIndex idx = view->indexAt(p);
    if (idx.isValid()) return idx.row();
    const int rows = view->model()->rowCount();
    if (rows <= 0) return -1;
    const QRect first = view->visualRect(view->model()->index(0, 0));
    const QRect last = view->visualRect(view->model()->index(rows - 1, 0));
    if (p.y() < first.top()) return 0;
    if (p.y() > last.bottom()) return rows - 1;
    for (int d = 1; d <= 8; ++d) {
        const QModelIndex up = view->indexAt(p - QPoint(0, d));
        if (up.isValid()) return up.row();
        const QModelIndex dn = view->indexAt(p + QPoint(0, d));
        if (dn.isValid()) return dn.row();
    }
    return -1;
}

}  // namespace

// Sweep-select the rows between the press point and `pos`. Ctrl toggles the
// swept range on top of the press-time snapshot, Shift adds to it; with no
// modifier the range replaces the selection. Restoring the snapshot on
// every move keeps repeated toggles stable while the frame is dragged.
void MainWindow::applyRubberSelection(const QPoint& pos) {
    QAbstractItemView* view = rubberView_;
    if (!view || !view->model() || !view->selectionModel()) return;
    const int rows = view->model()->rowCount();
    const int cols = view->model()->columnCount();
    if (rows <= 0 || cols <= 0) return;

    const int r1 = rowAtPoint(view, rubberOrigin_);
    const int r2 = rowAtPoint(view, pos);
    if (r1 < 0 || r2 < 0) return;
    const int lo = qMin(r1, r2), hi = qMax(r1, r2);

    QItemSelection range;
    for (int r = lo; r <= hi; ++r) {
        const QModelIndex proxyIdx = view->model()->index(r, 0);
        const QModelIndex src = rubberProxy_
            ? rubberProxy_->mapToSource(proxyIdx) : proxyIdx;
        if (isParentEntry(rubberSource_, src)) continue;   // never select ".."
        range.select(view->model()->index(r, 0),
                     view->model()->index(r, cols - 1));
    }

    auto* sel = view->selectionModel();
    const bool ctrl = (rubberMods_ & Qt::ControlModifier) != 0;
    const bool shift = (rubberMods_ & Qt::ShiftModifier) != 0;
    if (ctrl || shift) {
        QItemSelection base;
        for (int r : rubberBaseRows_) {
            if (r < 0 || r >= rows) continue;
            base.select(view->model()->index(r, 0),
                        view->model()->index(r, cols - 1));
        }
        sel->select(base, QItemSelectionModel::ClearAndSelect);
        sel->select(range, ctrl ? QItemSelectionModel::Toggle
                                : QItemSelectionModel::Select);
    } else {
        sel->select(range, QItemSelectionModel::ClearAndSelect);
    }
}

void MainWindow::endRubberSelection() {
    if (rubberBand_) rubberBand_->hide();
    rubberCandidate_ = false;
    rubberActive_ = false;
    rubberView_ = nullptr;
    rubberProxy_ = nullptr;
    rubberSource_ = nullptr;
    rubberBaseRows_.clear();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Which remote tab (if any) does this viewport belong to?
    int tabIdx = -1;
    for (int i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->table->viewport() == watched ||
            tabs_[i]->table == watched) { tabIdx = i; break; }
    }
    const bool onRemote = tabIdx >= 0;
    const bool onLocal = watched == left_->viewport() || watched == left_;

    // Backspace navigates up one directory (both panes). The QTableView has
    // no default Backspace binding, so the key press reaches us here.
    if (event->type() == QEvent::KeyPress) {
        // Any keyboard action cancels a pending slow-click rename.
        cancelPendingRename();
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Backspace) {
            if (onRemote) {
                RemoteTab* tab = tabs_[tabIdx];
                if (tab->model->isConnected()) {
                    tab->model->goUp();
                    tab->pathEdit->setText(
                        QString::fromStdString(tab->model->currentPath()));
                    return true;
                }
            } else if (onLocal) {
                onUp();
                return true;
            }
        }
        // Delete removes the selected files/folders from whichever pane has
        // focus (mirrors FileZilla / Explorer). Requires a selection.
        if (ke->key() == Qt::Key_Delete) {
            if (onRemote) {
                RemoteTab* tab = tabs_[tabIdx];
                if (tab->model->isConnected() &&
                    !tab->table->selectionModel()->selectedRows(0).isEmpty()) {
                    deleteRemoteSelected(*tab);
                    return true;
                }
            } else if (onLocal) {
                if (!left_->selectionModel()->selectedRows(0).isEmpty()) {
                    deleteLocalSelected();
                    return true;
                }
            }
        }
    }

    // ---- Drag-to-select (rubber band) ------------------------------------
    // Both panes have dragEnabled(true), so a left-drag on a row normally
    // starts a QDrag right away. Keep that for drags starting on an
    // already-selected row (move into a folder / upload) and for mostly
    // horizontal drags (pulling a file to the opposite pane). A mostly
    // vertical drag starting elsewhere sweeps a selection instead, so the
    // user can select many rows by simply dragging over them (Explorer
    // style) without holding Ctrl/Shift.
    if (onLocal || onRemote) {
        QAbstractItemView* view = onLocal
            ? static_cast<QAbstractItemView*>(left_)
            : static_cast<QAbstractItemView*>(tabs_[tabIdx]->table);

        // ---- Explorer-style "slow second click" rename ------------------
        // A press on the name cell of the single already-selected entry arms
        // a short timer. A drag or a quick second click (double-click to
        // open a folder) cancels it; otherwise the timer opens the inline
        // rename editor on that name.
        // Only the viewport receives mouse events; guarding on it avoids the
        // second (parent-view) filter pass cancelling the candidate we just
        // armed on this very press.
        if (event->type() == QEvent::MouseButtonPress &&
            watched == view->viewport())
            cancelPendingRename();   // a new click supersedes a pending one

        const bool renameTarget =
            renameCandidate_ && renameView_ &&
            (watched == renameView_ || watched == renameView_->viewport());
        if (renameTarget && event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if ((me->buttons() & Qt::LeftButton) &&
                (me->position().toPoint() - renamePress_).manhattanLength() >
                    QApplication::startDragDistance())
                cancelPendingRename();   // it's a file drag, not a rename
        }
        if (renameCandidate_ && renameView_ &&
            event->type() == QEvent::MouseButtonRelease &&
            (watched == renameView_ ||
             watched == renameView_->viewport())) {
            auto* me = static_cast<QMouseEvent*>(event);
            const QPoint d = me->position().toPoint() - renamePress_;
            const QModelIndex rel =
                renameView_->indexAt(me->position().toPoint());
            if (d.manhattanLength() > QApplication::startDragDistance() ||
                !rel.isValid() || rel.row() != renameRow_) {
                cancelPendingRename();
            } else if (renameClickTimer_) {
                // Wait past the double-click interval: if another press
                // arrives first, cancelPendingRename() above kills this.
                renameClickTimer_->start();
            }
        }

        if (event->type() == QEvent::MouseButtonPress && view) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() != Qt::LeftButton) {
                endRubberSelection();
            } else {
                const QModelIndex idx =
                    view->indexAt(me->position().toPoint());
                if (idx.isValid() &&
                    view->selectionModel()->isSelected(idx)) {
                    endRubberSelection();   // let Qt start a native file drag

                    // This press happened while the row was already selected:
                    // clicking its name (and not dragging / double-clicking)
                    // means "rename", exactly like Windows Explorer. The
                    // EditRole check also rules out the synthetic ".." row.
                    const bool onName = idx.column() == 0 &&
                        idx.data(Qt::EditRole).isValid();
                    const bool singleSelection =
                        view->selectionModel()->selectedRows(0).size() == 1;
                    const bool noMods = !(me->modifiers() &
                        (Qt::ControlModifier | Qt::ShiftModifier |
                         Qt::AltModifier));
                    if (onName && singleSelection && noMods) {
                        renameView_ = view;
                        renameRow_ = idx.row();
                        renamePress_ = me->position().toPoint();
                        renameCandidate_ = true;
                    }
                } else {
                    rubberView_ = view;
                    rubberProxy_ = onLocal ? proxy_ : tabs_[tabIdx]->proxy;
                    rubberSource_ = onLocal
                        ? static_cast<QAbstractItemModel*>(model_)
                        : static_cast<QAbstractItemModel*>(
                              tabs_[tabIdx]->model);
                    rubberOrigin_ = me->position().toPoint();
                    rubberMods_ = me->modifiers();
                    rubberBaseRows_.clear();
                    for (const QModelIndex& r :
                         view->selectionModel()->selectedRows(0))
                        rubberBaseRows_.push_back(r.row());
                    rubberCandidate_ = true;
                    rubberActive_ = false;
                }
            }
        } else if (event->type() == QEvent::MouseMove &&
                   rubberCandidate_ && rubberView_ &&
                   watched == rubberView_->viewport()) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (!(me->buttons() & Qt::LeftButton)) {
                endRubberSelection();
            } else {
                const QPoint d = me->position().toPoint() - rubberOrigin_;
                const bool forceSelect =
                    (rubberMods_ & Qt::ControlModifier) ||
                    (rubberMods_ & Qt::ShiftModifier) ||
                    !rubberView_->indexAt(rubberOrigin_).isValid();
                const bool horizontal = qAbs(d.x()) > qAbs(d.y()) * 2;
                if (!rubberActive_ && horizontal && !forceSelect) {
                    // Pulling a file sideways toward the other pane: hand
                    // the move stream back to Qt so it can start a QDrag.
                    endRubberSelection();
                } else if (!rubberActive_ &&
                           d.manhattanLength() <
                               QApplication::startDragDistance()) {
                    // Below the drag threshold: leave hover etc. to Qt.
                } else {
                    if (!rubberActive_) {
                        rubberActive_ = true;
                        if (!rubberBand_)
                            rubberBand_ = new QRubberBand(
                                QRubberBand::Rectangle,
                                rubberView_->viewport());
                        else
                            rubberBand_->setParent(rubberView_->viewport());
                        rubberBand_->show();
                    }
                    QRect r = QRect(rubberOrigin_,
                                    me->position().toPoint()).normalized();
                    r.setLeft(qMax(r.left(), 0));
                    r.setRight(qMin(
                        r.right(), rubberView_->viewport()->width() - 1));
                    rubberBand_->setGeometry(r);
                    applyRubberSelection(me->position().toPoint());
                    return true;   // swallow: no QDrag, no hover side effects
                }
            }
        } else if (event->type() == QEvent::MouseButtonRelease &&
                   (rubberCandidate_ || rubberActive_)) {
            const bool wasActive = rubberActive_;
            endRubberSelection();
            if (wasActive) return true;   // avoid a stray clicked() emission
        }
    }

    if (event->type() == QEvent::DragEnter ||
        event->type() == QEvent::DragMove) {
        auto* de = static_cast<QDragEnterEvent*>(event);
        const QMimeData* m = de->mimeData();
        const bool crossLocalToRemote =
            (onRemote && m->hasFormat("application/x-ftpflow-local-paths"));
        const bool crossRemoteToLocal =
            (onLocal && m->hasFormat("application/x-ftpflow-remote-paths"));
        // External files/folders dragged in from Windows Explorer carry only
        // the standard URI list (no custom MIME). Accept them onto the remote
        // pane for upload.
        const bool externalToRemote =
            (onRemote && m->hasUrls() &&
             !m->hasFormat("application/x-ftpflow-remote-paths") &&
             !m->hasFormat("application/x-ftpflow-local-paths"));
        // Same-pane drag onto a folder = move into it.
        const bool localMove =
            (onLocal && m->hasFormat("application/x-ftpflow-local-paths"));
        const bool remoteMove =
            (onRemote && m->hasFormat("application/x-ftpflow-remote-paths"));
        // Both panes are backed by a sort/filter proxy, so the index under
        // the cursor is a PROXY index and must be mapped to the source model
        // before isDir()/pathAt() see it. Without the mapping the folder
        // under the cursor resolves to whatever source row happens to share
        // its number (the list is sorted folders-first), i.e. drops land in
        // the wrong directory.
        bool targetIsDir = false;
        if (localMove) {
            const QModelIndex proxyIdx =
                left_->indexAt(de->position().toPoint());
            const QModelIndex src =
                proxy_ ? proxy_->mapToSource(proxyIdx) : proxyIdx;
            targetIsDir = src.isValid() && model_ && model_->isDir(src);
            // Highlight the folder that will receive the drop so the target
            // is obvious before the user releases the mouse.
            if (targetIsDir)
                left_->selectionModel()->setCurrentIndex(
                    proxyIdx, QItemSelectionModel::NoUpdate);
        } else if (remoteMove && tabIdx >= 0) {
            RemoteTab* tb = tabs_[tabIdx];
            const QModelIndex proxyIdx =
                tb->table->indexAt(de->position().toPoint());
            const QModelIndex src = tb->proxy->mapToSource(proxyIdx);
            targetIsDir = src.isValid() && tb->model->isDir(src);
            if (targetIsDir)
                tb->table->selectionModel()->setCurrentIndex(
                    proxyIdx, QItemSelectionModel::NoUpdate);
        }
        const bool moving = (localMove || remoteMove) && targetIsDir;
        const bool ok = crossLocalToRemote || crossRemoteToLocal ||
                        externalToRemote || moving;
        if (ok) {
            // A same-pane drop MOVES the entries into the target folder;
            // cross-pane and OS drops are uploads/downloads (copies).
            de->setDropAction(moving ? Qt::MoveAction : Qt::CopyAction);
            de->accept();
            return true;   // swallow; we handle the drop ourselves
        }
        return false;
    }
    if (event->type() == QEvent::Drop) {
        auto* de = static_cast<QDropEvent*>(event);
        const QMimeData* m = de->mimeData();
        // External files/folders dropped from Windows Explorer (standard URI
        // list, no custom MIME) -> upload to the remote pane's current dir.
        if (onRemote && m->hasUrls() &&
            !m->hasFormat("application/x-ftpflow-remote-paths") &&
            !m->hasFormat("application/x-ftpflow-local-paths")) {
            QStringList paths;
            for (const QUrl& u : m->urls()) {
                if (u.isLocalFile()) paths << u.toLocalFile();
            }
            if (!paths.isEmpty()) {
                de->acceptProposedAction();
                remoteTabs_->setCurrentIndex(tabIdx);   // target the dropped tab
                enqueueLocalToRemote(paths);
                return true;
            }
            return false;
        }
        if (onRemote &&
            m->hasFormat("application/x-ftpflow-local-paths")) {
            const QStringList paths = QString::fromUtf8(
                m->data("application/x-ftpflow-local-paths"))
                .split('\n', Qt::SkipEmptyParts);
            de->acceptProposedAction();
            remoteTabs_->setCurrentIndex(tabIdx);   // target the dropped tab
            enqueueLocalToRemote(paths);
            return true;
        }
        if (onLocal &&
            m->hasFormat("application/x-ftpflow-remote-paths")) {
            const QStringList paths = QString::fromUtf8(
                m->data("application/x-ftpflow-remote-paths"))
                .split('\n', Qt::SkipEmptyParts);
            de->acceptProposedAction();
            enqueueRemoteToLocal(paths);
            return true;
        }
        // Same-pane move: local file/folder dropped onto a local folder.
        if (onLocal &&
            m->hasFormat("application/x-ftpflow-local-paths")) {
            // Map the proxy index back to the source model (see the drag-enter
            // note above: proxy rows and source rows differ once sorted).
            const QModelIndex proxyIdx = left_->indexAt(de->position().toPoint());
            const QModelIndex target =
                proxy_ ? proxy_->mapToSource(proxyIdx) : proxyIdx;
            if (target.isValid() && model_ && model_->isDir(target)) {
                const QStringList paths = QString::fromUtf8(
                    m->data("application/x-ftpflow-local-paths"))
                    .split('\n', Qt::SkipEmptyParts);
                const QString destDir = model_->pathAt(target);
                if (destDir.isEmpty()) return false;
                de->setDropAction(Qt::MoveAction);
                de->accept();
                const int moved = model_->moveEntries(paths, destDir);
                if (moved > 0) {
                    statusBar()->showMessage(
                        QString("已移动 %1 个条目到 %2")
                            .arg(moved)
                            .arg(QDir::toNativeSeparators(destDir)),
                        6000);
                    appendLog(QString("本地移动：%1 个条目 -> %2")
                                  .arg(moved)
                                  .arg(destDir));
                }
                return true;
            }
            return false;
        }
        // Same-pane move: remote file/folder dropped onto a remote folder.
        if (onRemote &&
            m->hasFormat("application/x-ftpflow-remote-paths")) {
            RemoteTab* tab = tabs_[tabIdx];
            const QModelIndex proxyIdx =
                tab->table->indexAt(de->position().toPoint());
            const QModelIndex target = tab->proxy->mapToSource(proxyIdx);
            if (target.isValid() && tab->model->isDir(target)) {
                const QStringList paths = QString::fromUtf8(
                    m->data("application/x-ftpflow-remote-paths"))
                    .split('\n', Qt::SkipEmptyParts);
                std::vector<std::string> src;
                src.reserve(static_cast<std::size_t>(paths.size()));
                for (const QString& p : paths) src.push_back(p.toStdString());
                const std::string destDir = tab->model->pathAt(target);
                if (destDir.empty() || src.empty()) return false;
                de->setDropAction(Qt::MoveAction);
                de->accept();
                tab->model->moveEntries(src, destDir);
                return true;
            }
            return false;
        }
        return false;
    }
    return QMainWindow::eventFilter(watched, event);
}

// ---- transfer queue -----------------------------------------------------

// Shared helper: build a TransferRequest pre-filled with the current remote
// connection credentials (used by both the buttons and the drop handlers).
// `storeOk` (optional) is set to false when the known_hosts store exists but
// cannot be read -- callers must then ABORT the enqueue instead of letting
// the job fall through to trust-on-first-use.
namespace {
TransferRequest makeRequest(const RemoteFsModel::ConnInfo& ci,
                            bool* storeOk = nullptr) {
    if (storeOk) *storeOk = true;
    TransferRequest req;
    req.protocol = ci.isSftp ? BackendKind::Sftp : BackendKind::Ftp;
    req.ftp = ci.ftp;
    req.ssh = ci.ssh;
    req.pushCommandTemplate = ci.pushCmdTemplate;
    req.siteName = ci.siteName;
    if (ci.isSftp && !ci.knownHostsPath.empty()) {
        KnownHosts kh(ci.knownHostsPath);
        bool ok = true;
        req.trustedHostKeyHex =
            kh.trustedHexFor(ci.ssh.host, ci.ssh.port, &ok);
        if (!ok && storeOk) *storeOk = false;
    }
    return req;
}
}  // namespace

void MainWindow::enqueueLocalToRemote(const QStringList& localPaths) {
    RemoteFsModel* rm = currentRemote();
    if (!rm || !rm->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    if (localPaths.isEmpty()) return;
    const auto ci = rm->connInfo();
    const std::string remoteDir = rm->currentPath();

    // Batch the plain files into one bulk enqueue (single journal write +
    // single lock), so a drag of thousands of files doesn't stall the UI
    // thread on per-file locking and per-file journal disk IO. Folders go
    // through the existing background tree walk.
    std::vector<TransferRequest> fileReqs;
    fileReqs.reserve(localPaths.size());
    for (const QString& localPath : localPaths) {
        const QFileInfo fi(localPath);
        if (!fi.exists()) continue;
        const std::string name = baseName(localPath.toStdString());

        bool storeOk = true;
        TransferRequest req = makeRequest(ci, &storeOk);
        if (!storeOk) {
            statusBar()->showMessage(
                "无法读取 known_hosts 主机密钥库，已取消传输", 6000);
            return;
        }
        // Lock the request to the encoding the live session already learned
        // (GBK on a Chinese IIS) so the queue's independent FtpSession sends
        // GBK bytes for the remote path instead of round-tripping UTF-8.
        if (req.protocol == BackendKind::Ftp)
            req.ftp.encoding = rm->effectiveEncoding();
        req.direction = TransferDirection::Upload;
        req.conflict = (ConflictPolicy)conflictCombo_->currentData().toInt();
        req.localPath = localPath.toStdString();
        req.remotePath = (remoteDir.empty() || remoteDir == ".")
            ? name : remoteDir + "/" + name;

        if (fi.isDir()) {
            // A folder: offer "pack into one archive + auto-extract on the
            // remote" only when the backend can unpack it (SFTP via exec).
            // FTP has no execution channel, so packing would just leave an
            // un-extractable archive — route it to per-file transfer instead.
            const bool canAutoExtract = req.protocol == BackendKind::Sftp;
            const bool archive =
                canAutoExtract && askFolderArchive(fi.absoluteFilePath());
            if (archive) {
                req.dirArchive = true;
                fileReqs.push_back(std::move(req));
            } else {
                // Walk the tree off the UI thread (large trees freeze it).
                enqueueTreeAsync(std::move(req), QStringLiteral("递归上传"));
            }
        } else {
            fileReqs.push_back(std::move(req));
        }
    }
    if (!fileReqs.empty()) {
        queue_->enqueueBatch(std::move(fileReqs));
    }
    transferDock_->show();
}

void MainWindow::enqueueRemoteToLocal(const QStringList& remotePaths) {
    RemoteFsModel* rm = currentRemote();
    if (!rm || !rm->isConnected()) {
        statusBar()->showMessage("请先连接远端", 4000);
        return;
    }
    if (remotePaths.isEmpty()) return;
    const auto ci = rm->connInfo();
    const std::string localDir = model_->currentDirFullPath();
    for (const QString& remotePath : remotePaths) {
        // A trailing slash marks a directory (set by the drag source).
        QString rp = remotePath;
        const bool isDir = rp.endsWith('/');
        if (isDir) rp.chop(1);
        const std::string rpath = rp.toStdString();
        const std::string name = baseName(rpath);

        bool storeOk = true;
        TransferRequest req = makeRequest(ci, &storeOk);
        if (!storeOk) {
            statusBar()->showMessage(
                "无法读取 known_hosts 主机密钥库，已取消传输", 6000);
            return;
        }
        // Lock to the live session's learned encoding (see upload path).
        if (req.protocol == BackendKind::Ftp)
            req.ftp.encoding = rm->effectiveEncoding();
        req.direction = TransferDirection::Download;
        req.conflict = (ConflictPolicy)conflictCombo_->currentData().toInt();
        req.remotePath = rpath;
        req.localPath = localDir + "\\" + name;

        if (isDir) {
            // A remote folder: offer archive transfer when the backend can
            // pack on the remote (SFTP via exec). FTP can't, so it always
            // falls back to the per-file tree walk.
            if (req.protocol == BackendKind::Sftp &&
                askFolderArchive(QString::fromStdString(rpath))) {
                req.dirArchive = true;
                queue_->enqueue(std::move(req));
            } else {
                // Walk the tree off the UI thread (large trees freeze it).
                enqueueTreeAsync(std::move(req), QStringLiteral("递归下载"));
            }
        } else {
            queue_->enqueue(std::move(req));
        }
    }
    transferDock_->show();
}

// Folder transfers scan the whole tree before the first file can be queued.
// Remote walks (FTP/SFTP LIST round-trips per directory) can take minutes on
// big trees, so they run on a worker thread; completion lands back on the UI
// thread via a queued call. Lifetime: treeWorkers_ (and its blocking future
// destructors) is torn down before queue_ in ~MainWindow, so the task body
// always sees a live queue_, and queued callbacks targeting `this` are
// dropped automatically once the QObject is destroyed.
void MainWindow::enqueueTreeAsync(TransferRequest req, const QString& verb) {
    // Drop finished futures so the vector cannot grow without bound.
    for (auto it = treeWorkers_.begin(); it != treeWorkers_.end();) {
        if (it->wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready)
            it = treeWorkers_.erase(it);
        else
            ++it;
    }

    statusBar()->showMessage(verb + "：正在扫描目录...", 0);
    auto abortFlag = std::make_shared<std::atomic<bool>>(false);
    req.enqueueAbort = abortFlag;
    treeAbortFlags_.push_back(abortFlag);
    const quint64 epoch = queueStopEpoch_;
    treeWorkers_.push_back(std::async(
        std::launch::async,
        [this, req = std::move(req), verb, abortFlag, epoch]() mutable {
            std::string err;
            const auto ids = queue_->enqueueTree(req, &err);
            const int n = static_cast<int>(ids.size());
            QMetaObject::invokeMethod(
                this,
                [this, err, n, verb, abortFlag, epoch] {
                    if (epoch != queueStopEpoch_ || abortFlag->load()) {
                        appendLog(verb + QStringLiteral("：已取消（紧急停止）"));
                        return;
                    }
                    if (!err.empty()) {
                        const QString msg =
                            QString("%1失败: %2").arg(verb, QString::fromStdString(err));
                        statusBar()->showMessage(msg, 8000);
                        appendLog(msg);
                    } else {
                        const QString msg = QString("%1完成：已加入 %2 个文件")
                                                .arg(verb).arg(n);
                        statusBar()->showMessage(msg, 5000);
                        appendLog(msg);
                    }
                },
                Qt::QueuedConnection);
        }));
}

bool MainWindow::askFolderArchive(const QString& folderPath) {
    // Estimate the folder's total size + file count. For a local folder we
    // walk the tree directly; a remote path string (downloads) has no cheap
    // size, so we can't auto-skip and simply prompt.
    std::uint64_t totalBytes = 0;
    std::size_t fileCount = 0;
    bool sizeKnown = false;
    const std::string p = folderPath.toStdString();
    std::vector<TreeEntry> entries;
    std::string walkErr;
    if (walkLocalTree(p, entries, &walkErr)) {
        sizeKnown = true;
        for (const TreeEntry& e : entries) {
            if (e.isDir) continue;
            totalBytes += e.size;
            ++fileCount;
        }
    }

    // Small folders (or unknown size, e.g. remote) go straight through when
    // we know they're tiny; a genuinely tiny folder never prompts.
    constexpr std::uint64_t kMinBytes = 8ULL * 1024 * 1024;   // 8 MiB
    constexpr std::size_t kMinFiles = 8;
    if (sizeKnown && (totalBytes < kMinBytes || fileCount < kMinFiles)) {
        return false;   // too small to bother compressing
    }

    const QString prompt =
        folderPath.size() > 0 && p.find('/') == std::string::npos
        ? QString("文件夹「%1」将打包成单个压缩包传输，\n"
                  "到达对端后自动解压还原。是否压缩传输？")
              .arg(folderPath)
        : QString("该文件夹将打包成单个压缩包传输，\n"
                  "到达对端后自动解压还原。是否压缩传输？");

    const auto btn = QMessageBox::question(
        this, "压缩传输",
        prompt,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    return btn == QMessageBox::Yes;
}

void MainWindow::onTransferEvent(const TransferEvent& ev) {
    auto it = transferItems_.find(ev.jobId);
    if (it == transferItems_.end()) {
        // Only a fresh Queued event creates a row. Late events for a row
        // the user already removed (cancel -> Progress/Finished arriving
        // afterwards) must NOT resurrect a ghost row.
        if (ev.kind != TransferEvent::Kind::Queued) return;
        auto* item = new QListWidgetItem;
        item->setData(Qt::UserRole, (qulonglong)ev.jobId);
        it = transferItems_.emplace(ev.jobId, item).first;

        JobUi ui;
        ui.jobId = ev.jobId;
        ui.direction = ev.direction == TransferDirection::Upload ? "上传"
                                                                 : "下载";
        ui.mode = ev.mode;
        // FileZilla-style: keep the complete source path and the destination
        // so a row is self-explanatory at a glance.
        const bool upload = ev.direction == TransferDirection::Upload;
        ui.fullPath = QString::fromStdString(
            upload ? ev.localPath : ev.remotePath);
        ui.targetPath = QString::fromStdString(
            upload ? ev.remotePath : ev.localPath);
        ui.name = baseNameOf(ui.fullPath);
        jobUi_[ev.jobId] = ui;
    }

    // Message-log transcript: only record failures. Successful transfers
    // (queued / started / done / canceled / skipped) no longer spam the log.
    // Cache the text on the row too: refreshTransferItem() is called twice
    // below and the second call has ev == nullptr, which used to wipe the
    // error message from the Failed row.
    if (ev.kind == TransferEvent::Kind::Finished) {
        if (!ev.error.empty())
            jobUi_[ev.jobId].error = QString::fromStdString(ev.error);
        if (ev.state == TransferState::Failed) {
            appendLog(QString("%1：%2 失败 - %3")
                .arg(jobUi_[ev.jobId].direction, jobUi_[ev.jobId].fullPath)
                .arg(jobUi_[ev.jobId].error));
        }
    }

    // Update title/state first, then route the row to the bucket matching
    // its (possibly changed) state, then re-sync the progress bar (the move
    // may have re-created it).
    refreshTransferItem(ev.jobId, &ev);
    placeTransferItem(ev.jobId, it->second);
    refreshTransferItem(ev.jobId, nullptr);
    // Coalesced: one relabel after the burst instead of a full scan per row.
    if (transferCountTimer_) transferCountTimer_->start();
    else updateTransferTabCounts();

    // A finished upload may have changed the remote directory. Refresh is
    // throttled to the bare minimum:
    //   * only uploads can alter the remote side (downloads never do);
    //   * only when the uploaded file lands in the directory currently shown
    //     (an upload into some other folder shouldn't disturb this listing);
    //   * and even then it is debounced -- a batch of N files arms a single
    //     one-shot timer instead of issuing N separate LIST requests.
    if (ev.kind == TransferEvent::Kind::Finished) {
        if (ev.direction == TransferDirection::Upload &&
            !ev.remotePath.empty()) {
            uploadsSeen_ = true;
            RemoteFsModel* rm = currentRemote();
            if (rm && rm->isConnected()) {
                // The uploaded file's parent directory vs the directory the
                // user is looking at. Only when they match does the listing
                // need to pick up the new entry.
                const QString cur = QString::fromStdString(rm->currentPath());
                const QString parent =
                    parentDirOf(QString::fromStdString(ev.remotePath));
                // Normalize: treat "" (root as bare name) and "/" alike, and
                // strip a trailing slash so "/a/" == "/a".
                auto norm = [](QString p) {
                    while (p.endsWith('/') && p.size() > 1) p.chop(1);
                    if (p.isEmpty()) p = QStringLiteral("/");
                    return p;
                };
                if (norm(parent) == norm(cur)) remoteRefreshTimer_->start();
            }
        }

        // Auto-hide the dock when every task has finished successfully and
        // nothing is still queued or running. Failed/canceled tasks keep the
        // panel open so the user can inspect them.
        if (queue_->pending() == 0) {
            // The whole queue just drained. Whatever the per-file directory
            // matching above decided, the remote listing is now stale if any
            // upload ran into the directory the user is looking at -- refresh
            // it once, unconditionally, so the last files of a batch always
            // show up. (Download-only queues don't change the remote side, so
            // they skip this to avoid a needless LIST.)
            RemoteFsModel* rmDone = currentRemote();
            if (rmDone && rmDone->isConnected() && uploadsSeen_) {
                rmDone->refresh();
            }
            uploadsSeen_ = false;

            bool allSucceeded = true;
            for (const auto& [id, ui] : jobUi_) {
                (void)id;
                if (ui.state != TransferState::Done &&
                    ui.state != TransferState::Skipped) {
                    allSucceeded = false;
                    break;
                }
            }
            if (allSucceeded) transferDock_->hide();
        }
    }
}

void MainWindow::refreshTransferItem(std::uint64_t jobId,
                                     const TransferEvent* ev) {
    auto it = transferItems_.find(jobId);
    if (it == transferItems_.end()) return;
    auto uiIt = jobUi_.find(jobId);
    if (uiIt == jobUi_.end()) return;
    JobUi& ui = uiIt->second;

    if (ev) {
        // Progress carries the effective mode and completion.
        ui.mode = ev->mode;
        ui.pct = ev->progress.bytesTotal > 0
            ? 100.0 * (double)ev->progress.bytesMoved /
                  (double)ev->progress.bytesTotal
            : 0.0;
        ui.speedBps = ev->progress.speedBytesPerSec;
        if (ev->kind == TransferEvent::Kind::Started) {
            ui.state = TransferState::Running;
            ui.startedAtMs = QDateTime::currentMSecsSinceEpoch();
            ui.fullPath = ev->direction == TransferDirection::Upload
                ? QString::fromStdString(ev->localPath)
                : QString::fromStdString(ev->remotePath);
            ui.name = baseNameOf(ui.fullPath);
        }
        if (ev->kind == TransferEvent::Kind::Finished) {
            ui.state = ev->state;
            ui.paused = false;
        }
    }

    // FileZilla-style live timing: elapsed since the job went Running, and an
    // ETA derived from the instantaneous speed (remaining bytes / speed).
    QString timing;
    if (ui.state == TransferState::Running && ui.startedAtMs > 0) {
        const qint64 elapsedSec =
            (QDateTime::currentMSecsSinceEpoch() - ui.startedAtMs) / 1000;
        if (elapsedSec > 0)
            timing = "已用 " + humanDuration(elapsedSec);
        if (ui.speedBps > 0 && ev && ev->progress.bytesTotal > ev->progress.bytesMoved) {
            const qint64 remainSec = (qint64)((double)(ev->progress.bytesTotal -
                ev->progress.bytesMoved) / ui.speedBps);
            if (remainSec > 0) {
                if (!timing.isEmpty()) timing += " · ";
                timing += "剩余 " + humanDuration(remainSec);
            }
        }
    }

    // Compose the title. FileZilla-style: complete source path with a
    // direction arrow on the first (bold) line, destination + status + speed
    // on the second (dim) line, progress bar at the bottom.
    const QString arrow = ui.direction == "上传" ? "→" : "←";
    const QString srcShow = ui.fullPath.isEmpty() ? ui.name : ui.fullPath;
    const QString dstDir = parentDirOf(ui.targetPath);
    const QString dest = dstDir.isEmpty() ? ui.targetPath : dstDir;
    const QString modeStr = ui.mode == TransferMode::Compress
        ? "压缩" : ui.mode == TransferMode::Ftz1 ? "FTZ" : "直传";

    QString line1 = QString("%1 %2").arg(arrow).arg(srcShow);
    // A job is shown as paused when its own flag is set OR the whole queue
    // is halted via the FileZilla "process queue" toggle.
    const bool effectivePaused =
        ui.paused || (queue_ && queue_->isGloballyPaused());
    QString line2;
    if (ui.state == TransferState::Queued) {
        line2 = effectivePaused
            ? QString("目标 %1 · %2 · 排队中（已暂停）").arg(dest).arg(modeStr)
            : QString("目标 %1 · %2 · 排队中").arg(dest).arg(modeStr);
    } else if (ui.state == TransferState::Running) {
        if (effectivePaused) {
            line2 = QString("目标 %1 · %2 · 已暂停 (%3%)")
                        .arg(dest).arg(modeStr).arg(ui.pct, 0, 'f', 1);
        } else {
            line2 = QString("目标 %1 · %2 · %3%")
                        .arg(dest).arg(modeStr).arg(ui.pct, 0, 'f', 1);
            if (ev && (ev->kind == TransferEvent::Kind::Progress ||
                       ev->kind == TransferEvent::Kind::Finished)) {
                const QString spd = humanSpeed(ev->progress.speedBytesPerSec);
                if (!spd.isEmpty()) line2 += "  " + spd;
            }
            if (!timing.isEmpty()) line2 += " · " + timing;
        }
    } else if (ui.state == TransferState::Done) {
        line2 = QString("目标 %1 · %2 · ✓ 完成").arg(dest).arg(modeStr);
    } else if (ui.state == TransferState::Skipped) {
        line2 = QString("目标 %1 · ⊘ 已跳过（目标已存在）").arg(dest);
    } else if (ui.state == TransferState::Failed) {
        QString errText;
        const auto jit = jobUi_.find(jobId);
        if (jit != jobUi_.end()) errText = jit->second.error;
        if (errText.isEmpty() && ev) errText = QString::fromStdString(ev->error);
        line2 = QString("目标 %1 · ✗ %2").arg(dest).arg(errText);
    } else if (ui.state == TransferState::Canceled) {
        line2 = QString("目标 %1 · 已取消").arg(dest);
    }

    // If the row carries our custom widget, update it; otherwise fall back to
    // a single-line item text (should not happen after placement).
    QListWidget* owning = it->second->listWidget();
    QWidget* w = owning ? owning->itemWidget(it->second) : nullptr;
    const bool showBar = ui.state == TransferState::Queued ||
                         ui.state == TransferState::Running;
    if (auto* row = dynamic_cast<TransferRowWidget*>(w)) {
        row->setPath(line1);
        row->setMeta(line2);
        row->setShowProgress(showBar);
    } else {
        it->second->setText(line1 +
            (line2.isEmpty() ? QString() : QString("\n") + line2));
    }

    // Progress bar: color by direction/state, text shows percent. Only the
    // queued bucket shows a bar; finished/failed rows are text-only.
    if (showBar) {
        auto* row = dynamic_cast<TransferRowWidget*>(w);
        if (row) {
            QProgressBar* bar = row->bar();
            bar->setValue((int)(ui.pct * 10.0));
            bar->setFormat(QString("%1%").arg(ui.pct, 0, 'f', 0));
            // Upload = teal, download = blue, done = green, fail = red,
            // paused = gray.
            QString chunk;
            if (ui.state == TransferState::Failed) chunk = "#f48771";
            else if (ui.state == TransferState::Done) chunk = "#4ec9b0";
            else if (ui.state == TransferState::Skipped) chunk = "#9d9d9d";
            else if (effectivePaused) chunk = "#9d9d9d";
            else if (ui.state == TransferState::Canceled) chunk = "#6a6a6a";
            else if (ui.direction == "下载") chunk = "#569cd6";
            else chunk = "#2ea8c0";
            bar->setStyleSheet(
                QString("QProgressBar::chunk { background-color: %1; }")
                    .arg(chunk));
        }
    }
}

QListWidget* MainWindow::placeTransferItem(std::uint64_t jobId,
                                           QListWidgetItem* item) {
    auto uiIt = jobUi_.find(jobId);
    if (uiIt == jobUi_.end()) return nullptr;

    QListWidget* target = nullptr;
    switch (uiIt->second.state) {
        case TransferState::Queued:
        case TransferState::Running: target = queueList_; break;
        case TransferState::Failed:
        case TransferState::Canceled: target = failedList_; break;
        case TransferState::Done:
        case TransferState::Skipped: target = doneList_; break;
    }
    if (!target) return nullptr;

    QListWidget* current = item->listWidget();
    if (current != target) {
        // Re-home the row. takeItem() drops the item widget (progress bar),
        // so re-attach a fresh one after the move.
        if (current) current->takeItem(current->row(item));
        target->addItem(item);
    }
    if (target->itemWidget(item) == nullptr) {
        // Bound the number of rich widgets. Queued jobs beyond the cap get
        // plain two-line text (set in refreshTransferItem). Active jobs
        // always get a progress bar, so progress stays visible.
        const bool active = uiIt->second.state == TransferState::Running;
        if (active || target->count() <= kMaxRichTransferRows) {
            auto* row = new TransferRowWidget;
            target->setItemWidget(item, row);
            item->setSizeHint(row->sizeHint());
        } else {
            item->setSizeHint(QSize(10, 42));
        }
    }
    return target;
}

// ---- transfer dock interactions -----------------------------------------

namespace {
// Collects the job ids of the currently selected transfer rows.
std::vector<std::uint64_t> selectedJobIds(QListWidget* list) {
    std::vector<std::uint64_t> ids;
    for (QListWidgetItem* item : list->selectedItems()) {
        ids.push_back((std::uint64_t)item->data(Qt::UserRole).toULongLong());
    }
    return ids;
}
}  // namespace

// The list backing the currently visible bucket tab.
QListWidget* MainWindow::currentTransferList() {
    return qobject_cast<QListWidget*>(transferTabs_->currentWidget());
}

void MainWindow::updateTransferTabCounts() {
    int queued = 0, failed = 0, done = 0;
    for (const auto& [id, ui] : jobUi_) {
        (void)id;
        switch (ui.state) {
            case TransferState::Queued:
            case TransferState::Running: ++queued; break;
            case TransferState::Failed:
            case TransferState::Canceled: ++failed; break;
            case TransferState::Done:
            case TransferState::Skipped: ++done; break;
        }
    }
    transferTabs_->setTabText(0, QString("列队的文件 (%1)").arg(queued));
    transferTabs_->setTabText(1, QString("传输失败 (%1)").arg(failed));
    transferTabs_->setTabText(2, QString("成功的传输 (%1)").arg(done));
}

void MainWindow::appendLog(const QString& line) {
    if (!logView_) return;
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    logView_->appendPlainText(QString("[%1] %2").arg(ts, line));
    // Keep the transcript pinned to the latest line while it is growing.
    auto* sb = logView_->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void MainWindow::toggleTransferDock() {
    if (!transferDock_) return;
    // When the dock was closed via its [x] button, isVisible() stays false
    // but isFloating()/window state may differ; force both paths to a
    // deterministic show/hide.
    if (transferDock_->isVisible()) {
        transferDock_->hide();
    } else {
        transferDock_->show();
        transferDock_->raise();
    }
}

// ---- Membership / account entry -------------------------------------------

void MainWindow::refreshAccountButton() {
    if (!accountBtn_) return;
    accountBtn_->setIcon(makeAvatarIcon(18));
    if (sessionLocked_) {
        accountBtn_->setText("已锁定 · 验证身份");
        accountBtn_->setToolTip(
            "上次登录的会员站点已锁定；点击验证密码、切换游客或更换账号");
    } else if (memberName_.isEmpty()) {
        accountBtn_->setText("登录 / 注册");
        accountBtn_->setToolTip("登录账号（当前为游客站点列表）");
    } else {
        // Simple: avatar + name (matching a clean account entry).
        accountBtn_->setText(memberName_);
        accountBtn_->setToolTip(
            QString("%1（%2）· 独立的会员站点列表")
                .arg(memberName_, memberLevel_));
    }
}

void MainWindow::onAccountMenu() {
    QMenu menu(this);

    // Pop upward from the corner button (it sits in the bottom bar).
    auto execMenu = [this, &menu]() -> QAction* {
        menu.ensurePolished();
        const int mh = menu.sizeHint().height();
        return menu.exec(accountBtn_->mapToGlobal(QPoint(0, -mh)));
    };

    // A locked startup session: the member site list has NOT been loaded.
    // Offer exactly the three ways out (verify / guest / another account).
    if (sessionLocked_) {
        QAction* unlockAct = menu.addAction("验证密码解锁");
        unlockAct->setIcon(makeAvatarIcon(16));
        menu.addSeparator();
        QAction* guestAct = menu.addAction("以游客身份使用（站点列表为空）");
        QAction* switchAct = menu.addAction("更换账号登录");
        QAction* chosen = execMenu();
        if (chosen == unlockAct) onStartupUnlock();
        else if (chosen == guestAct) useGuestFromLock();
        else if (chosen == switchAct) onSwitchAccount();
        return;
    }

    if (memberName_.isEmpty()) {
        // Guest: only a sign-in entry. Guest sites are private to this
        // identity and are migrated into the member account on login.
        QAction* loginAct = menu.addAction("登录 / 注册");
        loginAct->setIcon(makeAvatarIcon(16));
        if (execMenu() == loginAct) onLogin();
        return;
    }

    // Logged in: a simple account menu. Header shows avatar + name + a
    // version badge, then a plain-text list of actions (no clutter).
    auto* header = new QWidget(&menu);
    header->setObjectName("accountMenuHeader");
    auto* headLay = new QHBoxLayout(header);
    headLay->setContentsMargins(14, 12, 14, 10);
    headLay->setSpacing(10);
    auto* avatar = new QLabel(header);
    avatar->setPixmap(makeAvatarIcon(36).pixmap(36, 36));
    avatar->setFixedSize(36, 36);
    auto* nameCol = new QVBoxLayout();
    nameCol->setSpacing(3);
    auto* nameLbl = new QLabel(memberName_, header);
    nameLbl->setObjectName("accountMenuName");
    auto* badge = new QLabel(
        QString("%1 · 基础版本").arg(memberLevel_.isEmpty() ? "普通用户"
                                                            : memberLevel_),
        header);
    badge->setObjectName("accountMenuBadge");
    nameCol->addWidget(nameLbl);
    nameCol->addWidget(badge);
    headLay->addWidget(avatar);
    headLay->addLayout(nameCol, 1);

    auto* headerAct = new QWidgetAction(&menu);
    headerAct->setDefaultWidget(header);
    menu.addAction(headerAct);
    menu.addSeparator();

    QAction* siteAct = menu.addAction("官方网站");
    QAction* docAct = menu.addAction("帮助文档");
    QAction* forumAct = menu.addAction("讨论社区");
    menu.addSeparator();

    // Member-only preference: ask for the account password on every launch.
    QAction* requirePwAct = menu.addAction("每次启动要求验证密码");
    requirePwAct->setCheckable(true);
    requirePwAct->setChecked(requirePasswordOnStartup_);
    requirePwAct->setToolTip(
        "开启后，重启软件需输入本账号密码才能加载会员站点列表；"
        "也可选择以游客身份使用或更换账号登录");
    menu.addSeparator();
    QAction* switchAct = menu.addAction("切换账号登录");
    QAction* logoutAct = menu.addAction("退出登录（切回游客）");

    QAction* chosen = execMenu();
    if (chosen == logoutAct) onLogout();
    else if (chosen == switchAct) onSwitchAccount();
    else if (chosen == requirePwAct)
        onToggleRequirePassword(requirePwAct->isChecked());
    else if (chosen == siteAct || chosen == docAct || chosen == forumAct)
        statusBar()->showMessage(
            QString("「%1」功能即将上线").arg(chosen->text()), 4000);
}

void MainWindow::onLogin() {
    // Real sign-in against the AotCMS member API. The dialog offers both
    // login and register; on success it hands back a JWT + user profile.
    AuthDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    applyLoginResult(dlg.result(), dlg.baseUrl(), dlg.submittedPassword());
}

void MainWindow::switchSiteScope(const QString& scope) {
    siteStore_.setScope(scope);
    siteStore_.load();
}

void MainWindow::applyLoginResult(const AuthResult& r, const QString& baseUrl,
                                  const QString& password) {
    const QString newScope = memberScopeFor(r.userId, r.username);

    // 1) Guest sites (with their FTP passwords) are about to be moved into
    //    this member account. Capture them BEFORE wiping the guest scope.
    pendingGuestSitesJson_.clear();
    if (memberScope_.isEmpty() &&
        (!siteStore_.sites().isEmpty() || !siteStore_.folders().isEmpty()))
        pendingGuestSitesJson_ = serializeSitesForSync(siteStore_);

    // 2) Wipe the guest list + guest credentials: after login the guest
    //    identity must not keep a copy of these (now member) FTP secrets,
    //    and after logout the guest list must be empty.
    if (!pendingGuestSitesJson_.isEmpty())
        siteStore_.clearAll();

    // 3) Switch to this member's private list. A different userId/username
    //    gets a different file, so accounts never affect each other.
    memberScope_ = newScope;
    switchSiteScope(newScope);

    // 4) Session fields + persistence.
    memberName_ = r.nickname.isEmpty() ? r.username : r.nickname;
    memberLevel_ = r.levelName.isEmpty()
                       ? (r.level > 0 ? QString("Lv.%1").arg(r.level) : "普通用户")
                       : r.levelName;
    memberToken_ = r.token;
    memberBaseUrl_ = baseUrl;
    memberUserId_ = r.userId;
    memberUsername_ = r.username;
    sessionLocked_ = false;

    QSettings settings;
    settings.setValue("member/name", memberName_);
    settings.setValue("member/level", memberLevel_);
    settings.setValue("member/token", memberToken_);
    settings.setValue("member/baseUrl", memberBaseUrl_);
    settings.setValue("member/userId", memberUserId_);
    settings.setValue("member/username", memberUsername_);
    settings.setValue("member/scope", memberScope_);
    // Local offline verifier for "require password on every launch".
    const QString salt = QUuid::createUuid().toString(QUuid::WithoutBraces);
    settings.setValue("member/passSalt", salt);
    settings.setValue("member/passHash",
                      passwordVerifier(salt, memberUsername_, password));

    refreshAccountButton();
    const QString migrated = pendingGuestSitesJson_.isEmpty()
        ? QString()
        : QStringLiteral("；游客站点已迁入本账号");
    statusBar()->showMessage(
        QString("已登录：%1（独立的会员站点列表%2）")
            .arg(memberName_, migrated), 6000);

    // Two-way site sync (cloud backup / cross-device restore).
    if (!memberToken_.isEmpty()) startSiteSync();
}

void MainWindow::clearMemberSession() {
    memberName_.clear();
    memberLevel_.clear();
    memberToken_.clear();
    memberBaseUrl_.clear();
    memberUsername_.clear();
    memberUserId_ = 0;
    QSettings settings;
    settings.remove("member/name");
    settings.remove("member/level");
    settings.remove("member/token");
    settings.remove("member/baseUrl");
    settings.remove("member/username");
    settings.remove("member/userId");
    settings.remove("member/scope");
    settings.remove("member/passSalt");
    settings.remove("member/passHash");
    // NOTE: member/requirePasswordOnStartup is a user preference and stays.
}

void MainWindow::onLogout() {
    // Final flush of the member's own list into that member's cloud slot.
    if (!memberToken_.isEmpty() && syncClient_) pushSitesNow();
    siteSyncTimer_->stop();
    pendingGuestSitesJson_.clear();
    pendingFetchScope_.clear();
    clearMemberSession();
    sessionLocked_ = false;
    memberScope_.clear();
    switchSiteScope(QString());   // guest file was emptied at login -> empty
    refreshAccountButton();
    statusBar()->showMessage(
        "已退出登录：当前为游客站点列表（空），会员站点已隐藏", 6000);
}

void MainWindow::onSwitchAccount() {
    // Drop the current session first so the new account starts from its own
    // (empty) guest scope and can never inherit the previous account's sites.
    onLogout();
    onLogin();
}

void MainWindow::onToggleRequirePassword(bool on) {
    QSettings settings;
    if (on) {
        // Need a local verifier. If it doesn't exist yet (e.g. the setting
        // was turned on from a restored session), ask for the password once.
        const QString salt = settings.value("member/passSalt").toString();
        const QString hash = settings.value("member/passHash").toString();
        if (salt.isEmpty() || hash.isEmpty()) {
            bool ok = false;
            const QString pw = QInputDialog::getText(
                this, "设置启动验证",
                QString("请输入账号 %1 的登录密码（用于本机离线验证）：")
                    .arg(memberUsername_.isEmpty() ? memberName_
                                                   : memberUsername_),
                QLineEdit::Password, QString(), &ok);
            if (!ok || pw.isEmpty()) {
                statusBar()->showMessage(
                    "已取消：需要密码才能开启启动验证", 5000);
                return;
            }
            const QString newSalt =
                QUuid::createUuid().toString(QUuid::WithoutBraces);
            settings.setValue("member/passSalt", newSalt);
            settings.setValue("member/passHash",
                              passwordVerifier(newSalt, memberUsername_, pw));
        }
    }
    requirePasswordOnStartup_ = on;
    settings.setValue("member/requirePasswordOnStartup", on);
    statusBar()->showMessage(
        on ? "已开启：每次重新启动软件需要验证会员密码"
           : "已关闭：启动时不验证密码", 5000);
}

void MainWindow::useGuestFromLock() {
    clearMemberSession();   // scoped member files stay on disk, not loaded
    sessionLocked_ = false;
    memberScope_.clear();
    switchSiteScope(QString());
    refreshAccountButton();
    restoreJournal();
    statusBar()->showMessage(
        "已切换为游客：游客站点列表为空，会员站点未加载", 6000);
}

void MainWindow::onStartupUnlock() {
    QSettings settings;
    const QString name = settings.value("member/name").toString();
    const QString user = settings.value("member/username").toString();
    const QString salt = settings.value("member/passSalt").toString();
    const QString hash = settings.value("member/passHash").toString();

    auto verify = [salt, hash, user](const QString& pw) {
        if (salt.isEmpty() || hash.isEmpty()) return false;
        return passwordVerifier(salt, user, pw) == hash;
    };

    SessionUnlockDialog dlg(name, user, verify, this);
    if (dlg.exec() != QDialog::Accepted ||
        dlg.outcome() == SessionUnlockDialog::Outcome::Canceled) {
        useGuestFromLock();   // closing the dialog must never reveal sites
        return;
    }

    switch (dlg.outcome()) {
    case SessionUnlockDialog::Outcome::Guest:
        useGuestFromLock();
        return;
    case SessionUnlockDialog::Outcome::SwitchAccount: {
        clearMemberSession();
        sessionLocked_ = false;
        memberScope_.clear();
        switchSiteScope(QString());
        refreshAccountButton();
        restoreJournal();
        onLogin();
        return;
    }
    case SessionUnlockDialog::Outcome::Verified:
        break;
    default:
        useGuestFromLock();
        return;
    }

    // Verified: only NOW load the member's private site list.
    memberName_ = name;
    memberLevel_ = settings.value("member/level").toString();
    memberToken_ = settings.value("member/token").toString();
    memberBaseUrl_ = settings.value("member/baseUrl").toString();
    memberUsername_ = user;
    memberUserId_ = settings.value("member/userId", 0).toInt();
    memberScope_ = settings.value("member/scope").toString();
    sessionLocked_ = false;
    switchSiteScope(memberScope_);
    refreshAccountButton();
    statusBar()->showMessage(
        QString("验证通过：已恢复 %1 的会员站点列表").arg(memberName_), 5000);
    restoreJournal();

    // Refresh the JWT in the background with the password we just verified;
    // harmless when the server is unreachable (the saved token stays).
    if (!memberUsername_.isEmpty() && !memberBaseUrl_.isEmpty())
        syncClient_->login(memberBaseUrl_, memberUsername_, dlg.password());
    QTimer::singleShot(900, this, [this] {
        if (!memberToken_.isEmpty() && !sessionLocked_) startSiteSync();
    });
}

void MainWindow::onUnlockReloginFinished(const AuthResult& r) {
    if (!r.success || r.token.isEmpty() || sessionLocked_) return;
    memberToken_ = r.token;
    QSettings().setValue("member/token", r.token);
}

void MainWindow::onOpenMemberCenter() {
    statusBar()->showMessage("会员中心功能即将上线", 4000);
}

// Serialize the WHOLE identity-scoped site configuration (sites + standalone
// folders, passwords included) into one opaque JSON string for the cloud
// sync endpoint. The server only checks that this is valid JSON (array or
// object) and encrypts it verbatim, so folders can be carried without any
// backend change. `store` decides which identity's credentials are read.
static QString serializeSitesForSync(const ftpflow::SiteStore& store) {
    QJsonObject root;
    QJsonArray arr;
    for (const ftpflow::SiteEntry& e : store.sites()) {
        QJsonObject o;
        o.insert("name", e.name);
        o.insert("protocol", e.isSftp ? "sftp" : "ftp");
        o.insert("host", e.host);
        o.insert("port", e.port);
        o.insert("user", e.user);
        if (!e.keyPath.isEmpty()) o.insert("keyPath", e.keyPath);
        if (!e.startDir.isEmpty()) o.insert("startDir", e.startDir);
        if (!e.pushCmd.isEmpty()) o.insert("pushCmd", e.pushCmd);
        if (!e.sftpCmd.isEmpty()) o.insert("sftpCmd", e.sftpCmd);
        if (!e.group.isEmpty()) o.insert("group", e.group);
        if (!e.encoding.isEmpty() && e.encoding != "auto")
            o.insert("encoding", e.encoding);
        if (!e.note.isEmpty()) o.insert("note", e.note);
        // Include the password (from this scope's Credential Manager slot)
        // so cloud restore can rehydrate credentials on a new device.
        const QString pw = store.readPassword(e.name);
        if (!pw.isEmpty()) o.insert("password", pw);
        arr.append(o);
    }
    root.insert("sites", arr);

    QJsonArray farr;
    for (const QString& f : store.folders()) farr.append(f);
    root.insert("folders", farr);

    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// Merge a downloaded payload into the local store. Accepts BOTH the new
// object format {"sites":[...],"folders":[...]} and the legacy bare array
// written by older builds, so existing cloud data is never lost.
// `overwriteExisting` is used for guest->member migration: the guest's just
// captured sites win over same-named cloud entries. Normal cloud merges are
// additive and never clobber locally edited sites.
// Returns the number of sites added/replaced.
static int mergeDownloadedSites(ftpflow::SiteStore& store,
                                const QString& payload,
                                bool overwriteExisting) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) return 0;

    QJsonArray siteArr;
    QJsonArray folderArr;
    if (doc.isArray()) {
        siteArr = doc.array();                  // legacy format
    } else if (doc.isObject()) {
        siteArr = doc.object().value("sites").toArray();
        folderArr = doc.object().value("folders").toArray();
    } else {
        return 0;
    }

    // Build the set of existing local names to avoid clobbering them.
    QSet<QString> existing;
    if (!overwriteExisting)
        for (const ftpflow::SiteEntry& e : store.sites())
            existing.insert(e.name);

    int added = 0;
    store.beginBatch();
    for (const QJsonValue& v : siteArr) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        const QString name = o.value("name").toString();
        if (name.isEmpty() || (!overwriteExisting && existing.contains(name)))
            continue;

        ftpflow::SiteEntry e;
        e.name = name;
        e.isSftp = o.value("protocol").toString("sftp") == "sftp";
        e.host = o.value("host").toString();
        e.port = o.value("port").toInt(e.isSftp ? 22 : 21);
        e.user = o.value("user").toString();
        e.keyPath = o.value("keyPath").toString();
        e.startDir = o.value("startDir").toString();
        e.pushCmd = o.value("pushCmd").toString();
        e.sftpCmd = o.value("sftpCmd").toString();
        e.group = o.value("group").toString();
        e.encoding = o.value("encoding").toString("auto");
        e.note = o.value("note").toString();

        if (e.host.isEmpty()) continue;   // skip malformed entries
        store.upsert(e);

        // Passwords are written into THIS scope's credential namespace.
        const QString pw = o.value("password").toString();
        if (!pw.isEmpty()) store.writePassword(name, pw);
        ++added;
    }
    for (const QJsonValue& f : folderArr) {
        if (f.isString() && !f.toString().trimmed().isEmpty())
            store.addFolder(f.toString());
    }
    store.endBatch();
    return added;
}

void MainWindow::pushSitesNow() {
    if (memberToken_.isEmpty() || !syncClient_ || sessionLocked_) return;
    const QString sites = serializeSitesForSync(siteStore_);
    syncClient_->syncSites(memberBaseUrl_, memberToken_, sites);
}

void MainWindow::startSiteSync() {
    if (memberToken_.isEmpty() || !syncClient_ || sessionLocked_) return;

    // Guest sites captured at login are merged into this member's list
    // BEFORE the fetch, so they are part of every push even when the cloud
    // request fails. The pending string is cleared only once a push
    // actually succeeds (see onSyncFinished) -- a failed sync is retried on
    // the next login instead of silently losing the guest data.
    if (!pendingGuestSitesJson_.isEmpty()) {
        suppressSiteSync_ = true;
        mergeDownloadedSites(siteStore_, pendingGuestSitesJson_,
                             /*overwriteExisting=*/true);
        siteStore_.save();
        suppressSiteSync_ = false;
    }

    statusBar()->showMessage("正在同步站点数据…", 3000);
    // Pull first: merge any cloud-only sites into the local store, then push
    // the merged result back so both ends converge to the union.
    pendingFetchScope_ = memberScope_;
    syncClient_->fetchSites(memberBaseUrl_, memberToken_);
}

void MainWindow::onFetchFinished(bool ok, const QString& message,
                                 const QString& sitesJson) {
    // A late reply from a session we already logged out of (or switched
    // away from) must NOT be merged into the guest / another account's list.
    if (pendingFetchScope_.isEmpty() ||
        pendingFetchScope_ != memberScope_ ||
        memberToken_.isEmpty() || sessionLocked_) {
        pendingFetchScope_.clear();
        return;
    }
    pendingFetchScope_.clear();

    if (!ok) {
        statusBar()->showMessage(
            QString("站点同步失败：%1（本地数据已保留，下次登录重试）")
                .arg(message), 6000);
        return;
    }

    // Merge cloud sites into local (additive; never clobber existing).
    int added = 0;
    if (!sitesJson.isEmpty()) {
        suppressSiteSync_ = true;
        added = mergeDownloadedSites(siteStore_, sitesJson,
                                     /*overwriteExisting=*/false);
        if (added > 0) siteStore_.save();
        suppressSiteSync_ = false;
    }

    // Push the merged local list (with passwords) back to the cloud.
    const QString merged = serializeSitesForSync(siteStore_);
    syncClient_->syncSites(memberBaseUrl_, memberToken_, merged);

    if (added > 0)
        statusBar()->showMessage(
            QString("已从云端恢复 %1 个站点").arg(added), 5000);
}

void MainWindow::onSyncFinished(bool ok, const QString& message,
                                const QString& sitesJson) {
    Q_UNUSED(sitesJson);
    if (ok) {
        // The cloud now contains any guest sites that were migrated at
        // login, so the pending copy can be dropped.
        pendingGuestSitesJson_.clear();
        statusBar()->showMessage("站点已同步到云端", 5000);
    } else {
        statusBar()->showMessage(
            QString("站点同步失败：%1（将在下次登录重试）").arg(message),
            6000);
    }
}

void MainWindow::onTransferContextMenu(const QPoint& pos) {
    // Right-click can arrive on any of the three bucket lists.
    auto* list = qobject_cast<QListWidget*>(sender());
    if (!list) list = currentTransferList();
    if (!list) return;
    const QModelIndex idx = list->indexAt(pos);
    if (!idx.isValid()) return;

    QMenu menu(this);
    // Requeue makes sense only for rows that have reached a terminal state
    // (failed / canceled / skipped). It re-enqueues them with fresh jobs.
    const bool hasTerminal = [&]() {
        const auto ids = selectedJobIds(list);
        for (std::uint64_t id : ids) {
            auto it = jobUi_.find(id);
            if (it == jobUi_.end()) continue;
            const TransferState s = it->second.state;
            if (s == TransferState::Failed || s == TransferState::Canceled ||
                s == TransferState::Skipped)
                return true;
        }
        return false;
    }();
    const bool hasActive = [&]() {
        const auto ids = selectedJobIds(list);
        for (std::uint64_t id : ids) {
            auto it = jobUi_.find(id);
            if (it == jobUi_.end()) continue;
            const TransferState s = it->second.state;
            if (s == TransferState::Queued || s == TransferState::Running)
                return true;
        }
        return false;
    }();

    // FileZilla: the "process queue" entry toggles the whole queue.
    QAction* procAction = menu.addAction(
        queue_->isGloballyPaused() ? "处理队列" : "暂停队列");
    QAction* retryAction = nullptr;
    if (hasTerminal) retryAction = menu.addAction("重新排列");
    QAction* retryAllAction = nullptr;
    // "All" is a bucket action, not a selection action: right-clicking any
    // row in the failed bucket must offer it even when that row is Done.
    if (list == failedList_)
        retryAllAction = menu.addAction("重置并重新加入全部");
    QAction* cancelAction = nullptr;
    if (hasActive) cancelAction = menu.addAction("取消");
    menu.addSeparator();
    QAction* removeAction = menu.addAction("移除选中");
    QAction* clearAction = menu.addAction("清空全部");
    QAction* stopAction = menu.addAction("紧急停止全部 (Ctrl+Shift+X)");

    QAction* chosen = menu.exec(list->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == procAction) onToggleProcessQueue();
    else if (chosen == retryAction) onRetrySelected();
    else if (chosen == retryAllAction) onRetryAllFailed();
    else if (chosen == cancelAction) onCancelSelected();
    else if (chosen == removeAction) onRemoveSelected();
    else if (chosen == clearAction) onClearAll();
    else if (chosen == stopAction) onEmergencyStop();
}

void MainWindow::onToggleProcessQueue() {
    // FileZilla "process queue" semantics: the button is checked while
    // paused. Toggling flips the whole queue between processing and paused.
    const bool nowPaused = pauseBtn_->isChecked();
    if (nowPaused) {
        queue_->pauseAll();
    } else {
        queue_->resumeAll();
    }
    // Reflect the new global state on the rows. For a huge restored batch,
    // relabelling all 17,000 rows makes the click feel frozen; the status
    // bar + the toggle state already tell the user what happened, and the
    // next per-row event refreshes any row that matters.
    if (jobUi_.size() <= kMaxRichTransferRows * 3) {
        for (const auto& [id, ui] : jobUi_) {
            (void)id;
            if (ui.state == TransferState::Queued ||
                ui.state == TransferState::Running) {
                refreshTransferItem(ui.jobId, nullptr);
            }
        }
    }
    statusBar()->showMessage(
        nowPaused ? "传输队列已暂停" : "传输队列已恢复", 3000);
}

void MainWindow::onCancelSelected() {
    QListWidget* list = currentTransferList();
    if (!list) return;
    const auto ids = selectedJobIds(list);
    if (ids.empty()) {
        statusBar()->showMessage("未选中传输任务", 4000);
        return;
    }
    for (std::uint64_t id : ids) {
        queue_->cancel(id);
    }
}

void MainWindow::onRetrySelected() {
    QListWidget* list = currentTransferList();
    if (!list) return;
    // NOTE: no "current remote" requirement -- a job carries its own
    // endpoint and credentials (see TransferQueue::retry), so retrying
    // while another tab is active (or after a disconnect) must not drag in
    // the wrong server's connection.
    const auto ids = selectedJobIds(list);
    if (ids.empty()) {
        statusBar()->showMessage("未选中传输任务", 4000);
        return;
    }

    const ConflictPolicy conflict =
        (ConflictPolicy)conflictCombo_->currentData().toInt();
    int retried = 0;
    int stale = 0;
    for (std::uint64_t id : ids) {
        auto uiIt = jobUi_.find(id);
        if (uiIt == jobUi_.end()) continue;
        const JobUi& ui = uiIt->second;
        // Only terminal states can be retried; running/queued rows are
        // managed through pause/cancel instead.
        if (ui.state != TransferState::Failed &&
            ui.state != TransferState::Canceled &&
            ui.state != TransferState::Skipped)
            continue;

        // Requeue the ORIGINAL request (same server, same credentials);
        // the conflict policy is taken from the combo so the user can flip
        // it before retrying.
        const bool ok = queue_->retry(id, conflict) != 0;

        // Drop the old terminal row in both cases: on success the new job
        // re-creates a fresh row; on failure this row is a ghost.
        auto itemIt = transferItems_.find(id);
        if (itemIt != transferItems_.end()) {
            delete itemIt->second;
            transferItems_.erase(itemIt);
        }
        jobUi_.erase(id);
        if (ok) ++retried;
        else ++stale;
    }

    if (retried > 0 || stale > 0) {
        transferDock_->show();
        updateTransferTabCounts();
        QString msg;
        if (retried > 0)
            msg = QString("已重新加入 %1 个任务").arg(retried);
        else
            msg = QString("%1 个任务已失效并移除").arg(stale);
        if (retried > 0 && stale > 0)
            msg += QString("（%1 个失效行已移除）").arg(stale);
        statusBar()->showMessage(msg, 4000);
    }
}

void MainWindow::onRetryAllFailed() {
    // One-click retry for the "传输失败" bucket: failed and canceled rows
    // only. Skipped jobs live in the "成功的传输" bucket -- including them
    // here made a click on 3 visible failures re-enqueue hundreds of
    // skipped files and look like a scrambled queue.
    const ConflictPolicy conflict =
        (ConflictPolicy)conflictCombo_->currentData().toInt();

    // Collect ids first; the per-id requeue below mutates jobUi_.
    std::vector<std::uint64_t> terminal;
    for (const auto& [id, ui] : jobUi_) {
        if (ui.state == TransferState::Failed ||
            ui.state == TransferState::Canceled)
            terminal.push_back(id);
    }

    int retried = 0;
    int stale = 0;
    for (std::uint64_t id : terminal) {
        const bool ok = queue_->retry(id, conflict) != 0;
        // Drop the old terminal row in both cases. On success the new job
        // re-creates a fresh row; on failure the job is no longer retryable
        // (e.g. evicted cache after an emergency stop) and the row is a
        // ghost that must not keep claiming "failed" forever.
        auto itemIt = transferItems_.find(id);
        if (itemIt != transferItems_.end()) {
            delete itemIt->second;
            transferItems_.erase(itemIt);
        }
        jobUi_.erase(id);
        if (ok) ++retried;
        else ++stale;
    }

    if (retried > 0) {
        transferDock_->show();
        updateTransferTabCounts();
        statusBar()->showMessage(
            QString("已重置并重新加入 %1 个文件%2")
                .arg(retried)
                .arg(stale > 0 ? QString("（%1 个失效行已移除）").arg(stale)
                               : QString()), 4000);
        appendLog(QString("已重置并重新加入 %1 个失败文件").arg(retried));
    } else if (stale > 0) {
        updateTransferTabCounts();
        statusBar()->showMessage(
            QString("%1 个失败任务已失效并移除").arg(stale), 4000);
    } else {
        statusBar()->showMessage("没有可重新加入的失败任务", 4000);
    }
}

void MainWindow::onRemoveSelected() {
    // FileZilla "remove selected": drop the chosen rows from the list. Active
    // (queued/running) rows are canceled first so they don't keep running.
    QListWidget* list = currentTransferList();
    if (!list) return;
    const auto ids = selectedJobIds(list);
    if (ids.empty()) {
        statusBar()->showMessage("未选中传输任务", 4000);
        return;
    }
    for (std::uint64_t id : ids) {
        auto uiIt = jobUi_.find(id);
        if (uiIt != jobUi_.end()) {
            const TransferState s = uiIt->second.state;
            if (s == TransferState::Queued || s == TransferState::Running)
                queue_->cancel(id);
        }
        auto itemIt = transferItems_.find(id);
        if (itemIt != transferItems_.end()) {
            delete itemIt->second;
            transferItems_.erase(itemIt);
        }
        jobUi_.erase(id);
    }
    updateTransferTabCounts();
    if (transferItems_.empty()) transferDock_->hide();
}

void MainWindow::onClearFinished() {
    std::vector<std::uint64_t> toRemove;
    for (auto& [id, item] : transferItems_) {
        auto uiIt = jobUi_.find(id);
        if (uiIt == jobUi_.end()) continue;
        const TransferState s = uiIt->second.state;
        if (s == TransferState::Done || s == TransferState::Failed ||
            s == TransferState::Canceled || s == TransferState::Skipped) {
            toRemove.push_back(id);
        }
    }
    for (std::uint64_t id : toRemove) {
        delete transferItems_[id];
        transferItems_.erase(id);
        jobUi_.erase(id);
    }
    updateTransferTabCounts();
    if (transferItems_.empty()) transferDock_->hide();
}

void MainWindow::clearTransferRows() {
    for (auto& [id, item] : transferItems_) {
        (void)id;
        delete item;
    }
    transferItems_.clear();
    jobUi_.clear();
    updateTransferTabCounts();
}

void MainWindow::onEmergencyStop() {
    // One user action must be enough even with 17,000 queued files:
    // invalidate pending scans, stop live jobs in one lock, and make sure a
    // restart does not resurrect the batch from the journal.
    ++queueStopEpoch_;
    for (const auto& flag : treeAbortFlags_) {
        if (flag) flag->store(true);
    }
    treeAbortFlags_.clear();

    std::size_t stopped = 0;
    if (queue_) {
        queue_->pauseAll();
        stopped = queue_->cancelAll();   // also truncates the journal
        queue_->resumeAll();             // leave the queue usable afterwards
    }
    if (pauseBtn_) pauseBtn_->setChecked(false);
    clearTransferRows();
    if (transferDock_) transferDock_->hide();

    const QString msg = QString("已紧急停止：取消 %1 个任务，恢复记录已清空")
                            .arg(stopped);
    statusBar()->showMessage(msg, 8000);
    appendLog(msg);
}

void MainWindow::onClearAll() {
    // Same instant, O(n) path as the emergency brake. The old per-id cancel
    // loop was O(n^2) and could itself hang the UI on a huge queue.
    onEmergencyStop();
}

void MainWindow::refreshStatusBar() {
    const std::string rel = model_->currentDir();
    const QString where = rel.empty()
        ? QString("(root)")
        : QString::fromStdString(rel);
    const auto st = protocolEngineStatus();
    RemoteFsModel* rm = currentRemote();
    QString remote = (rm && rm->isConnected())
        ? QString::fromStdString(rm->currentPath())
        : QString("-");
    statusLabel_->setText(QString("%1 | %2 行 | 远端: %3 | engines: ftp=%4 "
                                  "ssh=%5 zstd=%6 exec=%7")
                              .arg(where)
                              .arg(model_->rowCount({}))
                              .arg(remote)
                              .arg(st.ftpAvailable).arg(st.sshAvailable)
                              .arg(st.zstdAvailable)
                              .arg(st.execChannelCompression));
}

}  // namespace ftpflow
