// ftpflow -- startup identity verification dialog implementation.
#include "session_unlock_dialog.hpp"

#include "dialog_title_bar.hpp"

#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace ftpflow {

SessionUnlockDialog::SessionUnlockDialog(
    const QString& displayName, const QString& accountName,
    std::function<bool(const QString&)> verify, QWidget* parent)
    : QDialog(parent), verify_(std::move(verify)) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setWindowTitle("验证身份");
    resize(420, 286);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* titleBar = new DialogTitleBar(this);
    titleBar->setTitle("验证身份");
    titleBar->attachWindow(this);
    lay->addWidget(titleBar);

    auto* body = new QWidget(this);
    auto* bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(20, 16, 20, 16);
    bodyLay->setSpacing(10);
    lay->addWidget(body, 1);

    auto* heading = new QLabel("会员站点已锁定", body);
    heading->setObjectName("paneTitle");
    QFont hf = heading->font();
    hf.setPointSize(hf.pointSize() + 2);
    hf.setBold(true);
    heading->setFont(hf);
    bodyLay->addWidget(heading);

    const QString who = displayName.trimmed().isEmpty()
                            ? accountName
                            : displayName;
    auto* hint = new QLabel(
        QString("上次登录：%1\n请输入该账号的登录密码以恢复会员站点列表。\n"
                "未验证前软件只显示游客站点列表（会员数据不会加载）。")
            .arg(who),
        body);
    hint->setWordWrap(true);
    hint->setObjectName("paneTitle");
    bodyLay->addWidget(hint);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 4, 0, 4);
    form->setSpacing(8);
    pass_ = new QLineEdit(body);
    pass_->setEchoMode(QLineEdit::Password);
    pass_->setPlaceholderText("会员登录密码");
    form->addRow("密码", pass_);
    bodyLay->addLayout(form);

    status_ = new QLabel(" ", body);
    status_->setObjectName("paneTitle");
    status_->setWordWrap(true);
    bodyLay->addWidget(status_);

    bodyLay->addStretch(1);

    auto* verifyBtn = new QPushButton("验证并进入", body);
    verifyBtn->setProperty("accent", true);
    verifyBtn->setMinimumHeight(34);
    verifyBtn->setDefault(true);
    bodyLay->addWidget(verifyBtn);

    auto* altRow = new QHBoxLayout;
    altRow->setSpacing(8);
    auto* guestBtn = new QPushButton("以游客身份使用", body);
    auto* switchBtn = new QPushButton("更换账号登录", body);
    altRow->addWidget(guestBtn);
    altRow->addWidget(switchBtn);
    bodyLay->addLayout(altRow);

    connect(verifyBtn, &QPushButton::clicked, this,
            &SessionUnlockDialog::onVerify);
    connect(pass_, &QLineEdit::returnPressed, this,
            &SessionUnlockDialog::onVerify);
    connect(guestBtn, &QPushButton::clicked, this, [this] {
        outcome_ = Outcome::Guest;
        accept();
    });
    connect(switchBtn, &QPushButton::clicked, this, [this] {
        outcome_ = Outcome::SwitchAccount;
        accept();
    });

    pass_->setFocus();
}

QString SessionUnlockDialog::password() const {
    return pass_ ? pass_->text() : QString();
}

void SessionUnlockDialog::onVerify() {
    const QString pw = pass_->text();
    if (pw.isEmpty()) {
        status_->setText("请输入密码");
        return;
    }
    if (!verify_ || !verify_(pw)) {
        status_->setText("密码不正确，请重试；也可切换为游客或更换账号登录");
        pass_->selectAll();
        pass_->setFocus();
        return;
    }
    outcome_ = Outcome::Verified;
    accept();
}

}  // namespace ftpflow
