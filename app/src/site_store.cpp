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
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <string>

#include <wincred.h>

namespace ftpflow {
namespace {

constexpr const char* kGuestCredPrefix = "ftpflow/site/";

// A member scope is stored as a separate file suffix and a separate
// credential namespace. "@" is deliberately used so a guest enumeration
// ("ftpflow/site/*") can tell member targets apart from its own.
QString scopeFileSuffix(const QString& scope) {
    return (scope.isEmpty() || scope == QStringLiteral("guest"))
               ? QString()
               : QStringLiteral("_") + scope;
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
    e.sftpCmd = o.value("sftpCmd").toString();
    e.group = o.value("group").toString();
    e.encoding = o.value("encoding").toString("auto");
    e.note = o.value("note").toString();
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
    if (!e.sftpCmd.isEmpty()) o.insert("sftpCmd", e.sftpCmd);
    if (!e.group.isEmpty()) o.insert("group", e.group);
    if (!e.encoding.isEmpty() && e.encoding != "auto")
        o.insert("encoding", e.encoding);
    if (!e.note.isEmpty()) o.insert("note", e.note);
    return o;
}

// Writes `doc` to `target` atomically: full write to a temp file first, then
// a single MoveFileEx replace. A crash mid-save can therefore never truncate
// the live file (the old file survives intact until the rename lands).
bool writeJsonAtomic(const QString& target, const QJsonDocument& doc) {
    QDir().mkpath(QFileInfo(target).absolutePath());
    const QString tmp = target + ".tmp";
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const QByteArray bytes = doc.toJson(QJsonDocument::Indented);
        const qint64 written = f.write(bytes);
        f.flush();
        f.close();
        if (written != bytes.size()) {
            QFile::remove(tmp);
            return false;
        }
    }
    const std::wstring wtmp = tmp.toStdWString();
    const std::wstring wdst = target.toStdWString();
    if (::MoveFileExW(wtmp.c_str(), wdst.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    // Fallback (e.g. exotic filesystems): best-effort replace via Qt.
    QFile::remove(target);
    return QFile::rename(tmp, target);
}

}  // namespace

QString SiteStore::defaultFilePath() {
    return filePathForScope(QString());
}

QString SiteStore::foldersFilePath() {
    return foldersFilePathForScope(QString());
}

QString SiteStore::filePathForScope(const QString& scope) {
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + "/sites" + scopeFileSuffix(scope) + ".json";
}

QString SiteStore::foldersFilePathForScope(const QString& scope) {
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + "/folders" + scopeFileSuffix(scope) + ".json";
}

void SiteStore::setScope(const QString& scope) {
    scope_ = (scope == QStringLiteral("guest")) ? QString() : scope;
    path_ = filePathForScope(scope_);
    foldersPath_ = foldersFilePathForScope(scope_);
    sites_.clear();
    folders_.clear();
    batchDepth_ = 0;
}

void SiteStore::notifyChanged() {
    if (batchDepth_ == 0) emit sitesChanged();
}

void SiteStore::endBatch() {
    if (batchDepth_ == 0) return;
    if (--batchDepth_ == 0) emit sitesChanged();
}

bool SiteStore::load(const QString& path) {
    if (!path.isEmpty()) path_ = path;
    if (path_.isEmpty()) path_ = filePathForScope(scope_);
    if (foldersPath_.isEmpty()) foldersPath_ = foldersFilePathForScope(scope_);

    QFile f(path_);
    if (!f.exists()) {
        // First run in this scope: empty store is fine.
        sites_.clear();
    } else if (!f.open(QIODevice::ReadOnly)) {
        // Unreadable file: keep whatever is in memory. Clearing here would
        // let a later save() overwrite the (possibly perfectly fine) file
        // with an empty list and lose every site.
        return false;
    } else {
        const QByteArray raw = f.readAll();
        f.close();
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
            // Corrupt file: stash a copy so evidence survives even if a
            // later save() writes a fresh list over the original path.
            QFile::copy(path_, path_ + ".corrupt");
            return false;
        }
        // Parse into a scratch list; only commit on full success.
        QList<SiteEntry> parsed;
        for (const QJsonValue& v : doc.array()) {
            if (v.isObject()) parsed.push_back(fromJson(v.toObject()));
        }
        sites_ = std::move(parsed);
    }

