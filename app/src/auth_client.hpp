// ftpflow -- membership auth client.
//
// Thin async wrapper over the AotCMS member API. The desktop app has no
// built-in notion of a session; it delegates to the same JSON endpoints the
// WeChat mini program uses:
//
//   POST /api/auth/login    body: {username, password, captchaCode?, captchaToken?}
//   POST /api/auth/register body: {username, email, password, nickname?, inviteCode?, dealer?}
//
// Both return an AuthResultDto-shaped JSON payload (see AotCMS
// Models/AuthDtos.cs): { success, message, token, expiresAt, user{...},
// needTwoFactor, isLocked, needCaptcha, remainingAttempts, failedAttempts }.
//
// AuthClient owns a QNetworkAccessManager and fires one request at a time;
// the caller keeps the AuthClient alive across the whole dialog lifetime so
// the reply slot outlives the request.

#pragma once

#include <QObject>

#include <QString>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace ftpflow {

// Result of a login/register call, normalized from AuthResultDto.
struct AuthResult {
    bool success = false;
    QString message;
    QString token;
    QString expiresAt;   // ISO-8601 string, empty when absent
    // user fields
    int userId = 0;
    QString username;
    QString nickname;
    QString email;
    QString phone;
    QString avatar;
    int level = 0;
    QString levelName;
    // security flags echoed back to the UI
    bool needTwoFactor = false;
    bool isLocked = false;
    bool needCaptcha = false;
    int remainingAttempts = 0;
    int failedAttempts = 0;
};

class AuthClient : public QObject {
    Q_OBJECT
public:
    explicit AuthClient(QObject* parent = nullptr);
    ~AuthClient() override;

    // Fire an async login request. Exactly one of loginFinished /
    // requestFailed fires per call.
    void login(const QString& baseUrl, const QString& usernameOrEmail,
               const QString& password, const QString& captchaCode = {},
               const QString& captchaToken = {});

    // Fire an async register request.
    void registerAccount(const QString& baseUrl, const QString& username,
                         const QString& email, const QString& password,
                         const QString& nickname = {},
                         const QString& inviteCode = {},
                         const QString& dealer = {});

    // Fire an async site-sync upload: pushes the local site list (JSON array
    // string, passwords included) to the member's cloud slot.
    void syncSites(const QString& baseUrl, const QString& token,
                   const QString& sitesJson);

    // Fire an async site-sync download: fetches the member's cloud site list.
    void fetchSites(const QString& baseUrl, const QString& token);

signals:
    void loginFinished(const ftpflow::AuthResult& result);
    void registerFinished(const ftpflow::AuthResult& result);
    // Site-sync result: ok=false means business failure or transport error.
    // `sitesJson` is the decrypted JSON array string (download) or empty.
    void syncFinished(bool ok, const QString& message, const QString& sitesJson);
    void fetchFinished(bool ok, const QString& message, const QString& sitesJson);
    // A transport/network error (timeout, DNS, connection refused, non-2xx
    // with unparseable body). `what` is a user-facing message.
    void requestFailed(const QString& what);

private:
    void post(const QString& url, const QByteArray& body, int kind,
              const QString& bearer = {});
    void get(const QString& url, int kind, const QString& bearer = {});
    void onReplyFinished(QNetworkReply* reply, int kind);

    std::unique_ptr<QNetworkAccessManager> nam_;
};

}  // namespace ftpflow
