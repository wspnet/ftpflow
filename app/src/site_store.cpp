// ftpflow -- saved site store implementation.
#include "site_store.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <wincred.h>

namespace ftpflow {
namespace {

QString credTarget(const QString& siteName) {
    return "ftpflow/site/" + siteName;
}

SiteEntry fromJson(const QJsonObject& o) {
    SiteEntry e;
    e.name = o.value("name").toString();
    e.isSftp = o.value("protocol").toString("sftp") == "sftp";
    e.host = o.value("host").toString();
    e.port = o.value("port").toInt(e.isSftp ? 22 : 21);
    e.user = o.value("user").toString();
    e.keyPath = o.value("keyPath").toString();
    e.startDir = o.value("startDir").toString();
    e.pushCmd = o.value("pushCmd").toString();
    return e;
}

QJsonObject toJson(const SiteEntry& e) {
    QJsonObject o;
    o.insert("name", e.name);
    o.insert("protocol", e.isSftp ? "sftp" : "ftp");
    o.insert("host", e.host);
    o.insert("port", e.port);
    o.insert("user", e.user);
    if (!e.keyPath.isEmpty()) o.insert("keyPath", e.keyPath);
    if (!e.startDir.isEmpty()) o.insert("startDir", e.startDir);
    if (!e.pushCmd.isEmpty()) o.insert("pushCmd", e.pushCmd);
    return o;
}

}  // namespace

QString SiteStore::defaultFilePath() {
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + "/sites.json";
}

bool SiteStore::load(const QString& path) {
    path_ = path.isEmpty() ? defaultFilePath() : path;
    sites_.clear();
    QFile f(path_);
    if (!f.exists()) return true;   // first run: empty store is fine
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return false;
    for (const QJsonValue& v : doc.array()) {
        if (v.isObject()) sites_.push_back(fromJson(v.toObject()));
    }
    return true;
}

bool SiteStore::save(const QString& path) const {
    const QString target = path.isEmpty()
        ? (path_.isEmpty() ? defaultFilePath() : path_)
        : path;
    QDir().mkpath(QFileInfo(target).absolutePath());
    QJsonArray arr;
    for (const SiteEntry& e : sites_) arr.append(toJson(e));
    QFile f(target);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}

void SiteStore::upsert(const SiteEntry& site) {
    for (SiteEntry& e : sites_) {
        if (e.name == site.name) {
            e = site;
            return;
        }
    }
    sites_.push_back(site);
}

void SiteStore::remove(const QString& name) {
    for (int i = 0; i < sites_.size(); ++i) {
        if (sites_[i].name == name) {
            sites_.removeAt(i);
            return;
        }
    }
}

QString SiteStore::readPassword(const QString& siteName) {
    PCREDENTIALW cred = nullptr;
    const QString target = credTarget(siteName);
    if (!CredReadW((const wchar_t*)target.utf16(), CRED_TYPE_GENERIC, 0,
                   &cred)) {
        return {};
    }
    QString out;
    if (cred->CredentialBlob && cred->CredentialBlobSize >= sizeof(wchar_t)) {
        const int chars = cred->CredentialBlobSize / sizeof(wchar_t);
        out = QString::fromWCharArray(
            (const wchar_t*)cred->CredentialBlob, chars);
    }
    CredFree(cred);
    return out;
}

bool SiteStore::writePassword(const QString& siteName,
                              const QString& password) {
    const QString target = credTarget(siteName);
    const wchar_t* blob = (const wchar_t*)password.utf16();
    const DWORD blobBytes =
        (DWORD)password.size() * sizeof(wchar_t);   // utf16() is NUL-terminated
    CREDENTIALW c{};
    c.Flags = 0;
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = (wchar_t*)target.utf16();
    c.Comment = (wchar_t*)QString("ftpflow site credential").utf16();
    c.CredentialBlob = (LPBYTE)blob;
    c.CredentialBlobSize = blobBytes;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;   // roaming profile
    return CredWriteW(&c, 0) != 0;
}

void SiteStore::erasePassword(const QString& siteName) {
    CredDeleteW((const wchar_t*)credTarget(siteName).utf16(),
                CRED_TYPE_GENERIC, 0);
}

SiteEntry SiteStore::withPassword(const SiteEntry& e) const {
    SiteEntry out = e;
    out.password = readPassword(e.name);
    return out;
}

}  // namespace ftpflow
