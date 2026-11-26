// ftpflow -- remote connection dialog implementation.
#include "connect_dialog.hpp"

#include "dialog_title_bar.hpp"
#include "file_icon.hpp"
#include "name_edit_delegate.hpp"
#include "ui_util.hpp"

#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QDate>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstdio>

#include "ftpflow/known_hosts.hpp"
#include <fstream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#endif

namespace ftpflow {
namespace {

std::string readWholeFileStr(const QString& path) {
    std::ifstream f(path.toStdString(), std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

// A small "eye" glyph used to toggle the password field between masked and
// plain text. `slashed` draws the eye with a diagonal strike (hidden state).
QIcon makeEyeIcon(bool slashed) {
    const int s = 32;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c(0xd4, 0xd4, 0xd4);
    QPen pen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);

    const qreal cx = s / 2.0, cy = s / 2.0;
    // Eye outline (an almond shape approximated by two arcs).
    QPainterPath eye;
    eye.moveTo(5, cy);
    eye.cubicTo(11, cy - 7, 21, cy - 7, 27, cy);
    eye.cubicTo(21, cy + 7, 11, cy + 7, 5, cy);
    eye.closeSubpath();
    p.drawPath(eye);
    // Pupil.
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(cx, cy), 3.6, 3.6);

    if (slashed) {
        QPen slash(QColor(0xf4, 0x87, 0x71), 2.2, Qt::SolidLine,
                   Qt::RoundCap, Qt::RoundJoin);
        p.setPen(slash);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(7, 26), QPointF(26, 7));
    }
    p.end();
    return QIcon(pm);
}

// Tree item roles carrying the selected site name. kOriginalNameRole keeps
// the name as it was when the node was built: itemChanged() compares it
// against text(0) to tell a real inline edit from a programmatic rebuild.
constexpr int kSiteNameRole = Qt::UserRole + 1;
constexpr int kIsGroupRole = Qt::UserRole + 2;
constexpr int kOriginalNameRole = Qt::UserRole + 3;

// QTreeWidget::editItem() goes through QAbstractItemView::edit(), which may
// consult editTriggers. Temporarily allow every trigger for this one
// programmatic edit and restore the setting immediately afterwards.
void beginTreeInlineEdit(QTreeWidget* tree, QTreeWidgetItem* item) {
    if (!tree || !item) return;
    const QAbstractItemView::EditTriggers saved = tree->editTriggers();
    tree->setEditTriggers(QAbstractItemView::AllEditTriggers);
    tree->editItem(item, 0);
    tree->setEditTriggers(saved);
}

// A flat "server" glyph for site nodes (matches the file-icon palette).
QIcon makeSiteIcon() {
    const int s = 16;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c(0x2e, 0xa8, 0xc0);   // teal accent
    // Two stacked server bays.
    p.setPen(QPen(c, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2.0, 2.0, 12.0, 5.0), 1.0, 1.0);
    p.drawRoundedRect(QRectF(2.0, 9.0, 12.0, 5.0), 1.0, 1.0);
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(5.0, 4.5), 0.9, 0.9);   // status dots
    p.drawEllipse(QPointF(5.0, 11.5), 0.9, 0.9);
    p.end();
    return QIcon(pm);
}

// A flat "folder" glyph for group nodes (amber, matching folderIcon()).
QIcon makeGroupIcon() {
    const int s = 16;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor body(0xe0, 0xa9, 0x3e);   // amber
    const QColor front(0xb8, 0x86, 0x2b);
    // Tab.
    p.setPen(Qt::NoPen);
    p.setBrush(body);
    p.drawRoundedRect(QRectF(2.0, 3.0, 6.0, 3.0), 1.0, 1.0);
    // Body.
    p.drawRoundedRect(QRectF(2.0, 5.0, 12.0, 8.0), 1.0, 1.0);
    // Front face.
    p.setBrush(front);
    p.drawRoundedRect(QRectF(2.0, 8.0, 12.0, 5.0), 1.0, 1.0);
    p.end();
    return QIcon(pm);
}

}  // namespace

ConnectDialog::ConnectDialog(QWidget* parent, SiteStore* store)
    : QDialog(parent), store_(store) {
    buildUi();
}

void ConnectDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
#ifdef Q_OS_WIN
    // Windows 11 native rounded corners, matching the main window chrome.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    const DWM_WINDOW_CORNER_PREFERENCE pref =
        static_cast<DWM_WINDOW_CORNER_PREFERENCE>(DWMWCP_ROUND);
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref,
                          sizeof(pref));
#else
    (void)event;
#endif
}

