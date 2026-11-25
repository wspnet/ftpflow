// ftpflow -- membership auth client implementation.
#include "auth_client.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace ftpflow {

namespace {
// Distinguish the endpoints in the shared onReplyFinished handler.
enum class Kind : int {
    Login = 1,
    Register = 2,
    SyncUpload = 3,
    SyncDownload = 4
};

// Build the full endpoint URL from a base URL the user may have typed with
// or without a trailing slash.
QString endpoint(const QString& baseUrl, const char* path) {
    QString b = baseUrl.trimmed();
    while (b.endsWith('/')) b.chop(1);
    return b + QString::fromLatin1(path);
}

AuthResult parseResult(const QByteArray& body, bool* ok) {
    AuthResult r;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (ok) *ok = false;
        r.success = false;
        r.message = QStringLiteral("服务器返回了无法解析的响应");
        return r;
    }
    if (ok) *ok = true;
    const QJsonObject o = doc.object();
    r.success = o.value("success").toBool(false);
    r.message = o.value("message").toString();
    r.token = o.value("token").toString();
    r.expiresAt = o.value("expiresAt").toString();
    r.needTwoFactor = o.value("needTwoFactor").toBool(false);
    r.isLocked = o.value("isLocked").toBool(false);
    r.needCaptcha = o.value("needCaptcha").toBool(false);
    r.remainingAttempts = o.value("remainingAttempts").toInt(0);
    r.failedAttempts = o.value("failedAttempts").toInt(0);

    const QJsonObject u = o.value("user").toObject();
    r.userId = u.value("id").toInt(0);
    r.username = u.value("username").toString();
    r.nickname = u.value("nickname").toString();
    r.email = u.value("email").toString();
    r.phone = u.value("phone").toString();
    r.avatar = u.value("avatar").toString();
    r.level = u.value("level").toInt(0);
    r.levelName = u.value("levelName").toString();
    return r;
}

// Extract the `sites` JSON-array string from a site-sync download response
// body. Returns empty when absent/unparseable.
QString parseSitesField(const QByteArray& body) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object().value("sites").toString();
}
}  // namespace

AuthClient::AuthClient(QObject* parent) : QObject(parent) {
    nam_ = std::make_unique<QNetworkAccessManager>(this);
}

AuthClient::~AuthClient() = default;

void AuthClient::login(const QString& baseUrl, const QString& usernameOrEmail,
                       const QString& password, const QString& captchaCode,
                       const QString& captchaToken) {
    QJsonObject o;
    o.insert("username", usernameOrEmail);
    o.insert("password", password);
    if (!captchaCode.isEmpty()) o.insert("captchaCode", captchaCode);
    if (!captchaToken.isEmpty()) o.insert("captchaToken", captchaToken);
    post(endpoint(baseUrl, "/api/auth/login"),
         QJsonDocument(o).toJson(QJsonDocument::Compact),
         static_cast<int>(Kind::Login), {});
}

void AuthClient::registerAccount(const QString& baseUrl, const QString& username,
                                 const QString& email, const QString& password,
                                 const QString& nickname, const QString& inviteCode,
                                 const QString& dealer) {
    QJsonObject o;
    o.insert("username", username);
    o.insert("email", email);
    o.insert("password", password);
    if (!nickname.isEmpty()) o.insert("nickname", nickname);
    if (!inviteCode.isEmpty()) o.insert("inviteCode", inviteCode);
    if (!dealer.isEmpty()) o.insert("dealer", dealer);
    post(endpoint(baseUrl, "/api/auth/register"),
         QJsonDocument(o).toJson(QJsonDocument::Compact),
         static_cast<int>(Kind::Register), {});
}

void AuthClient::post(const QString& url, const QByteArray& body, int kind,
                      const QString& bearer) {
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    // AOT-friendly: accept JSON.
    req.setRawHeader("Accept", "application/json");
    if (!bearer.isEmpty())
        req.setRawHeader("Authorization",
                         ("Bearer " + bearer).toUtf8());
    QNetworkReply* reply = nam_->post(req, body);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, kind] { onReplyFinished(reply, kind); });
}

void AuthClient::get(const QString& url, int kind, const QString& bearer) {
    QNetworkRequest req((QUrl(url)));
    req.setRawHeader("Accept", "application/json");
    if (!bearer.isEmpty())
        req.setRawHeader("Authorization",
                         ("Bearer " + bearer).toUtf8());
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, kind] { onReplyFinished(reply, kind); });
}

void AuthClient::syncSites(const QString& baseUrl, const QString& token,
                           const QString& sitesJson) {
    QJsonObject o;
    o.insert("sites", sitesJson);
    post(endpoint(baseUrl, "/api/account/site-sync"),
         QJsonDocument(o).toJson(QJsonDocument::Compact),
         static_cast<int>(Kind::SyncUpload), token);
}

void AuthClient::fetchSites(const QString& baseUrl, const QString& token) {
    get(endpoint(baseUrl, "/api/account/site-sync"),
        static_cast<int>(Kind::SyncDownload), token);
}

void AuthClient::onReplyFinished(QNetworkReply* reply, int kind) {
    reply->deleteLater();
    const QByteArray body = reply->readAll();
    const int http = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Network-level failure (DNS, refused, timeout, TLS) has no status code.
    if (http == 0) {
        emit requestFailed(reply->errorString());
        return;
    }

    bool parsed = false;
    AuthResult r = parseResult(body, &parsed);

    if (http >= 200 && http < 300) {
        // A 2xx with a parseable AuthResult is the normal path (AotCMS always
        // returns 200 with success:false for business failures).
        if (kind == static_cast<int>(Kind::Login))
            emit loginFinished(r);
        else if (kind == static_cast<int>(Kind::Register))
            emit registerFinished(r);
        else if (kind == static_cast<int>(Kind::SyncUpload)) {
            // Upload response: {success, message, updatedAt}.
            emit syncFinished(r.success, r.message, {});
        } else if (kind == static_cast<int>(Kind::SyncDownload)) {
            // Download response: {success, message, sites, updatedAt}.
            const QString sites = parseSitesField(body);
            emit fetchFinished(r.success, r.message, sites);
        }
        return;
    }

    // Non-2xx: surface the server error. Prefer a parsed business message,
    // else fall back to the HTTP status line.
    if (parsed && !r.message.isEmpty()) {
        emit requestFailed(r.message);
    } else {
        emit requestFailed(QStringLiteral("请求失败（HTTP %1）").arg(http));
    }
}

}  // namespace ftpflow
