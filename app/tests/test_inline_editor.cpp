// Regression test for the inline rename editor's on-screen geometry:
// the editor must fit inside a short table row (no vertical clipping) and
// overlay the read-only columns so long names remain visible (Explorer-style).
#include "name_edit_delegate.hpp"
#include "theme.hpp"

#include <QApplication>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QTableView>

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
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ftpflow::applyDarkTheme(app);

    QTableView view;
    view.resize(460, 200);
    QStandardItemModel model(4, 3);
    const char* names[] = {"bin", "lib", "PrecompiledApp.config", "Web.config"};
    for (int r = 0; r < 4; ++r) {
        model.setItem(r, 0, new QStandardItem(QString::fromUtf8(names[r])));
        model.setItem(r, 1, new QStandardItem(r < 2 ? "<dir>" : "13.0 KiB"));
        model.setItem(r, 2, new QStandardItem("2026-11-24 15:29"));
    }
    view.setModel(&model);
    view.horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    view.horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    view.horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    view.setColumnWidth(1, 90);
    view.setColumnWidth(2, 150);
    view.setItemDelegateForColumn(0, new ftpflow::NameEditDelegate(&view));
    view.setEditTriggers(QAbstractItemView::NoEditTriggers);
    view.show();
    app.processEvents();

    const QModelIndex idx = model.index(3, 0);
    view.openPersistentEditor(idx);
    app.processEvents();
    QWidget* editor = view.indexWidget(idx);
    check(editor != nullptr, "persistent editor created through the delegate");
    if (!editor) {
        std::printf("inline_editor: %d checks, %d failed\n", checks, failures);
        return failures ? 1 : 0;
    }

    check(editor->property("ftpflowInlineEdit").toBool(),
          "editor opts out of the padded QLineEdit style");
    check(editor->parentWidget() == view.viewport(),
          "editor is a child of the viewport");
    check(editor->width() > view.columnWidth(0) + 40,
          "editor overlays the size/date columns instead of the narrow cell");
    check(editor->width() <= view.viewport()->width(),
          "editor stays inside the viewport");
    check(editor->height() >= view.visualRect(idx).height(),
          "editor fully covers the row height");

    if (argc > 1) view.viewport()->grab().save(QString::fromLocal8Bit(argv[1]));
    view.closePersistentEditor(idx);

    std::printf("inline_editor: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