void ConnectDialog::buildUi() {
    // Frameless dialog with a custom title bar matching the main window.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setWindowTitle("连接远端");
    setMinimumWidth(780);
    setMinimumHeight(600);

    // ---- left column: searchable site tree (XTerminal-style grouping) ----
    auto* left = new QWidget(this);
    auto* leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);

    filter_ = new QLineEdit(left);
    filter_->setPlaceholderText("搜索站点（按名称/主机过滤）...");
    filter_->setClearButtonEnabled(true);

    tree_ = new QTreeWidget(left);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    // Renames are inline: F2 or the context menu turns the node's name into
    // a QLineEdit. Double-click must not silently start an edit.
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    tree_->setItemDelegate(new NameEditDelegate(tree_));

    auto* siteTools = new QHBoxLayout;
    auto* newSiteBtn = new QPushButton("新建站点", left);
    auto* exportBtn = new QPushButton("导出", left);
    auto* importBtn = new QPushButton("导入", left);
    deleteSiteBtn_ = new QPushButton("删除站点", left);
    deleteSiteBtn_->setProperty("danger", true);
    siteTools->addWidget(newSiteBtn);
    siteTools->addWidget(exportBtn);
    siteTools->addWidget(importBtn);
    siteTools->addStretch(1);
    siteTools->addWidget(deleteSiteBtn_);

    leftLay->addWidget(filter_);
    leftLay->addWidget(tree_, 1);
    leftLay->addLayout(siteTools);

    // ---- right column: manual connection form ----------------------------
    auto* right = new QWidget(this);
    auto* form = new QFormLayout(right);
    form->setContentsMargins(16, 0, 8, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // A slim section heading (bold, muted) used to group related fields.
    auto makeSection = [&](const QString& text) {
        auto* lbl = new QLabel(text, right);
        QFont f = lbl->font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() * 0.95);
        lbl->setFont(f);
        lbl->setStyleSheet("color: #9d9d9d; padding-top: 6px;");
        return lbl;
    };

    form->addRow(makeSection("连接"));

    proto_ = new QComboBox(right);
    proto_->addItem("FTP");
    proto_->addItem("SFTP (SSH)");
    form->addRow("协议:", proto_);

    name_ = new QLineEdit(right);
    name_->setPlaceholderText("站点显示名称（留空则用主机名）");
    form->addRow("名称:", name_);

    // Host + port share one row for a compact, professional look.
    host_ = new QLineEdit(right);
    host_->setPlaceholderText("主机名或 IP");
    port_ = new QLineEdit(right);
    port_->setValidator(new QIntValidator(1, 65535, right));
    port_->setFixedWidth(72);
    port_->setAlignment(Qt::AlignCenter);
    auto* hostRow = new QWidget(right);
    auto* hostLay = new QHBoxLayout(hostRow);
    hostLay->setContentsMargins(0, 0, 0, 0);
    hostLay->setSpacing(6);
    hostLay->addWidget(host_, 1);
    hostLay->addWidget(new QLabel(":", right));
    hostLay->addWidget(port_);
    form->addRow("主机:", hostRow);

    form->addRow(makeSection("认证"));

    user_ = new QLineEdit(right);
    user_->setPlaceholderText("用户名");
    form->addRow("用户名:", user_);

    password_ = new QLineEdit(right);
    password_->setEchoMode(QLineEdit::Normal);   // new site: show the password
    password_->setPlaceholderText("密码（SFTP 可留空改用密钥）");
    // Toggle button to hide/reveal the plaintext password for easy copying.
    revealPwAct_ = password_->addAction(makeEyeIcon(true),
                                        QLineEdit::TrailingPosition);
    revealPwAct_->setToolTip("隐藏密码");
    connect(revealPwAct_, &QAction::triggered, this, [this] {
        const bool hidden = password_->echoMode() == QLineEdit::Password;
        password_->setEchoMode(hidden ? QLineEdit::Normal
                                      : QLineEdit::Password);
        revealPwAct_->setIcon(makeEyeIcon(!hidden));
        revealPwAct_->setToolTip(hidden ? "隐藏密码" : "显示密码");
    });
    form->addRow("密码:", password_);

    keyPath_ = new QLineEdit(right);
    keyPath_->setPlaceholderText("PEM 私钥路径（可选，SFTP）");
    browseKeyBtn_ = new QPushButton("浏览...", right);
    auto* keyRow = new QWidget(right);
    auto* keyLay = new QHBoxLayout(keyRow);
    keyLay->setContentsMargins(0, 0, 0, 0);
    keyLay->setSpacing(6);
    keyLay->addWidget(keyPath_, 1);
    keyLay->addWidget(browseKeyBtn_);
    form->addRow("私钥:", keyRow);

    form->addRow(makeSection("路径与高级"));

    startDir_ = new QLineEdit(right);
    startDir_->setPlaceholderText("远端起始目录（SFTP 默认 \".\"，FTP 默认 \"/\"）");
    form->addRow("起始目录:", startDir_);

    encoding_ = new QComboBox(right);
    encoding_->addItem("自动（UTF-8 优先，否则 GBK）", "auto");
    encoding_->addItem("UTF-8", "utf-8");
    encoding_->addItem("GBK / GB2312（中文 Windows/IIS）", "gbk");
    form->addRow("文件名编码:", encoding_);

    pushCmd_ = new QLineEdit(right);
    pushCmd_->setPlaceholderText(
        "远端解压命令模板，{file}=目标路径；默认 \"zstd -d -c - -o {file}\"");
    form->addRow("解压命令:", pushCmd_);

    sftpCmd_ = new QLineEdit(right);
    sftpCmd_->setPlaceholderText(
        "自定义 SFTP 启动命令（可选），如 sudo su -c /usr/libexec/openssh/sftp-server；"
        "留空用默认 subsystem sftp。需服务器配置 sudo NOPASSWD");
    sftpCmd_->setToolTip(
        "某些目录登录用户无写权限时，可用提权命令启动 sftp-server。\n"
        "留空 = 默认 subsystem sftp；服务器需允许该命令免密 sudo。");
    form->addRow("SFTP启动命令:", sftpCmd_);

    group_ = new QLineEdit(right);
    group_->setPlaceholderText("分组名（如 \"生产\" / \"测试\"，可留空）");
    form->addRow("分组:", group_);

    note_ = new QPlainTextEdit(right);
    note_->setPlaceholderText("备注（可选，多行）\n粘贴含网址/IP/账号/密码的文本后点「智能填充」可自动识别");
    note_->setFixedHeight(56);
    smartFillBtn_ = new QPushButton("智能填充", right);
    smartFillBtn_->setToolTip("识别备注框里的网址、IP、FTP 账号、密码并自动填入对应字段");
    auto* noteRow = new QHBoxLayout;
    noteRow->setContentsMargins(0, 0, 0, 0);
    noteRow->setSpacing(8);
    noteRow->addWidget(note_, 1);
    noteRow->addWidget(smartFillBtn_, 0, Qt::AlignTop);
    form->addRow("备注:", noteRow);

    auto* connectBtn = new QPushButton("连接", right);
    connectBtn->setProperty("accent", true);
    auto* okBtn = new QPushButton("确定", right);
    auto* cancelBtn = new QPushButton("取消", right);
    connectBtn->setDefault(true);
    errorLabel_ = new QLabel(right);
    errorLabel_->setStyleSheet("color: #f48771;");   // theme danger red
    // The label stays in the button row even when empty: its stretch
    // absorbs the row's slack so the three buttons keep their natural
    // text-sized width and sit right-aligned instead of being stretched.
    errorLabel_->setText(QString());
    auto* btns = new QHBoxLayout;
    btns->setContentsMargins(12, 8, 12, 12);
    btns->addWidget(errorLabel_, 1);
    btns->addWidget(connectBtn);
    btns->addWidget(okBtn);
    btns->addWidget(cancelBtn);
    btns->setSpacing(14);   // space the three buttons apart

    // ---- assemble split layout -------------------------------------------
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({260, 500});
    splitter->setContentsMargins(0, 8, 0, 0);

    // Custom title bar sits above the split layout (matches the main window).
    auto* titleBar = new DialogTitleBar(this);
    titleBar->setTitle("连接远端");
    titleBar->attachWindow(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(titleBar);
    root->addWidget(splitter, 1);
    root->addLayout(btns);

    // ---- wiring ----------------------------------------------------------
    connect(filter_, &QLineEdit::textChanged, this,
            &ConnectDialog::onFilterChanged);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            &ConnectDialog::onSiteChosen);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            &ConnectDialog::onTreeContextMenu);
    // Commit an inline rename when the editor closes (Enter / focus change).
    connect(tree_, &QTreeWidget::itemChanged, this,
            &ConnectDialog::onTreeItemChanged);
    // Editing the host by hand means the user is no longer on the loaded
    // site — treat the next save as a new site rather than an in-place edit.
    connect(host_, &QLineEdit::textEdited, this, [this](const QString&) {
        editingName_.clear();
        if (errorLabel_) errorLabel_->clear();
    });
    // Typing into a previously flagged field clears the validation hint.
    for (QLineEdit* e : {user_, port_}) {
        connect(e, &QLineEdit::textEdited, this, [this](const QString&) {
            if (errorLabel_) errorLabel_->clear();
        });
    }
    connect(deleteSiteBtn_, &QPushButton::clicked, this,
            &ConnectDialog::onDeleteSite);
    connect(newSiteBtn, &QPushButton::clicked, this, [this] {
        resetFormForNewSite(QString());
    });
    connect(exportBtn, &QPushButton::clicked, this,
            &ConnectDialog::onExportSites);
    connect(importBtn, &QPushButton::clicked, this,
            &ConnectDialog::onImportSites);
    // 连接: save the site, then connect.
    connect(connectBtn, &QPushButton::clicked, this, [this] {
        if (!validateForm()) return;
        onSaveSite();
        // Remember this site so the next dialog re-selects it by default.
        if (!editingName_.isEmpty())
            QSettings().setValue("site/lastConnected", editingName_);
        saveOnly_ = false;
        accept();
    });
    // 确定: save the site and close WITHOUT connecting (FileZilla OK).
    connect(okBtn, &QPushButton::clicked, this, [this] {
        if (!validateForm()) return;
        onSaveSite();
        saveOnly_ = true;
        accept();
    });
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(proto_, &QComboBox::currentIndexChanged, this,
            &ConnectDialog::onProtocolChanged);
    connect(browseKeyBtn_, &QPushButton::clicked, this,
            &ConnectDialog::onBrowseKey);
    connect(smartFillBtn_, &QPushButton::clicked, this,
            &ConnectDialog::onSmartFill);

    rebuildSiteTree();
    onProtocolChanged(0);
    // Default the group field to the current year-month (YYYYMM) so a fresh
    // dialog is ready to file the site right away.
    group_->setText(currentYearMonth());

    // Re-select the most recently connected site so the form is pre-filled
    // and the user can just hit 连接 (FileZilla-style last-used recall).
    const QString last = QSettings().value("site/lastConnected").toString();
    if (!last.isEmpty()) selectSiteInTree(last);
}

