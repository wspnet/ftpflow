// ftpflow -- saved connection sites (JSON) + passwords in the Windows
// Credential Manager. The JSON never contains secrets: passwords live in
// Credential Manager blobs under "ftpflow/site/<name>" (per-user, DPAPI
// protected at rest by the OS).
#pragma once

#include <QList>
#include <QString>

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
    // Transient: filled from Credential Manager, never serialized.
    QString password;
};

class SiteStore {
public:
    // %APPDATA%/ftpflow/sites.json (organization set in main()).
    static QString defaultFilePath();

    bool load(const QString& path = QString());
    bool save(const QString& path = QString()) const;

    const QList<SiteEntry>& sites() const { return sites_; }
    // Insert or replace by name.
    void upsert(const SiteEntry& site);
    void remove(const QString& name);

    // Grouping (XTerminal-style multi-level organization).
    QList<QString> groups() const;              // sorted, deduped, "" excluded
    QList<SiteEntry> sitesInGroup(const QString& group) const;  // "" = ungrouped

    // Import / export the full site list (JSON). Passwords are NOT included
    // (they live in the OS Credential Manager); export only emits metadata.
    bool exportTo(const QString& path, QString* error) const;
    // Merge import: existing entries with the same name are replaced.
    bool importFrom(const QString& path, QString* error);

    // ---- Windows Credential Manager --------------------------------------
    static QString readPassword(const QString& siteName);
    static bool writePassword(const QString& siteName,
                              const QString& password);
    static void erasePassword(const QString& siteName);

    // Entry copy with the password filled from Credential Manager ("" when
    // none is stored).
    SiteEntry withPassword(const SiteEntry& e) const;

private:
    QList<SiteEntry> sites_;
    QString path_;
};

}  // namespace ftpflow