    // Load standalone folders (separate file, may not exist yet). Failure
    // is non-fatal -- folders only organize the tree, no credentials.
    folders_.clear();
    QFile ff(foldersPath_);
    if (ff.exists() && ff.open(QIODevice::ReadOnly)) {
        const QJsonDocument fd = QJsonDocument::fromJson(ff.readAll());
        if (fd.isArray()) {
            for (const QJsonValue& v : fd.array()) {
                if (v.isString() && !v.toString().trimmed().isEmpty())
                    folders_.append(v.toString());
            }
        }
        ff.close();
    }
    return true;
}

bool SiteStore::save(const QString& path) const {
    const QString target = path.isEmpty()
        ? (path_.isEmpty() ? defaultFilePath() : path_)
        : path;
    QJsonArray arr;
    for (const SiteEntry& e : sites_) arr.append(toJson(e));
    if (!writeJsonAtomic(target, QJsonDocument(arr))) return false;

    // Persist standalone folders in the SAME scope as the sites.
    QJsonArray farr;
    for (const QString& g : folders_) farr.append(g);
    return writeJsonAtomic(
        foldersPath_.isEmpty() ? foldersFilePath() : foldersPath_,
        QJsonDocument(farr));
}

void SiteStore::clearAll() {
    // Delete scoped credentials first, while the site names are still in
    // memory (eraseAllPasswords falls back to enumerating the scope anyway).
    eraseAllPasswords();
    sites_.clear();
    folders_.clear();
    if (!path_.isEmpty()) QFile::remove(path_);
    if (!foldersPath_.isEmpty()) QFile::remove(foldersPath_);
}

void SiteStore::upsert(const SiteEntry& site) {
    for (SiteEntry& e : sites_) {
        if (e.name == site.name) {
            e = site;
            notifyChanged();
            return;
        }
    }
    sites_.push_back(site);
    notifyChanged();
}

void SiteStore::remove(const QString& name) {
    for (int i = 0; i < sites_.size(); ++i) {
        if (sites_[i].name == name) {
            sites_.removeAt(i);
            notifyChanged();
            return;
        }
    }
}

QList<QString> SiteStore::groups() const {
    QSet<QString> seen;
    for (const SiteEntry& e : sites_) {
        if (!e.group.isEmpty()) seen.insert(e.group);
    }
    // Standalone folders are groups too (even when empty).
    for (const QString& g : folders_) {
        if (!g.isEmpty()) seen.insert(g);
    }
    QList<QString> out = seen.values();
    // Descending order: newest year-month groups (e.g. 202701) sort first,
    // so recently added sites stay near the top of the tree.
    std::sort(out.begin(), out.end(),
              [](const QString& a, const QString& b) {
                  return a.localeAwareCompare(b) > 0;
              });
    return out;
}

void SiteStore::addFolder(const QString& name) {
    const QString n = name.trimmed();
    if (n.isEmpty() || folders_.contains(n)) return;
    folders_.append(n);
    std::sort(folders_.begin(), folders_.end(),
              [](const QString& a, const QString& b) {
                  return a.localeAwareCompare(b) < 0;
              });
    notifyChanged();
}

void SiteStore::removeFolder(const QString& name) {
    folders_.removeAll(name);
    // Sites in that folder fall back to ungrouped.
    for (SiteEntry& e : sites_) {
        if (e.group == name) e.group.clear();
    }
    notifyChanged();
}

void SiteStore::renameFolder(const QString& from, const QString& to) {
    const QString t = to.trimmed();
    if (t.isEmpty() || t == from) return;
    for (QString& g : folders_) {
        if (g == from) g = t;
    }
    std::sort(folders_.begin(), folders_.end(),
              [](const QString& a, const QString& b) {
                  return a.localeAwareCompare(b) < 0;
              });
    for (SiteEntry& e : sites_) {
        if (e.group == from) e.group = t;
    }
    notifyChanged();
}

QList<SiteEntry> SiteStore::sitesInGroup(const QString& group) const {
    QList<SiteEntry> out;
    for (const SiteEntry& e : sites_) {
        if (e.group == group) out.push_back(e);
    }
    std::sort(out.begin(), out.end(),
              [](const SiteEntry& a, const SiteEntry& b) {
                  return a.name.localeAwareCompare(b.name) < 0;
              });
    return out;
}

bool SiteStore::exportTo(const QString& path, QString* error) const {
    QJsonArray arr;
    for (const SiteEntry& e : sites_) arr.append(toJson(e));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QString("无法写入 %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}

bool SiteStore::importFrom(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QString("无法读取 %1").arg(path);
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) {
        if (error) *error = QString("%1 不是有效的站点文件").arg(path);
        return false;
    }
    int added = 0;
    beginBatch();
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject()) continue;
        const SiteEntry e = fromJson(v.toObject());
        if (e.name.isEmpty() || e.host.isEmpty()) continue;
        upsert(e);   // merge by name (replace existing)
        ++added;
    }
    endBatch();
    if (error && added == 0) *error = "文件中没有可导入的站点";
    return added > 0;
}