void ConnectDialog::rebuildSiteTree(const QString& filter) {
    if (!store_ || !tree_) return;
    tree_->clear();

    const QString needle = filter.trimmed();
    auto matches = [&](const SiteEntry& e) {
        if (needle.isEmpty()) return true;
        return e.name.contains(needle, Qt::CaseInsensitive) ||
               e.host.contains(needle, Qt::CaseInsensitive) ||
               e.user.contains(needle, Qt::CaseInsensitive);
    };

    // Group top-level nodes first, then an "未分组" node for the rest.
    // Every group gets a node (including empty folders) so standalone
    // folders remain visible even before they contain any sites.
    const QIcon groupIcon = makeGroupIcon();
    const QIcon siteIcon = makeSiteIcon();
    for (const QString& g : store_->groups()) {
        QTreeWidgetItem* groupNode = nullptr;
        for (const SiteEntry& e : store_->sitesInGroup(g)) {
            if (!matches(e)) continue;
            if (!groupNode) {
                groupNode = new QTreeWidgetItem(tree_);
                groupNode->setText(0, g);
                groupNode->setIcon(0, groupIcon);
                groupNode->setData(0, kOriginalNameRole, g);
                groupNode->setData(0, kIsGroupRole, true);
                groupNode->setFlags(
                    (groupNode->flags() & ~Qt::ItemIsSelectable) |
                    Qt::ItemIsEditable);
            }
            auto* item = new QTreeWidgetItem(groupNode);
            item->setText(0, e.name);
            item->setIcon(0, siteIcon);
            item->setToolTip(0, QString("%1@%2:%3")
                .arg(e.user, e.host).arg(e.port));
            item->setData(0, kOriginalNameRole, e.name);
            item->setData(0, kSiteNameRole, e.name);
            item->setData(0, kIsGroupRole, false);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
        if (!groupNode) {
            // Empty folder (no matching sites): still show the node.
            groupNode = new QTreeWidgetItem(tree_);
            groupNode->setText(0, g);
            groupNode->setIcon(0, groupIcon);
            groupNode->setData(0, kOriginalNameRole, g);
            groupNode->setData(0, kIsGroupRole, true);
            groupNode->setFlags(
                (groupNode->flags() & ~Qt::ItemIsSelectable) |
                Qt::ItemIsEditable);
        }
        groupNode->setExpanded(true);
    }
    // Ungrouped sites.
    QTreeWidgetItem* ungrouped = nullptr;
    for (const SiteEntry& e : store_->sitesInGroup(QString())) {
        if (!matches(e)) continue;
        if (!ungrouped) {
            ungrouped = new QTreeWidgetItem(tree_);
            ungrouped->setText(0, "未分组");
            ungrouped->setIcon(0, groupIcon);
            ungrouped->setData(0, kIsGroupRole, true);
            ungrouped->setFlags(ungrouped->flags() & ~Qt::ItemIsSelectable);
        }
        auto* item = new QTreeWidgetItem(ungrouped);
        item->setText(0, e.name);
        item->setIcon(0, siteIcon);
        item->setToolTip(0, QString("%1@%2:%3")
            .arg(e.user, e.host).arg(e.port));
        item->setData(0, kOriginalNameRole, e.name);
        item->setData(0, kSiteNameRole, e.name);
        item->setData(0, kIsGroupRole, false);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    if (ungrouped) ungrouped->setExpanded(true);
    // When filtering, expand everything so matches are visible.
    if (!needle.isEmpty()) tree_->expandAll();
}

void ConnectDialog::onFilterChanged(const QString& text) {
    rebuildSiteTree(text);
}

void ConnectDialog::onSiteChosen(QTreeWidgetItem* current,
                                 QTreeWidgetItem* /*previous*/) {
    if (!store_ || !current) return;
    if (current->data(0, kIsGroupRole).toBool()) return;
    const QString name = current->data(0, kSiteNameRole).toString();
    for (const SiteEntry& e : store_->sites()) {
        if (e.name != name) continue;
        editingName_ = name;   // remember the site being edited
        loadSite(store_->withPassword(e));
        return;
    }
}

void ConnectDialog::onProtocolChanged(int index) {
    const bool sftp = index == 1;
    // Only reset the port when it is empty or still the OTHER protocol's
    // default; a custom port the user typed survives a protocol switch.
    const QString cur = port_->text().trimmed();
    const QString def = sftp ? QStringLiteral("22") : QStringLiteral("21");
    const QString otherDef = sftp ? QStringLiteral("21") : QStringLiteral("22");
    if (cur.isEmpty() || cur == otherDef) port_->setText(def);
    keyPath_->setEnabled(sftp);
    browseKeyBtn_->setEnabled(sftp);
    pushCmd_->setEnabled(sftp);
    sftpCmd_->setEnabled(sftp);
    if (sftp && startDir_->text().isEmpty()) startDir_->setText(".");
    if (!sftp && startDir_->text() == ".") startDir_->setText("/");

    // Group default: FTP uses the current year-month (YYYYMM); SFTP (SSH)
    // uses a single "sftp(ssh)" group since SSH sites are few. Only swap
    // the auto value — a hand-typed group is never overwritten.
    const QString ym = currentYearMonth();
    const QString sftpGroup = QStringLiteral("sftp(ssh)");
    const QString curGroup = group_->text().trimmed();
    if (sftp) {
        if (curGroup.isEmpty() || curGroup == ym) group_->setText(sftpGroup);
    } else {
        if (curGroup == sftpGroup) group_->setText(ym);
    }
}

void ConnectDialog::onBrowseKey() {
    const QString path = QFileDialog::getOpenFileName(
        this, "选择 PEM 私钥", {}, "密钥文件 (*)");
    if (!path.isEmpty()) keyPath_->setText(path);
}

void ConnectDialog::onSmartFill() {
    const QString text = note_->toPlainText().trimmed();
    if (text.isEmpty()) {
        if (errorLabel_) errorLabel_->setText("请先在备注框粘贴含网址/IP/账号/密码的文本");
        note_->setFocus();
        return;
    }

    QString host, user, password, name, port;
    auto firstGroup = [](const QString& s, const QRegularExpression& re) {
        const auto m = re.match(s);
        return m.hasMatch() ? m.captured(1) : QString();
    };

    // 网址（用于「名称」）：优先 www. 开头的域名，其次任意含字母的域名。
    //    纯数字点分（IP）与数据库地址不算「网址」。
    static const QRegularExpression hostRe(
        QStringLiteral(
            "([A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
             "(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+)"));
    static const QRegularExpression pureIpRe(
        QStringLiteral("^\\d{1,3}(?:\\.\\d{1,3}){3}$"));
    QString wwwSite;   // 明确以 www. 开头的网站网址（去 www. 后作名称）
    QString domain;    // 首个含字母的域名（IP 之外的兜底）
    {
        // 数据库/连接地址等标签后的域名不是 FTP 网址，跳过，避免把
        // "连接地址：mssql06.dbs.71e.com" 误当网站名。
        static const QRegularExpression dbTagRe(
            QStringLiteral(
                "(连接地址|数据库地址|数据库|DB|database|db)\\s*[:：=]\\s*$"));
        auto it = hostRe.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString cand = m.captured(1);
            if (pureIpRe.match(cand).hasMatch()) continue;  // 跳过纯 IP
            // 该候选之前若紧跟数据库/连接地址标签，则视为数据库地址而非网址。
            const QString before = text.left(m.capturedStart());
            if (dbTagRe.match(before).hasMatch()) continue;
            if (domain.isEmpty()) domain = cand;
            if (cand.startsWith(QStringLiteral("www."),
                                Qt::CaseInsensitive) && wwwSite.isEmpty())
                wwwSite = cand;
        }
    }

    // 主机（用于「主机」文本框）：优先 IP，否则退回域名。
    //    IP 形如 116.196.115.111。
    static const QRegularExpression ipRe(
        QStringLiteral("\\b(\\d{1,3}(?:\\.\\d{1,3}){3})\\b"));
    host = firstGroup(text, ipRe);
    if (host.isEmpty()) host = domain;

    // 2) 用户名：FTP账号 / 账号 / 用户名 / username / user 后面的值。
    static const QRegularExpression userRe(QStringLiteral(
        "(?:FTP\\s*账号|FTP\\s*帐号|账号|帐号|用户名|username|user|login)"
        "\\s*[:：=]\\s*([^\\s;；,，]+)"),
        QRegularExpression::CaseInsensitiveOption);
    user = firstGroup(text, userRe);

    // 3) 密码：FTP密码 / 密码 / password / passwd / pwd 后面的值。
    //    密码可能含特殊字符（如 MQ70v]y6lD），用空白/逗号/分号/换行截断。
    static const QRegularExpression passRe(QStringLiteral(
        "(?:FTP\\s*密码|密码|password|passwd|pwd|pass)"
        "\\s*[:：=]\\s*([^\\s;；,，]+)"),
        QRegularExpression::CaseInsensitiveOption);
    password = firstGroup(text, passRe);

    // 4) 端口：可选，显式 "端口: 2121" 或 "host:2121"。
    static const QRegularExpression portRe(QStringLiteral(
        "(?:端口|port)\\s*[:：=]\\s*(\\d{1,5})"),
        QRegularExpression::CaseInsensitiveOption);
    port = firstGroup(text, portRe);
    if (port.isEmpty() && !domain.isEmpty()) {
        static const QRegularExpression hostPortRe(QStringLiteral(
            "\\b[A-Za-z0-9.-]+:(\\d{1,5})\\b"));
        const auto m = hostPortRe.match(text);
        if (m.hasMatch()) port = m.captured(1);
    }

    // 5) 名称优先级：www 网址（去 www.）→ 含字母域名 → FTP 账号 → 主机。
    //    无网址时优先用账号作名称（账号比 IP 更有辨识度，很多 IP 相同）。
    if (!wwwSite.isEmpty()) {
        name = wwwSite.mid(4);   // strip "www."
    } else if (!domain.isEmpty()) {
        name = domain;
    } else if (!user.isEmpty()) {
        name = user;
    } else if (!host.isEmpty()) {
        name = host;
    }

    int filled = 0;
    if (!name.isEmpty()) { name_->setText(name); ++filled; }
    if (!host.isEmpty()) { host_->setText(host); ++filled; }
    if (!user.isEmpty()) { user_->setText(user); ++filled; }
    if (!password.isEmpty()) {
        password_->setText(password);
        password_->setEchoMode(QLineEdit::Normal);   // freshly filled: show it
        if (revealPwAct_) {
            revealPwAct_->setIcon(makeEyeIcon(true));
            revealPwAct_->setToolTip("隐藏密码");
        }
        ++filled;
    }
    if (!port.isEmpty()) { port_->setText(port); ++filled; }

    if (filled == 0) {
        if (errorLabel_) errorLabel_->setText("未识别出可填充的字段，请检查文本格式");
        return;
    }
    if (errorLabel_) errorLabel_->clear();
    // 焦点移到主机框，方便用户核对或继续填写。
    host_->setFocus();
}

bool ConnectDialog::validateForm() {
    auto fail = [this](QWidget* focus, const QString& msg) {
        if (errorLabel_) errorLabel_->setText(msg);
        focus->setFocus();
        return false;
    };
    if (errorLabel_) errorLabel_->clear();
    if (host_->text().trimmed().isEmpty())
        return fail(host_, "请填写主机地址");
    if (user_->text().trimmed().isEmpty())
        return fail(user_, "请填写用户名");
    // Empty port (the validator allows blank) would connect to port 0 --
    // fill in the protocol default instead.
    if (port_->text().trimmed().isEmpty())
        port_->setText(proto_->currentIndex() == 1
                           ? QStringLiteral("22") : QStringLiteral("21"));
    return true;
}

void ConnectDialog::onSaveSite() {
    if (!store_) return;
    if (!validateForm()) return;
    // Name comes straight from the form (falls back to the host).
    QString name = name_->text().trimmed();
    if (name.isEmpty()) name = host_->text().trimmed();

    SiteEntry e;
    e.name = name;
    e.isSftp = proto_->currentIndex() == 1;
    e.host = host_->text().trimmed();
    e.port = port_->text().toInt();
    e.user = user_->text().trimmed();
    e.keyPath = keyPath_->text().trimmed();
    e.startDir = startDir_->text().trimmed();
    e.pushCmd = pushCmd_->text().trimmed();
    e.sftpCmd = sftpCmd_->text().trimmed();
    e.group = group_->text().trimmed();
    e.encoding = encoding_->currentData().toString();
    e.note = note_->toPlainText().trimmed();

    // Editing an existing site: update it in place (rename supported, the
    // credential is carried to the new name). Otherwise insert a new site.
    if (!editingName_.isEmpty() && editingName_ != name) {
        // Rename: drop the old entry, migrate its stored password.
        const QString oldPw = store_->readPassword(editingName_);
        store_->remove(editingName_);
        store_->erasePassword(editingName_);
        store_->upsert(e);
        if (!password_->text().isEmpty()) {
            store_->writePassword(e.name, password_->text());
        } else if (!oldPw.isEmpty()) {
            store_->writePassword(e.name, oldPw);
        }
    } else {
        store_->upsert(e);
        if (store_->save() && !password_->text().isEmpty()) {
            store_->writePassword(e.name, password_->text());
        }
    }
    store_->save();
    editingName_ = e.name;   // now editing under the (possibly new) name
    rebuildSiteTree(filter_ ? filter_->text() : QString());
}

namespace {

// Delete confirmation: "Yes" is the default action and owns the keyboard
// focus, so a plain Enter press confirms the deletion (same behaviour as
// the file panes in the main window).
bool confirmDeletion(QWidget* parent, const QString& title,
                     const QString& message) {
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(title);
    box.setText(message);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::Yes);
    if (QAbstractButton* yes = box.button(QMessageBox::Yes))
        yes->setFocus();
    box.exec();
    return box.clickedButton() == box.button(QMessageBox::Yes);
}

}  // namespace

