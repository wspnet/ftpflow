// ftpflow -- sorting proxy for the local/remote file tables.
//
// Windows-Explorer-style ordering, independent of which column is sorted
// and of the sort direction:
//   1. the synthetic ".." parent row is ALWAYS pinned at the very top;
//   2. folders always come before files (never interleaved);
//   3. within each group, rows are ordered by the active sort column,
//      honouring ascending/descending.
//
// The source models answer kEntryKindRole with 0 (parent row), 1 (folder),
// or 2 (file). Qt calls lessThan(right, left) for descending order (i.e. it
// reverses the whole ordering the comparator defines), so the GROUP test
// below flips with sortOrder() to cancel that out and keep folders above
// files in both directions. Rows inside the same group are handed to the
// base comparator with the original (already direction-adjusted) args.
#pragma once

#include <QtCore/QModelIndex>
#include <QtCore/QSortFilterProxyModel>

#include "entry_sort.hpp"

namespace ftpflow {

class EntrySortProxyModel : public QSortFilterProxyModel {
public:
    explicit EntrySortProxyModel(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {
        setSortRole(kDirFirstSortRole);
    }

protected:
    bool lessThan(const QModelIndex& left,
                  const QModelIndex& right) const override {
        const int lk = left.data(kEntryKindRole).toInt();
        const int rk = right.data(kEntryKindRole).toInt();
        if (lk != rk) {
            // Pin ".."(0) < folder(1) < file(2) in BOTH directions. On a
            // descending sort Qt has swapped the args; flipping the test
            // here cancels that swap so the grouping stays fixed.
            return sortOrder() == Qt::AscendingOrder ? (lk < rk)
                                                     : (lk > rk);
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }
};

}  // namespace ftpflow