QString SiteStore::credPrefix() const {
    if (scope_.isEmpty())
        return QString::fromLatin1(kGuestCredPrefix);
    return QString::fromLatin1(kGuestCredPrefix) + "@" + scope_ + "/";
}

QString SiteStore::credTarget(const QString& siteName) const {
    return credPrefix() + siteName;
}

bool SiteStore::targetInScope(const QString& target) const {
    if (scope_.isEmpty()) {
        // Guest credentials are exactly "ftpflow/site/<name>" with no
        // further slash. Member targets ("ftpflow/site/@m_.../<name>")
        // must NOT be touched by a guest clearAll().
        if (!target.startsWith(QString::fromLatin1(kGuestCredPrefix)))
            return false;
        const QString rest = target.mid(
            QString::fromLatin1(kGuestCredPrefix).size());
        return !rest.isEmpty() && !rest.contains('/');
    }
    return target.startsWith(credPrefix());
}

QString SiteStore::readPassword(const QString& siteName) const {
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
                              const QString& password) const {
    // NOTE: every wchar_t* handed to CredWriteW must stay alive until the
    // call returns -- utf16() on a temporary QString dangles the moment the
    // statement ends (this previously gave CredWriteW a freed Comment).
    const QString target = credTarget(siteName);
    const QString comment = QStringLiteral("ftpflow site credential");
    const wchar_t* blob = (const wchar_t*)password.utf16();
    const DWORD blobBytes =
        (DWORD)password.size() * sizeof(wchar_t);   // utf16() is NUL-terminated
    CREDENTIALW c{};
    c.Flags = 0;
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = (wchar_t*)target.utf16();
    c.Comment = (wchar_t*)comment.utf16();
    c.CredentialBlob = (LPBYTE)blob;
    c.CredentialBlobSize = blobBytes;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;   // roaming profile
    return CredWriteW(&c, 0) != 0;
}

void SiteStore::erasePassword(const QString& siteName) const {
    const QString target = credTarget(siteName);
    CredDeleteW((const wchar_t*)target.utf16(), CRED_TYPE_GENERIC, 0);
}

void SiteStore::eraseAllPasswords() const {
    // First pass: every site currently known in this scope.
    for (const SiteEntry& e : sites_) erasePassword(e.name);

    // Second pass: enumerate the whole "ftpflow/site/*" namespace so even
    // orphaned credentials (site entry removed but blob left behind) are
    // wiped. The filter is a plain prefix wildcard; targetInScope() then
    // makes sure we only delete blobs belonging to THIS scope.
    const QString filter =
        QString::fromLatin1(kGuestCredPrefix) + "*";
    DWORD count = 0;
    PCREDENTIALW* list = nullptr;
    if (CredEnumerateW((const wchar_t*)filter.utf16(), 0, &count, &list)) {
        for (DWORD i = 0; i < count; ++i) {
            if (!list[i] || !list[i]->TargetName) continue;
            const QString t = QString::fromWCharArray(list[i]->TargetName);
            if (targetInScope(t))
                CredDeleteW(list[i]->TargetName, CRED_TYPE_GENERIC, 0);
        }
        CredFree(list);
    }
}

SiteEntry SiteStore::withPassword(const SiteEntry& e) const {
    SiteEntry out = e;
    out.password = readPassword(e.name);
    return out;
}

}  // namespace ftpflow