void ConnectDialog::onDeleteSite() {
    if (!store_ || !tree_) return;
    const auto* cur = tree_->currentItem();
    if (!cur || cur->data(0, kIsGroupRole).toBool()) return;
    const QString name = cur->data(0, kSiteNameRole).toString();
    if (!confirmDeletion(
            this, "删除站点",
            QString("确定删除站点 \"%1\" 吗？（密码凭证也会一并删除）").arg(name)))
        return;
    store_->remove(name);
    store_->erasePassword(name);
    store_->save();
    rebuildSiteTree(filter_ ? filter_->text() : QString());
}

void ConnectDialog::onTreeContextMenu(const QPoint& pos) {
    if (!store_ || !tree_) return;
    QTreeWidgetItem* item = tree_->itemAt(pos);

    // Blank area (no node under the cursor): new site or folder/group.
    if (!item) {
        QMenu menu(this);
        QAction* newSite = menu.addAction("新建站点");
        QAction* newFolder = menu.addAction("新建文件夹");
        QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
        if (chosen == newSite) {
            resetFormForNewSite(QString());
            return;
        }
        if (chosen != newFolder) return;
        bool ok = false;
        const QString ng = QInputDialog::getText(
            this, "新建文件夹", "文件夹名称:", QLineEdit::Normal, QString(), &ok);
        if (!ok || ng.trimmed().isEmpty()) return;
        store_->addFolder(ng.trimmed());
        store_->save();
        rebuildSiteTree(filter_ ? filter_->text() : QString());
        return;
    }

    tree_->setCurrentItem(item);

    const bool isGroup = item->data(0, kIsGroupRole).toBool();
    const QString siteName = item->data(0, kSiteNameRole).toString();

    QMenu menu(this);

    if (isGroup) {
        // Group node: new site under it + group management.
        const QString group = item->text(0);
        QAction* newSite = menu.addAction("新建站点");
        menu.addSeparator();
        QAction* renameGroup = menu.addAction("重命名文件夹");
        QAction* deleteGroup = menu.addAction("删除文件夹");
        QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
        if (chosen == newSite) {
            resetFormForNewSite(group);
        } else if (chosen == renameGroup) {
            // Inline rename directly on the node's name (no input dialog).
            beginTreeInlineEdit(tree_, item);
        } else if (chosen == deleteGroup) {
            if (!confirmDeletion(
                    this, "删除文件夹",
                    QString("确定删除文件夹 \"%1\" 吗？（站点会移到“未分组”）")
                        .arg(group)))
                return;
            store_->removeFolder(group);
            store_->save();
            rebuildSiteTree(filter_ ? filter_->text() : QString());
        }
        return;
    }

    // Site node: load it (already done by selection), plus common ops.
    auto findEntry = [&](SiteEntry* out) -> bool {
        for (const SiteEntry& e : store_->sites()) {
            if (e.name == siteName) { *out = e; return true; }
        }
        return false;
    };
    SiteEntry entry;
    const bool found = findEntry(&entry);

    QAction* connectAct = menu.addAction("连接");
    menu.addSeparator();
    QAction* newSiteAct = menu.addAction("新建站点");
    QAction* dupAct = menu.addAction("复制站点");
    QAction* renameAct = menu.addAction("重命名站点");
    menu.addSeparator();
    QAction* moveGroupAct = menu.addAction("移动到分组...");
    QAction* copyHostAct = menu.addAction("复制主机");
    QAction* copyUserAct = menu.addAction("复制用户名");
    QAction* copyPathAct = menu.addAction("复制路径");
    menu.addSeparator();
    QAction* deleteAct = menu.addAction("删除站点");
    if (found) deleteAct->setEnabled(true);

    QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == connectAct) {
        if (found) {
            loadSite(store_->withPassword(entry));
            if (!host_->text().trimmed().isEmpty() &&
                !user_->text().trimmed().isEmpty()) {
                accept();
            }
        }
    } else if (chosen == newSiteAct) {
        resetFormForNewSite(found ? entry.group : QString());
    } else if (chosen == dupAct) {
        duplicateSite(siteName);
    } else if (chosen == renameAct) {
        // Inline rename directly on the node's name (no input dialog).
        beginTreeInlineEdit(tree_, item);
    } else if (chosen == moveGroupAct) {
        bool ok = false;
        const QString ng = QInputDialog::getText(
            this, "移动到分组", "分组名（留空=未分组）:",
            QLineEdit::Normal, entry.group, &ok);
        if (!ok) return;
        for (SiteEntry e : store_->sites()) {
            if (e.name != siteName) continue;
            e.group = ng.trimmed();
            store_->upsert(e);
            break;
        }
        store_->save();
        rebuildSiteTree(filter_ ? filter_->text() : QString());
    } else if (chosen == copyHostAct) {
        QApplication::clipboard()->setText(found ? entry.host : QString());
    } else if (chosen == copyUserAct) {
        QApplication::clipboard()->setText(found ? entry.user : QString());
    } else if (chosen == copyPathAct) {
        QApplication::clipboard()->setText(found ? entry.startDir : QString());
    } else if (chosen == deleteAct) {
        onDeleteSite();
    }
}

