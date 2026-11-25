// ftpflow -- frameless-dialog title bar (matches the main window chrome).
//
// A slim strip (same height, background and glyph style as TitleBar) with the
// dialog title on the left and a single close button on the right. Dialogs
// don't offer minimize/maximize, so only close is provided. The host dialog
// must be frameless; this bar provides dragging and the close action.
#pragma once

#include <QtWidgets/QWidget>

class QLabel;
class QToolButton;

namespace ftpflow {

class DialogTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit DialogTitleBar(QWidget* parent = nullptr);

    // The dialog this bar controls (must be the top-level dialog that
    // contains this bar). Set once after construction.
    void attachWindow(QWidget* window);
    void setTitle(const QString& title);

    static int preferredHeight() { return 36; }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QWidget* window_ = nullptr;   // controlled top-level dialog
    QPoint dragOffset_;           // press point relative to window origin
    bool dragging_ = false;

    QLabel* title_ = nullptr;
    QToolButton* closeBtn_ = nullptr;
};

}  // namespace ftpflow
