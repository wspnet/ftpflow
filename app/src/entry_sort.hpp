// ftpflow -- directory-listing sort helper shared by the UI models.
//
// Sorts entries "folders first, then files", each group by name, using a
// stable sort so an already name-sorted backend listing keeps its order
// within each group. A virtual ".." parent row sorts to the very top because
// '.' is the smallest printable ASCII byte.
#pragma once

#include <algorithm>
#include <string>

#include <QtCore/QtGlobal>

#include "ftpflow/vfs.hpp"

namespace ftpflow {

// Sort role the UI models answer for QSortFilterProxyModel. The name column
// returns a "0<name>" (directory) / "1<name>" (file) key so the default name
// sort keeps folders above files; size/mtime return numeric values.
constexpr int kDirFirstSortRole = Qt::UserRole + 1;

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

}  // namespace ftpflow
