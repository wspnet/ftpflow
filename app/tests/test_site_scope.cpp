// ftpflow -- identity-scoped site store regression test.
//
// Verifies the invariants the member/guest separation depends on:
//   * each scope has its own sites/folders files (guest keeps the legacy
//     paths so existing installs keep working),
//   * credentials are namespaced per scope, so a guest (or another member)
//     cannot read a member's FTP password,
//   * clearAll() wipes only that scope (used to empty the guest list after
//     its sites have been migrated into a member account).
//
// Run manually: build/release-full/app/test_site_scope.exe
// This uses its OWN QSettings app/org name, so the real user's sites.json is
// never touched.
#include "site_store.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdio>

namespace {

int checks = 0;
int failures = 0;

void check(bool cond, const char* what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

ftpflow::SiteEntry makeSite(const QString& name) {
    ftpflow::SiteEntry e;
    e.name = name;
    e.isSftp = false;
    e.host = "example.test";
    e.port = 21;
    e.user = "tester";
    return e;
}

void removeFile(const QString& path) {
    if (!path.isEmpty()) QFile::remove(path);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // Deliberately different from the real app so defaultFilePath() points
    // at a private test file, not the user's real sites.json.
    QCoreApplication::setApplicationName("ftpflow-scope-test");
    QCoreApplication::setOrganizationName("ftpflow-scope-test");

    const QString scopeA = QStringLiteral("m_scopetest_a");
    const QString scopeB = QStringLiteral("m_scopetest_b");
    const QString fileA = ftpflow::SiteStore::filePathForScope(scopeA);
    const QString foldersA = ftpflow::SiteStore::foldersFilePathForScope(scopeA);
    const QString fileB = ftpflow::SiteStore::filePathForScope(scopeB);
    const QString foldersB = ftpflow::SiteStore::foldersFilePathForScope(scopeB);
    const QString guestFile = ftpflow::SiteStore::defaultFilePath();
    const QString guestFolders = ftpflow::SiteStore::foldersFilePath();

    // Start clean (also removes leftovers from a previously aborted run).
    removeFile(fileA);
    removeFile(foldersA);
    removeFile(fileB);
    removeFile(foldersB);

    check(guestFile == ftpflow::SiteStore::filePathForScope(QString()),
          "guest file path equals the legacy default path");
    check(guestFolders == ftpflow::SiteStore::foldersFilePathForScope(QString()),
          "guest folders path equals the legacy default path");
    check(fileA != fileB, "different members get different site files");
    check(fileA != guestFile, "member file differs from the guest file");

    ftpflow::SiteStore store;

    // ---- Member A: one site, one folder, one password --------------------
    store.setScope(scopeA);
    check(store.scope() == scopeA, "setScope stores the identity key");
    store.load();
    store.upsert(makeSite(QStringLiteral("SiteA")));
    store.addFolder(QStringLiteral("GroupA"));
    check(store.writePassword(QStringLiteral("SiteA"),
                              QStringLiteral("secret-A")),
          "member A password written");
    check(store.save(), "member A saved");
    check(QFile::exists(fileA), "scope A sites file created");
    check(QFile::exists(foldersA), "scope A folders file created");

    // The guest list must not have been touched at all.
    {
        QFile f(guestFile);
        bool hasSiteA = false;
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
            for (const QJsonValue& v : d.array())
                if (v.toObject().value("name").toString() == "SiteA")
                    hasSiteA = true;
        }
        check(!hasSiteA, "guest sites.json has no member A site");
    }

    // ---- Switch to guest: no sites, no passwords -------------------------
    store.setScope(QString());
    check(store.scope().isEmpty(), "guest scope is empty");
    store.load();
    check(store.sites().isEmpty(), "guest site list is separate from member A");
    check(store.folders().isEmpty(), "guest folders are separate");
    check(store.readPassword(QStringLiteral("SiteA")).isEmpty(),
          "guest cannot read member A credential");

    // ---- Member B is independent of member A ----------------------------
    store.setScope(scopeB);
    store.load();
    check(store.sites().isEmpty(), "member B starts with its own empty list");
    check(store.readPassword(QStringLiteral("SiteA")).isEmpty(),
          "member B cannot read member A credential");
    store.upsert(makeSite(QStringLiteral("SiteB")));
    check(store.writePassword(QStringLiteral("SiteB"),
                              QStringLiteral("secret-B")),
          "member B password written");
    check(store.save(), "member B saved");

    // ---- Back to member A: data survived --------------------------------
    store.setScope(scopeA);
    store.load();
    check(store.sites().size() == 1, "member A site restored after switching");
    check(store.folders().contains(QStringLiteral("GroupA")),
          "member A folder restored");
    check(store.readPassword(QStringLiteral("SiteA")) ==
              QStringLiteral("secret-A"),
          "member A credential restored");

    // ---- clearAll(scopeA) must not affect member B ----------------------
    store.clearAll();
    check(store.sites().isEmpty(), "clearAll empties member A list");
    check(!QFile::exists(fileA), "clearAll removes member A sites file");
    check(!QFile::exists(foldersA), "clearAll removes member A folders file");
    check(store.readPassword(QStringLiteral("SiteA")).isEmpty(),
          "clearAll erases member A credential");

    store.setScope(scopeB);
    store.load();
    check(store.sites().size() == 1, "member B list survives clearing A");
    check(store.readPassword(QStringLiteral("SiteB")) ==
              QStringLiteral("secret-B"),
          "member B credential survives clearing A");

    // Cleanup: remove all test artifacts (files + credentials).
    store.clearAll();
    removeFile(fileA);
    removeFile(foldersA);
    removeFile(fileB);
    removeFile(foldersB);

    std::printf("site_scope: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