// An inline rename editor just committed. itemChanged also fires while the
// tree is rebuilt programmatically, so only act when the node carries its
// original-name role and the text really changed.
void ConnectDialog::onTreeItemChanged(QTreeWidgetItem* item, int column) {
    if (!item || !store_ || column != 0) return;
    const QString oldName = item->data(0, kOriginalNameRole).toString();
    if (oldName.isEmpty()) return;   // programmatic build / read-only node
    const QString newName = item->text(0).trimmed();

    auto revert = [this, item, oldName] {
        if (item->text(0) == oldName) return;
        QSignalBlocker blocker(tree_);
        item->setText(0, oldName);
    };
    auto reject = [this, &revert](const QString& why) {
        revert();
        QToolTip::showText(tree_->viewport()->mapToGlobal(QPoint(12, 12)),
                           why, tree_, QRect(), 2500);
    };

    if (newName == oldName) { revert(); return; }
    if (!isValidEntryName(newName)) {
        reject(QStringLiteral("名称为空或包含非法字符"));
        return;
    }

    if (item->data(0, kIsGroupRole).toBool()) {
        for (const QString& g : store_->groups()) {
            if (g == newName) {
                reject(QStringLiteral("已存在同名文件夹"));
                return;
            }
        }
        store_->renameFolder(oldName, newName);
    } else {
        for (const SiteEntry& e : store_->sites()) {
            if (e.name == newName) {
                reject(QStringLiteral("已存在同名站点"));
                return;
            }
        }
        // Credentials are keyed by site name, so move the saved password to
        // the new name exactly like the old dialog-based rename did.
        for (SiteEntry e : store_->sites()) {
            if (e.name != oldName) continue;
            const QString oldPw = store_->readPassword(oldName);
            store_->remove(oldName);
            store_->erasePassword(oldName);
            e.name = newName;
            store_->upsert(e);
            if (!oldPw.isEmpty()) store_->writePassword(e.name, oldPw);
            break;
        }
        if (editingName_ == oldName) editingName_ = newName;
    }
    store_->save();
    // Let the editor finish closing before the tree is rebuilt (it destroys
    // the very item that emitted this signal).
    QTimer::singleShot(0, this, [this] {
        rebuildSiteTree(filter_ ? filter_->text() : QString());
    });
}

