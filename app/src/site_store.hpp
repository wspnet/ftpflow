// ftpflow -- saved connection sites (JSON) + passwords in the Windows
// Credential Manager. The JSON never contains secrets: passwords live in
// Credential Manager blobs (per-user, DPAPI protected at rest by the OS).
//
// IDENTITY SCOPES
// ---------------
// Every site list belongs to exactly one identity "scope":
//   * "" / "guest"  -> the anonymous local user. Uses the legacy sites.json /
//                      folders.json and the legacy "ftpflow/site/<name>"
//                      credential names, so existing installs keep working.
//   * "m_..."       -> a logged-in member. Uses its own sites_m_xxx.json /
//                      folders_m_xxx.json and a private credential namespace
//                      "ftpflow/site/@m_xxx/<name>".
// Scopes never share sites, folders or credentials: a guest can therefore
// never see a member's FTP accounts/passwords, and two members never see
// each other's data.
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace ftpflow {

struct SiteEntry {
    QString name;
    bool isSftp = true;
    QString host;
    int port = 22;          // 21 when !isSftp
    QString user;
    QString keyPath;        // PEM private key file (path only)
    QString startDir;
    QString pushCmd;        // remote decompressor template (optional)
    QString group;          // grouping label ("" = ungrouped), XTerminal-style
    QString encoding;       // filename encoding: "auto"/"utf-8"/"gbk" (FTP)
    QString note;           // free-form multiline remark shown in the UI
    // Transient: filled from Credential Manager, never serialized.
    QString password;
};

class SiteStore : public QObject {
    Q_OBJECT
public:
    explicit SiteStore(QObject* parent = nullptr) : QObject(parent) {}

    // ---- Identity scope --------------------------------------------------
    QString scope() const { return scope_; }
    // Switch identity WITHOUT loading: clears in-memory sites/folders and
    // repoints the file + credential paths. The caller MUST call load()
    // afterwards (or clearAll() when the old scope should be wiped).
    void setScope(const QString& scope);
    // Erase everything in the CURRENT scope (memory + both files + every
    // credential in that scope), leaving an empty list. Used after guest
    // sites have been migrated into a member account: the guest list must
    // become empty and must not keep copies of FTP passwords.
    void clearAll();

    // Legacy / guest paths (%APPDATA%/ftpflow/sites.json, folders.json).
    static QString defaultFilePath();
    static QString foldersFilePath();
    // Scope-aware paths. Guest scope maps to the legacy paths above.
    static QString filePathForScope(const QString& scope);
    static QString foldersFilePathForScope(const QString& scope);

    bool load(const QString& path = QString());
    bool save(const QString& path = QString()) const;

    const QList<SiteEntry>& sites() const { return sites_; }
    // Insert or replace by name.
    void upsert(const SiteEntry& site);
    void remove(const QString& name);

    // Grouping (XTerminal-style multi-level organization).
    QList<QString> groups() const;              // deduped, "" excluded, newest first
    QList<SiteEntry> sitesInGroup(const QString& group) const;  // "" = ungrouped

    // Standalone folders (persisted separately; can exist with no sites).
    const QStringList& folders() const { return folders_; }
    void addFolder(const QString& name);        // no-op if already present
    void removeFolder(const QString& name);     // sites fall back to ungrouped
    void renameFolder(const QString& from, const QString& to);

    // Bulk-merge guard: sitesChanged is emitted once at endBatch() instead of
    // once per upsert, so merging a downloaded cloud list doesn't fire a
    // debounced push for every single site.
    void beginBatch() { ++batchDepth_; }
    void endBatch();

    // Import / export the full site list (JSON). Passwords are NOT included
    // (they live in the OS Credential Manager); export only emits metadata.
    bool exportTo(const QString& path, QString* error) const;
    // Merge import: existing entries with the same name are replaced.
    bool importFrom(const QString& path, QString* error);

    // ---- Windows Credential Manager (scoped) -----------------------------
    // All three are scoped to this store's identity. They are instance
    // methods (not static) precisely so a member's credentials can never be
    // reached from a guest-scoped store.
    QString readPassword(const QString& siteName) const;
    bool writePassword(const QString& siteName,
                       const QString& password) const;
    void erasePassword(const QString& siteName) const;
    // Erase every credential belonging to the current scope, including ones
    // whose site entry is no longer in memory.
    void eraseAllPasswords() const;

    // Entry copy with the password filled from Credential Manager ("" when
    // none is stored).
    SiteEntry withPassword(const SiteEntry& e) const;

signals:
    // Emitted after any mutation to the site list or folders (add/edit/delete/
    // import/rename). Drives automatic cloud sync after local edits.
    void sitesChanged();

private:
    QString credPrefix() const;
    QString credTarget(const QString& siteName) const;
    bool targetInScope(const QString& target) const;
    void notifyChanged();

    QList<SiteEntry> sites_;
    QStringList folders_;
    QString path_;
    QString foldersPath_;
    QString scope_;             // "" = guest
    int batchDepth_ = 0;
};

}  // namespace ftpflow
