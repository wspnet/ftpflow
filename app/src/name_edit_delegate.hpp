// ftpflow -- shared inline-rename editor for file lists and the site tree.
// Header-only: no Q_OBJECT / moc needed (the class only overrides virtuals).
#pragma once

#include <QAbstractItemView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>

namespace ftpflow {

// Explorer-style inline rename editor. The base name (without the extension)
// is pre-selected, illegal filename characters cannot be typed, Enter commits
// and Esc cancels. No modal "new name" dialog is ever shown for a rename.
class NameEditDelegate : public QStyledItemDelegate {
public:
    explicit NameEditDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex&) const override {
        auto* e = new QLineEdit(parent);
        // theme.hpp gives inline editors zero vertical padding so the name is
        // not clipped inside a table row.
        e->setProperty("ftpflowInlineEdit", true);
        e->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[^/\\\\:*?\"<>|]+")), e));
        e->setToolTip(QStringLiteral("输入新名称后按回车确认，按 Esc 取消"));
        return e;
    }

    // Explorer-style: the editor overlays the remaining (read-only) columns
    // instead of being squeezed into the narrow name cell. This keeps long
    // file names fully visible while editing.
    void updateEditorGeometry(QWidget* editor,
                              const QStyleOptionViewItem& option,
                              const QModelIndex&) const override {
        QRect geom = option.rect;
        if (QWidget* vp = editor->parentWidget()) {
            const QRect bounds = vp->rect();
            geom.setLeft(qMax(geom.left(), bounds.left() + 1));
            const int maxRight = bounds.right() - 2;
            if (maxRight > geom.left() + 1) geom.setRight(maxRight);
        }
        editor->setGeometry(geom);
        editor->raise();
    }

    void setEditorData(QWidget* editor, const QModelIndex& idx) const override {
        QStyledItemDelegate::setEditorData(editor, idx);
        auto* e = qobject_cast<QLineEdit*>(editor);
        if (!e) return;
        const QString text = e->text();
        const int dot = text.lastIndexOf(QLatin1Char('.'));
        if (dot > 0 && dot < text.size() - 1)
            e->setSelection(0, dot);   // leave the extension untouched
        else
            e->selectAll();            // folder / extension-less name
    }

    bool eventFilter(QObject* object, QEvent* event) override {
        auto* e = qobject_cast<QLineEdit*>(object);
        if (e && event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            // Enter is committed by the base delegate, Esc cancels. Mark the
            // editor so the focus-out that follows neither commits twice nor
            // resurrects a name the user just cancelled.
            if (key == Qt::Key_Return || key == Qt::Key_Enter ||
                key == Qt::Key_Escape)
                e->setProperty(kDone, true);
        } else if (e && event->type() == QEvent::FocusOut &&
                   !e->property(kDone).toBool()) {
            // Explorer keeps the typed name when the editor loses focus.
            e->setProperty(kDone, true);
            emit commitData(e);
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }

private:
    static inline const char* kDone = "ftpflowRenameDone";
};

}  // namespace ftpflow
