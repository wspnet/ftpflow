// ftpflow -- membership login / register dialog implementation.
#include "auth_dialog.hpp"

#include "dialog_title_bar.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>

namespace ftpflow {

namespace {
constexpr const char* kBaseUrlKey = "member/apiBaseUrl";
constexpr const char* kDefaultBaseUrl = "http://localhost:18823";
}  // namespace

AuthDialog::AuthDialog(QWidget* parent) : QDialog(parent) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setWindowTitle("登录 / 注册");
    resize(400, 460);

    client_ = new AuthClient(this);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* titleBar = new DialogTitleBar(this);
    titleBar->setTitle("账号");
    titleBar->attachWindow(this);
    lay->addWidget(titleBar);

    auto* body = new QWidget(this);
    auto* bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(16, 12, 16, 14);
    bodyLay->setSpacing(12);
    lay->addWidget(body, 1);

    tabs_ = new QTabWidget(body);
    tabs_->addTab(buildLoginPage(), "登录");
    tabs_->addTab(buildRegisterPage(), "注册");
    bodyLay->addWidget(tabs_, 1);

    // Footer: API base URL (editable, persisted) + status line.
    auto* footer = new QWidget(body);
    auto* footLay = new QVBoxLayout(footer);
    footLay->setContentsMargins(0, 0, 0, 0);
    footLay->setSpacing(6);

    auto* urlRow = new QHBoxLayout;
    auto* urlLbl = new QLabel("接口地址", footer);
    urlLbl->setObjectName("paneTitle");
    baseUrlEdit_ = new QLineEdit(footer);
    baseUrlEdit_->setPlaceholderText(QString::fromLatin1(kDefaultBaseUrl));
    {
        QSettings s;
        const QString saved = s.value(QString::fromLatin1(kBaseUrlKey)).toString();
        baseUrlEdit_->setText(saved.isEmpty()
                                  ? QString::fromLatin1(kDefaultBaseUrl)
                                  : saved);
    }
    urlRow->addWidget(urlLbl);
    urlRow->addWidget(baseUrlEdit_, 1);
    footLay->addLayout(urlRow);

    status_ = new QLabel(" ", footer);
    status_->setObjectName("paneTitle");
    status_->setWordWrap(true);
    footLay->addWidget(status_);

    bodyLay->addWidget(footer);

    connect(client_, &AuthClient::loginFinished, this,
            &AuthDialog::onLoginFinished);
    connect(client_, &AuthClient::registerFinished, this,
            &AuthDialog::onRegisterFinished);
    connect(client_, &AuthClient::requestFailed, this,
            &AuthDialog::onRequestFailed);

    loginUser_->setFocus();
}

QString AuthDialog::baseUrl() const {
    QString b = baseUrlEdit_->text().trimmed();
    if (b.isEmpty()) b = QString::fromLatin1(kDefaultBaseUrl);
    return b;
}

void AuthDialog::setBusy(bool busy) {
    loginBtn_->setEnabled(!busy);
    regBtn_->setEnabled(!busy);
    baseUrlEdit_->setEnabled(!busy);
    tabs_->setEnabled(!busy);
}

QWidget* AuthDialog::buildLoginPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(4, 16, 4, 4);
    form->setSpacing(10);

    loginUser_ = new QLineEdit(page);
    loginUser_->setPlaceholderText("用户名或邮箱");
    loginPass_ = new QLineEdit(page);
    loginPass_->setEchoMode(QLineEdit::Password);
    loginPass_->setPlaceholderText("密码");

    loginBtn_ = new QPushButton("登 录", page);
    loginBtn_->setProperty("accent", true);
    loginBtn_->setMinimumHeight(36);

    form->addRow("账号", loginUser_);
    form->addRow("密码", loginPass_);
    form->addRow(loginBtn_);

    connect(loginBtn_, &QPushButton::clicked, this, &AuthDialog::onSubmitLogin);
    connect(loginPass_, &QLineEdit::returnPressed, this,
            &AuthDialog::onSubmitLogin);
    return page;
}

QWidget* AuthDialog::buildRegisterPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    form->setContentsMargins(4, 16, 4, 4);
    form->setSpacing(10);

    regUser_ = new QLineEdit(page);
    regUser_->setPlaceholderText("用户名");
    regEmail_ = new QLineEdit(page);
    regEmail_->setPlaceholderText("邮箱");
    regPass_ = new QLineEdit(page);
    regPass_->setEchoMode(QLineEdit::Password);
    regPass_->setPlaceholderText("密码（至少6位）");
    regPass2_ = new QLineEdit(page);
    regPass2_->setEchoMode(QLineEdit::Password);
    regPass2_->setPlaceholderText("确认密码");

    regBtn_ = new QPushButton("注 册", page);
    regBtn_->setProperty("accent", true);
    regBtn_->setMinimumHeight(36);

    form->addRow("用户名", regUser_);
    form->addRow("邮箱", regEmail_);
    form->addRow("密码", regPass_);
    form->addRow("确认密码", regPass2_);
    form->addRow(regBtn_);

    connect(regBtn_, &QPushButton::clicked, this,
            &AuthDialog::onSubmitRegister);
    connect(regPass2_, &QLineEdit::returnPressed, this,
            &AuthDialog::onSubmitRegister);
    return page;
}

void AuthDialog::onSubmitLogin() {
    const QString u = loginUser_->text().trimmed();
    const QString p = loginPass_->text();
    if (u.isEmpty() || p.isEmpty()) {
        status_->setText("请输入账号和密码");
        return;
    }
    // Persist the base URL on every submit so it survives even a failed login.
    QSettings s;
    s.setValue(QString::fromLatin1(kBaseUrlKey), baseUrl());
    setBusy(true);
    submittedPassword_ = p;
    status_->setText("正在登录...");
    client_->login(baseUrl(), u, p);
}

void AuthDialog::onSubmitRegister() {
    const QString u = regUser_->text().trimmed();
    const QString e = regEmail_->text().trimmed();
    const QString p = regPass_->text();
    const QString p2 = regPass2_->text();
    if (u.isEmpty() || e.isEmpty() || p.isEmpty()) {
        status_->setText("请填写用户名、邮箱和密码");
        return;
    }
    if (p.length() < 6) {
        status_->setText("密码至少6位");
        return;
    }
    if (p != p2) {
        status_->setText("两次输入的密码不一致");
        return;
    }
    QSettings s;
    s.setValue(QString::fromLatin1(kBaseUrlKey), baseUrl());
    setBusy(true);
    submittedPassword_ = p;
    status_->setText("正在注册...");
    client_->registerAccount(baseUrl(), u, e, p, u /* nickname = username */);
}

void AuthDialog::onLoginFinished(const AuthResult& r) {
    if (r.success && !r.token.isEmpty()) {
        result_ = r;
        accept();
        return;
    }
    setBusy(false);
    // Surface business failures (wrong password, locked, need 2FA, captcha).
    QString msg = r.message;
    if (msg.isEmpty()) msg = "登录失败";
    if (r.needCaptcha) msg += "（需要验证码）";
    if (r.isLocked) msg += "（账户已锁定）";
    status_->setText(msg);
}

void AuthDialog::onRegisterFinished(const AuthResult& r) {
    if (r.success && !r.token.isEmpty()) {
        result_ = r;
        accept();
        return;
    }
    setBusy(false);
    status_->setText(r.message.isEmpty() ? "注册失败" : r.message);
}

void AuthDialog::onRequestFailed(const QString& what) {
    setBusy(false);
    status_->setText(what);
}

}  // namespace ftpflow