void ConnectDialog::onExportSites() {
    if (!store_) return;
    const QString path = QFileDialog::getSaveFileName(
        this, "导出站点", "ftpflow-sites.json", "JSON (*.json)");
    if (path.isEmpty()) return;
    QString err;
    if (store_->exportTo(path, &err)) {
        QMessageBox::information(this, "导出成功",
            QString("已导出 %1 个站点到 %2（密码不包含在导出文件中）")
                .arg(store_->sites().size()).arg(path));
    } else {
        QMessageBox::warning(this, "导出失败", err);
    }
}

void ConnectDialog::onImportSites() {
    if (!store_) return;
    const QString path = QFileDialog::getOpenFileName(
        this, "导入站点", {}, "JSON (*.json)");
    if (path.isEmpty()) return;
    QString err;
    if (store_->importFrom(path, &err)) {
        store_->save();
        rebuildSiteTree(filter_ ? filter_->text() : QString());
        QMessageBox::information(this, "导入成功",
            QString("已导入站点（同名站点已覆盖），共 %1 个").arg(store_->sites().size()));
    } else {
        QMessageBox::warning(this, "导入失败", err);
    }
}

void ConnectDialog::loadSite(const SiteEntry& e) {
    proto_->setCurrentIndex(e.isSftp ? 1 : 0);
    name_->setText(e.name);
    host_->setText(e.host);
    port_->setText(QString::number(e.port));
    user_->setText(e.user);
    password_->setEchoMode(QLineEdit::Password);  // saved site: masked by default
    password_->setText(e.password);
    if (revealPwAct_) {
        revealPwAct_->setIcon(makeEyeIcon(false));   // "show password" state
        revealPwAct_->setToolTip("显示密码");
    }
    note_->setPlainText(e.note);
    keyPath_->setText(e.keyPath);
    startDir_->setText(e.startDir);
    pushCmd_->setText(e.pushCmd);
    sftpCmd_->setText(e.sftpCmd);
    group_->setText(e.group);
    // Restore the saved filename encoding (default "auto").
    int encIdx = encoding_->findData(e.encoding.isEmpty() ? "auto" : e.encoding);
    encoding_->setCurrentIndex(encIdx < 0 ? 0 : encIdx);
}

