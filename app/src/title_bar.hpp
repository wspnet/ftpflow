// ftpflow -- custom frameless-window title bar (XTerminal-style).
//
// Replaces the native window chrome: a slim dark strip with the app mark and
// title on the left, and min / max / close controls on the right. The host
// window is frameless; this bar provides dragging, double-click-to-maximize,
// and the window buttons.
#pragma once

#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;
class QToolButton;

namespace ftpflow {

class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);

    // The window whose state this bar controls (must be the top-level window
    // that contains this bar). Set once after construction.
    void attachWindow(QWidget* window);
    void setTitle(const QString& title);

    static int preferredHeight() { return 36; }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void syncMaximizeIcon();

    QWidget* window_ = nullptr;   // controlled top-level window
    QPoint pressGlobalPos_;       // press point (global) for drag threshold
    QPoint dragOffset_;           // press point relative to window origin
    bool pressed_ = false;        // left button held on the bar
    bool dragging_ = false;       // moved past the threshold -> real drag

    QLabel* title_ = nullptr;
    QToolButton* minBtn_ = nullptr;
    QToolButton* maxBtn_ = nullptr;
    QToolButton* closeBtn_ = nullptr;
};

}  // namespace ftpflow
