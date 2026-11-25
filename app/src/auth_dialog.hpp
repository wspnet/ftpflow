// ftpflow -- membership login / register dialog.
//
// A single frameless dialog with two tabs ("登录" / "注册") and an editable
// API base URL at the bottom. The base URL is persisted in QSettings so the
// user only sets it once. On success the dialog accepts and exposes the
// AuthResult so the main window can store the token and refresh its account
// entry.

#pragma once

#include <QDialog>

#include "auth_client.hpp"

class QLineEdit;
class QTabWidget;
class QPushButton;
class QLabel;

namespace ftpflow {

class AuthDialog : public QDialog {
    Q_OBJECT
public:
    explicit AuthDialog(QWidget* parent = nullptr);

    // The result of a successful login/register (accept() already called).
    AuthResult result() const { return result_; }
    // The API base URL the user used (persisted; empty if unset).
    QString baseUrl() const;
    // The password the user just typed (login or register). Exposed so the
    // main window can build an offline startup-verification hash; read only
    // while the dialog is still alive (right after exec() returns).
    QString submittedPassword() const { return submittedPassword_; }

private slots:
    void onSubmitLogin();
    void onSubmitRegister();
    void onLoginFinished(const ftpflow::AuthResult& r);
    void onRegisterFinished(const ftpflow::AuthResult& r);
    void onRequestFailed(const QString& what);

private:
    QWidget* buildLoginPage();
    QWidget* buildRegisterPage();
    void setBusy(bool busy);

    AuthClient* client_ = nullptr;

    QTabWidget* tabs_ = nullptr;

    // Login page widgets.
    QLineEdit* loginUser_ = nullptr;
    QLineEdit* loginPass_ = nullptr;
    QPushButton* loginBtn_ = nullptr;

    // Register page widgets.
    QLineEdit* regUser_ = nullptr;
    QLineEdit* regEmail_ = nullptr;
    QLineEdit* regPass_ = nullptr;
    QLineEdit* regPass2_ = nullptr;
    QPushButton* regBtn_ = nullptr;

    // Shared footer.
    QLineEdit* baseUrlEdit_ = nullptr;
    QLabel* status_ = nullptr;

    AuthResult result_;
    QString submittedPassword_;
};

}  // namespace ftpflow