QString ConnectDialog::currentYearMonth() {
    const QDate now = QDate::currentDate();
    return QString::number(now.year() * 100 + now.month());
}

void ConnectDialog::resetFormForNewSite(const QString& group) {
    proto_->setCurrentIndex(0);   // FTP is the default protocol
    onProtocolChanged(0);         // sync port + SSH-only field enablement
    name_->clear();
    host_->clear();
    user_->clear();
    password_->clear();
    password_->setEchoMode(QLineEdit::Normal);   // new site: show the typed password
    if (revealPwAct_) {
        revealPwAct_->setIcon(makeEyeIcon(true));    // "hide password" state
        revealPwAct_->setToolTip("隐藏密码");
    }
    keyPath_->clear();
    startDir_->clear();
    pushCmd_->clear();
    sftpCmd_->clear();
    // Default group: the current year-month as YYYYMM (e.g. 202611,
    // 202701) when no specific group is requested.
    if (group.isEmpty()) {
        group_->setText(currentYearMonth());
    } else {
        group_->setText(group);
    }
    note_->clear();
    encoding_->setCurrentIndex(0);
    editingName_.clear();
    host_->setFocus();
}

void ConnectDialog::duplicateSite(const QString& siteName) {
    if (!store_) return;
    SiteEntry src;
    bool found = false;
    for (const SiteEntry& e : store_->sites()) {
        if (e.name == siteName) { src = e; found = true; break; }
    }
    if (!found) return;

    // Generate a unique "<name> 副本" name (append a counter on collision).
    const QString base = src.name + " 副本";
    QString newName = base;
    int n = 2;
    auto nameExists = [&](const QString& name) {
        for (const SiteEntry& e : store_->sites())
            if (e.name == name) return true;
        return false;
    };
    while (nameExists(newName)) newName = QString("%1 %2").arg(base).arg(n++);

    // Copy the entry (credentials are carried over via Credential Manager).
    const QString pw = store_->readPassword(siteName);
    src.name = newName;
    store_->upsert(src);
    if (!pw.isEmpty()) store_->writePassword(newName, pw);
    store_->save();

    rebuildSiteTree(filter_ ? filter_->text() : QString());
    selectSiteInTree(newName);
}

