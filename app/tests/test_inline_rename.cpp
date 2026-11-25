// Regression test for the inline-rename plumbing at the model layer:
// Qt::ItemIsEditable / Qt::EditRole / setData() validation and the deferred
// filesystem mutation. Console-only (no GUI platform needed).
#include "local_fs_model.hpp"
#include "ui_util.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QModelIndex>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>

#include <cstdio>

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

// Let queued singleShot(0) tasks and the model's background probes finish.
void drainEvents(int ms = 200) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- shared name validator (used by both panes) ---------------------
    check(ftpflow::isValidEntryName(QStringLiteral("报告.txt")),
          "Chinese filename accepted");
    check(ftpflow::isValidEntryName(QStringLiteral("my.folder")),
          "dotted folder name accepted");
    check(!ftpflow::isValidEntryName(QString()), "empty name rejected");
    check(!ftpflow::isValidEntryName(QStringLiteral("  ")),
          "whitespace-only name rejected");
    check(!ftpflow::isValidEntryName(QStringLiteral(".")), "'.' rejected");
    check(!ftpflow::isValidEntryName(QStringLiteral("..")), "'..' rejected");
    check(!ftpflow::isValidEntryName(QStringLiteral("a/b")), "slash rejected");
    check(!ftpflow::isValidEntryName(QStringLiteral("a\\b")),
          "backslash rejected");
    check(!ftpflow::isValidEntryName(QStringLiteral("a:b")), "colon rejected");
    check(!ftpflow::isValidEntryName(QStringLiteral("a*b")),
          "wildcard rejected");
    // The editor commits through setData(), which trims first: surrounding
    // spaces are normalized away (same as Windows), not treated as an error.
    check(ftpflow::isValidEntryName(QStringLiteral("  name  ")),
          "surrounding spaces are trimmed by the caller");
    check(!ftpflow::isValidEntryName(QStringLiteral("name.")),
          "trailing dot rejected");
    check(!ftpflow::isValidEntryName(QStringLiteral("a\tb")),
          "control character rejected");

    // ---- LocalFsModel edit plumbing -------------------------------------
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory created");
    if (!dir.isValid()) {
        std::printf("inline_rename: %d checks, %d failed\n", checks, failures);
        return 1;
    }
    const QString oldPath = dir.filePath(QStringLiteral("old name.txt"));
    {
        QFile f(oldPath);
        check(f.open(QIODevice::WriteOnly), "test file created");
        f.write("x");
    }

    ftpflow::LocalFsModel model;
    std::string err;
    check(model.openRoot(dir.path().toStdString(), &err), "openRoot succeeds");

    int row = -1;
    for (int r = 0; r < model.rowCount(); ++r) {
        const QModelIndex n = model.index(r, ftpflow::LocalFsModel::ColName);
        if (n.data(Qt::EditRole).toString() == QStringLiteral("old name.txt")) {
            row = r;
            break;
        }
    }
    check(row >= 0, "row found through Qt::EditRole");
    if (row < 0) {
        std::printf("inline_rename: %d checks, %d failed\n", checks, failures);
        return failures ? 1 : 0;
    }

    const QModelIndex name = model.index(row, ftpflow::LocalFsModel::ColName);
    check((name.flags() & Qt::ItemIsEditable) != 0,
          "name cell is editable");
    check(name.data(Qt::EditRole).toString() ==
              QStringLiteral("old name.txt"),
          "EditRole returns the raw filename");
    const QModelIndex size = model.index(row, ftpflow::LocalFsModel::ColSize);
    check((size.flags() & Qt::ItemIsEditable) == 0,
          "size cell is not editable");
    check(size.data(Qt::EditRole).isValid() == false,
          "size cell has no EditRole");

    // Invalid / no-op edits are rejected without touching the filesystem.
    check(!model.setData(size, QStringLiteral("hack"), Qt::EditRole),
          "setData rejects a non-name column");
    check(!model.setData(name, QStringLiteral("hack"), Qt::DisplayRole),
          "setData rejects the wrong role");
    check(!model.setData(name, QStringLiteral("bad/name"), Qt::EditRole),
          "setData rejects an illegal name");
    check(!model.setData(name, QStringLiteral(".."), Qt::EditRole),
          "setData rejects '..'");
    check(!model.setData(name, QStringLiteral("old name.txt"), Qt::EditRole),
          "setData ignores an unchanged name");
    drainEvents(60);
    check(QFile::exists(oldPath), "no filesystem change after rejected edits");

    // A valid edit returns immediately, then renames on the next turn.
    const QString newPath = dir.filePath(QStringLiteral("new name.txt"));
    check(model.setData(name, QStringLiteral("new name.txt"), Qt::EditRole),
          "setData accepts a valid new name");
    drainEvents();
    check(QFile::exists(newPath), "file renamed on disk after event loop");
    check(!QFile::exists(oldPath), "old filename no longer exists");

    std::printf("inline_rename: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
