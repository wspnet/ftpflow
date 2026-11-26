// ftpflow -- directory-listing sort helper shared by the UI models.
//
// Sorts entries "folders first, then files", each group by name, using a
// stable sort so an already name-sorted backend listing keeps its order
// within each group. A virtual ".." parent row sorts to the very top because
// '.' is the smallest printable ASCII byte.
#pragma once

#include <algorithm>
#include <string>

#include <QtCore/QHash>
#include <QtCore/QtGlobal>
#include <QtCore/QString>

#include "ftpflow/vfs.hpp"

namespace ftpflow {

// Sort role the UI models answer for QSortFilterProxyModel. The name column
// returns a "0<name>" (directory) / "1<name>" (file) key so the default name
// sort keeps folders above files; size/mtime return numeric values.
constexpr int kDirFirstSortRole = Qt::UserRole + 1;

// Group key answered by both file-table models so EntrySortProxyModel can
// pin ".." at the top and keep folders above files regardless of the active
// sort column or direction: 0 = synthetic ".." row, 1 = folder, 2 = file.
constexpr int kEntryKindRole = Qt::UserRole + 2;

inline bool isDirEntry(const Entry& e) {
    return e.type == EntryType::Directory;
}

// Stable sort: directories before files, then by name ascending.
inline void sortDirFirst(std::vector<Entry>& v) {
    std::stable_sort(v.begin(), v.end(),
                     [](const Entry& a, const Entry& b) {
                         const bool ad = isDirEntry(a);
                         const bool bd = isDirEntry(b);
                         if (ad != bd) return ad;
                         return a.name < b.name;
                     });
}

// The synthetic ".." row that sits at the top of a non-root listing.
inline Entry parentEntry() {
    Entry e;
    e.name = "..";
    e.type = EntryType::Directory;
    return e;
}

// Human-readable file type for the "文件类型" column, in the same spirit as
// Windows Explorer: "文件夹" / "文件" / "<EXT> 文件" with a few well-known
// friendly names (e.g. ICO 文件, CONFIG 文件, Microsoft Excel 工作表).
// Pure function over the entry so it works for both local and remote rows
// without any OS shell dependency.
inline QString fileTypeText(const Entry& e) {
    if (e.type == EntryType::Directory) return QStringLiteral("文件夹");
    const std::size_t dot = e.name.find_last_of('.');
    if (dot == std::string::npos || dot == 0 ||
        dot + 1 >= e.name.size())
        return QStringLiteral("文件");
    const QString ext = QString::fromStdString(e.name.substr(dot + 1))
                            .toUpper();
    // A few friendly names matching Windows Explorer's wording.
    static const QHash<QString, QString> kFriendly = {
        {QStringLiteral("ICO"),    QStringLiteral("ICO 文件")},
        {QStringLiteral("CONFIG"), QStringLiteral("CONFIG 文件")},
        {QStringLiteral("INI"),    QStringLiteral("INI 文件")},
        {QStringLiteral("JSON"),   QStringLiteral("JSON 文件")},
        {QStringLiteral("XML"),    QStringLiteral("XML 文件")},
        {QStringLiteral("TXT"),    QStringLiteral("文本文档")},
        {QStringLiteral("LOG"),    QStringLiteral("文本文档")},
        {QStringLiteral("MD"),     QStringLiteral("Markdown 文件")},
        {QStringLiteral("PDF"),    QStringLiteral("PDF 文件")},
        {QStringLiteral("PNG"),    QStringLiteral("PNG 文件")},
        {QStringLiteral("JPG"),    QStringLiteral("JPEG 文件")},
        {QStringLiteral("JPEG"),   QStringLiteral("JPEG 文件")},
        {QStringLiteral("GIF"),    QStringLiteral("GIF 文件")},
        {QStringLiteral("BMP"),    QStringLiteral("BMP 文件")},
        {QStringLiteral("ZIP"),    QStringLiteral("ZIP 压缩文件")},
        {QStringLiteral("RAR"),    QStringLiteral("RAR 压缩文件")},
        {QStringLiteral("7Z"),     QStringLiteral("7-Zip 压缩文件")},
        {QStringLiteral("EXE"),    QStringLiteral("应用程序")},
        {QStringLiteral("DLL"),    QStringLiteral("动态链接库")},
        {QStringLiteral("XLS"),    QStringLiteral("Microsoft Excel 工作表")},
        {QStringLiteral("XLSX"),   QStringLiteral("Microsoft Excel 工作表")},
        {QStringLiteral("DOC"),    QStringLiteral("Microsoft Word 文档")},
        {QStringLiteral("DOCX"),   QStringLiteral("Microsoft Word 文档")},
        {QStringLiteral("CPP"),    QStringLiteral("C++ 源文件")},
        {QStringLiteral("H"),      QStringLiteral("C/C++ 头文件")},
        {QStringLiteral("HPP"),    QStringLiteral("C/C++ 头文件")},
    };
    auto it = kFriendly.find(ext);
    if (it != kFriendly.end()) return it.value();
    return ext + QStringLiteral(" 文件");
}

}  // namespace ftpflow