void ConnectDialog::selectSiteInTree(const QString& name) {
    if (!tree_) return;
    const auto items =
        tree_->findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
        if (item->data(0, kIsGroupRole).toBool()) continue;
        tree_->setCurrentItem(item);
        tree_->scrollToItem(item);
        return;
    }
}

RemoteFsModel::ConnInfo ConnectDialog::connInfo() const {
    RemoteFsModel::ConnInfo ci;
    ci.isSftp = proto_->currentIndex() == 1;
    ci.ftp.host = host_->text().trimmed().toStdString();
    ci.ftp.port = static_cast<std::uint16_t>(port_->text().toInt());
    ci.ftp.user = user_->text().trimmed().toStdString();
    ci.ftp.password = password_->text().toStdString();
    ci.ftp.initialPath =
        startDir_->text().isEmpty() ? "/" : startDir_->text().toStdString();
    ci.ftp.encoding = encoding_->currentData().toString().toStdString();

    ci.ssh.host = ci.ftp.host;
    ci.ssh.port = ci.ftp.port;
    ci.ssh.user = ci.ftp.user;
    ci.ssh.password = ci.ftp.password;
    ci.ssh.sftpCommand = ci.isSftp
        ? sftpCmd_->text().trimmed().toStdString() : std::string();
    if (!keyPath_->text().trimmed().isEmpty()) {
        ci.ssh.privateKeyPem =
            readWholeFileStr(keyPath_->text().trimmed());
    }
    ci.startDir = startDir_->text().isEmpty()
        ? (ci.isSftp ? "." : "/")
        : startDir_->text().toStdString();
    ci.pushCmdTemplate = pushCmd_->text().toStdString();
    // Saved-site name (for tab title / credential recovery). Prefer the
    // form's "名称" field so a hand-entered site shows its name too; fall
    // back to the tree selection (which loadSite also mirrors into name_).
    const QString formName = name_->text().trimmed();
    if (!formName.isEmpty()) {
        ci.siteName = formName.toStdString();
    } else if (tree_ && tree_->currentItem() &&
               !tree_->currentItem()->data(0, kIsGroupRole).toBool()) {
        ci.siteName = tree_->currentItem()->data(0, kSiteNameRole)
                          .toString().toStdString();
    }
    // Strict host-key verification against the persistent known_hosts store.
    ci.knownHostsPath = ftpflow::KnownHosts::defaultPath();
    return ci;
}

}  // namespace ftpflow
