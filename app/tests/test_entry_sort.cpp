// Sorting-proxy regression: the ".." parent row stays pinned at the top and
// folders never interleave with files, on every column and in both sort
// directions (Windows-Explorer style). Also covers the 文件类型 column text.
#include "entry_sort.hpp"
#include "entry_sort_proxy.hpp"
#include "local_fs_model.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

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

void writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data);
}

using ftpflow::LocalFsModel;

// Return the ordered list of (kind, name) pairs from the proxy.
struct Row { int kind; std::string name; };
QList<Row> ordered(ftpflow::EntrySortProxyModel& proxy) {
    QList<Row> out;
    for (int r = 0; r < proxy.rowCount(); ++r) {
        const QModelIndex i = proxy.index(r, LocalFsModel::ColName);
        out.push_back({i.data(ftpflow::kEntryKindRole).toInt(),
                       i.data(Qt::DisplayRole).toString().toStdString()});
    }
    return out;
}

// Assert the invariant that matters: parent first, then all folders, then
// all files, regardless of column / direction.
void checkGrouping(const QList<Row>& rows, const char* what) {
    if (!rows.isEmpty())
        check(rows[0].kind == 0, "row 0 is the '..' parent row");
    bool sawFile = false;
    // After the parent row, exactly one folder block then one file block.
    for (int i = 1; i < rows.size(); ++i) {
        const int k = rows[i].kind;
        if (k == 2) sawFile = true;
        if (k == 1) check(!sawFile, "no folder after a file");
    }
    (void)what;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- fileTypeText() pure-function behaviour ------------------------
    {
        ftpflow::Entry d; d.type = ftpflow::EntryType::Directory;
        d.name = "Config";
        check(ftpflow::fileTypeText(d) == QStringLiteral("文件夹"),
              "directory -> 文件夹");

        ftpflow::Entry f; f.type = ftpflow::EntryType::File;
        f.name = "favicon.ico";
        check(ftpflow::fileTypeText(f) == QStringLiteral("ICO 文件"),
              ".ico -> ICO 文件");
        f.name = "ConnectionStrings.config";
        check(ftpflow::fileTypeText(f) == QStringLiteral("CONFIG 文件"),
              ".config -> CONFIG 文件");
        f.name = "newbook.core.xlsx";
        check(ftpflow::fileTypeText(f) ==
                  QStringLiteral("Microsoft Excel 工作表"),
              ".xlsx -> Excel");
        f.name = "README";
        check(ftpflow::fileTypeText(f) == QStringLiteral("文件"),
              "extensionless -> 文件");
        f.name = "data.weirdext";
        check(ftpflow::fileTypeText(f) == QStringLiteral("WEIRDEXT 文件"),
              "unknown ext -> <EXT> 文件");
    }

    // ---- build a fixture with mixed dirs and files ----------------------
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir created");
    if (!dir.isValid()) return 1;
    const QString root = dir.path();
    check(QDir(root).mkdir("Config"), "folder Config");
    check(QDir(root).mkdir("images"), "folder images");
    check(QDir(root).mkdir("jsonlist"), "folder jsonlist");
    writeFile(root + "/favicon.ico", QByteArray(963, 'a'));
    writeFile(root + "/newbook.core.xlsx", QByteArray(4412, 'b'));
    writeFile(root + "/ConnectionStrings.config", QByteArray(854, 'c'));
    writeFile(root + "/zeta.txt", QByteArray(2000, 'd'));

    LocalFsModel model;
    std::string err;
    check(model.openRoot(root.toStdString(), &err), "model openRoot");
    check(model.rowCount() == 8, "8 rows incl '..'");  // .. + 3 dirs + 4 files

    ftpflow::EntrySortProxyModel proxy;
    proxy.setSourceModel(&model);

    const int cols[] = {
        LocalFsModel::ColName, LocalFsModel::ColSize,
        LocalFsModel::ColType, LocalFsModel::ColModified,
    };
    const Qt::SortOrder orders[] = {Qt::AscendingOrder, Qt::DescendingOrder};

    for (int c : cols) {
        for (Qt::SortOrder o : orders) {
            proxy.sort(c, o);
            const QList<Row> rows = ordered(proxy);
            checkGrouping(rows, "grouping");

            // Within the folder group and within the file group, names must
            // be a contiguous block each (count matches).
            int dirs = 0, files = 0;
            for (const Row& r : rows) {
                if (r.kind == 1) ++dirs;
                if (r.kind == 2) ++files;
            }
            check(dirs == 3, "three folders present");
            check(files == 4, "four files present");
        }
    }

    // Spot check: name-sorted ascending, the first folder is "Config" and
    // the last row (largest kind) is a file; descending flips names inside
    // groups but the top row is still "..".
    proxy.sort(LocalFsModel::ColName, Qt::AscendingOrder);
    QList<Row> asc = ordered(proxy);
    check(asc[0].name == "..", "'..' first, name asc");
    check(asc[1].name == "Config", "first folder Config, name asc");
    proxy.sort(LocalFsModel::ColName, Qt::DescendingOrder);
    QList<Row> desc = ordered(proxy);
    check(desc[0].name == "..", "'..' still first, name desc");
    check(desc[1].kind == 1, "folder still above files, name desc");

    // Size-sorted: '..' pinned, folders still above files.
    proxy.sort(LocalFsModel::ColSize, Qt::DescendingOrder);
    QList<Row> bySize = ordered(proxy);
    check(bySize[0].name == "..", "'..' first, size desc");
    check(bySize[1].kind == 1, "folder above files, size desc");
    proxy.sort(LocalFsModel::ColSize, Qt::AscendingOrder);
    bySize = ordered(proxy);
    check(bySize[0].name == "..", "'..' first, size asc");
    check(bySize[1].kind == 1, "folder above files, size asc");

    // Type-sorted both directions keeps the grouping.
    proxy.sort(LocalFsModel::ColType, Qt::DescendingOrder);
    QList<Row> byType = ordered(proxy);
    check(byType[0].name == "..", "'..' first, type desc");
    check(byType[1].kind == 1, "folder above files, type desc");

    std::printf("entry_sort: %d checks, %d failed\n", checks, failures);
    if (failures == 0) std::printf("ALL ENTRY-SORT TESTS PASSED\n");
    return failures == 0 ? 0 : 1;
}
