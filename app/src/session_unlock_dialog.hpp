// ftpflow -- startup identity verification dialog.
//
// Shown on launch when a member session was saved AND the user enabled
// "每次启动要求验证密码". Its whole purpose is to keep the member's site
// list locked until the password is confirmed: the main window deliberately
// loads the (empty) guest list before showing this dialog, so closing it or
// choosing "guest" can never expose member FTP accounts/passwords.
#pragma once

#include <QDialog>

#include <QString>

#include <functional>

class QLineEdit;
class QLabel;

namespace ftpflow {

class SessionUnlockDialog : public QDialog {
    Q_OBJECT
public:
    enum class Outcome {
        Verified,       // password accepted -> restore the member session
        Guest,          // drop the saved session, use the guest list
        SwitchAccount,  // open the login/register dialog for another account
        Canceled        // dialog closed without choosing (treated as Guest)
    };

    // `verify` returns true when `password` matches the locally stored
    // salted hash. It runs offline so the user is never locked out just
    // because the membership server is unreachable at startup.
    SessionUnlockDialog(const QString& displayName,
                        const QString& accountName,
                        std::function<bool(const QString&)> verify,
                        QWidget* parent = nullptr);

    Outcome outcome() const { return outcome_; }
    QString password() const;

private slots:
    void onVerify();

private:
    QLineEdit* pass_ = nullptr;
    QLabel* status_ = nullptr;
    std::function<bool(const QString&)> verify_;
    Outcome outcome_ = Outcome::Canceled;
};

}  // namespace ftpflow
