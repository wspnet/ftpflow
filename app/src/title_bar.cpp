// ftpflow -- custom frameless-window title bar implementation.
#include "title_bar.hpp"

#include "theme.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>

namespace ftpflow {

namespace {

// A crisp, theme-aware window-control glyph (minimize / maximize / restore /
// close) drawn with round-capped lines, matching the app's flat icon style.
QIcon makeWindowGlyph(char kind) {
    const int s = 32;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c(0xd4, 0xd4, 0xd4);
    QPen pen(c, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);

    const qreal m = 8.0;
    const QRectF r(m, m, s - 2 * m, s - 2 * m);
    switch (kind) {
        case 'm':  // minimize: a bottom line
            p.drawLine(QPointF(m, s - m), QPointF(s - m, s - m));
            break;
        case 'x':  // maximize: a square
            p.drawRect(r);
            break;
        case 'r':  // restore: two overlapping squares
            p.drawRect(QRectF(m + 3, m, s - 2 * m - 3, s - 2 * m - 3));
            p.drawPolyline(QPolygonF{
                QPointF(m, m + 3), QPointF(m, s - m), QPointF(s - m, s - m),
                QPointF(s - m, m + 3)});
            break;
        case 'c':  // close: an X
            p.drawLine(QPointF(m, m), QPointF(s - m, s - m));
            p.drawLine(QPointF(s - m, m), QPointF(m, s - m));
            break;
        default:
            break;
    }
    p.end();
    return QIcon(pm);
}

}  // namespace

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(preferredHeight());
    setObjectName("titleBar");

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 0, 0, 0);
    lay->setSpacing(0);

    // App mark (drawn in paintEvent) + title text.
    title_ = new QLabel(this);
    title_->setObjectName("titleBarText");
    lay->addSpacing(4);
    lay->addWidget(title_);
    lay->addStretch(1);

    // Window controls (right).
    auto makeBtn = [&](char glyph) {
        auto* b = new QToolButton(this);
        b->setObjectName("titleBarBtn");
        b->setIcon(makeWindowGlyph(glyph));
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(46, preferredHeight());
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(b);
        return b;
    };
    minBtn_ = makeBtn('m');
    maxBtn_ = makeBtn('x');
    closeBtn_ = makeBtn('c');
    closeBtn_->setObjectName("titleBarCloseBtn");

    connect(minBtn_, &QToolButton::clicked, this, [this] {
        if (window_) window_->showMinimized();
    });
    connect(maxBtn_, &QToolButton::clicked, this, [this] {
        if (!window_) return;
        if (window_->isMaximized()) window_->showNormal();
        else window_->showMaximized();
    });
    connect(closeBtn_, &QToolButton::clicked, this, [this] {
        if (window_) window_->close();
    });

    // Repaint the bar when the window toggles maximized (restore icon swaps
    // in syncMaximizeIcon, which watches the window's state).
}

void TitleBar::attachWindow(QWidget* window) {
    window_ = window;
    if (!window_) return;
    window_->installEventFilter(this);
    syncMaximizeIcon();
}

void TitleBar::setTitle(const QString& title) {
    title_->setText(title);
}

void TitleBar::syncMaximizeIcon() {
    if (!window_ || !maxBtn_) return;
    maxBtn_->setIcon(makeWindowGlyph(window_->isMaximized() ? 'r' : 'x'));
    maxBtn_->setToolTip(window_->isMaximized() ? "还原" : "最大化");
}

bool TitleBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_ && event->type() == QEvent::WindowStateChange) {
        syncMaximizeIcon();
    }
    return QWidget::eventFilter(watched, event);
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && window_) {
        // Record the press point (global) and the cursor offset from the
        // window's top-left. Dragging only starts once the cursor moves past
        // a small threshold, so a double-click's micro-jitter can't hijack
        // the maximize/restore toggle (see mouseMoveEvent).
        pressGlobalPos_ = event->globalPosition().toPoint();
        dragOffset_ = event->globalPosition().toPoint() -
                      window_->frameGeometry().topLeft();
        pressed_ = true;
        dragging_ = false;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent* event) {
    if (pressed_ && window_ && (event->buttons() & Qt::LeftButton)) {
        const QPoint cur = event->globalPosition().toPoint();
        if (!dragging_) {
            // Only begin a drag after passing the system drag threshold;
            // until then, stay put so a double-click still registers.
            const int dx = qAbs(cur.x() - pressGlobalPos_.x());
            const int dy = qAbs(cur.y() - pressGlobalPos_.y());
            if (dx < 4 && dy < 4) {
                event->accept();
                return;
            }
            dragging_ = true;
        }
        // Dragging while maximized first restores the window at the cursor.
        if (window_->isMaximized()) {
            window_->showNormal();
            // Recenter under the cursor so it doesn't jump.
            const QPoint g = event->globalPosition().toPoint();
            window_->move(g.x() - dragOffset_.x(), g.y() - dragOffset_.y());
        }
        window_->move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressed_ = false;
        dragging_ = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && window_) {
        // Toggle maximize/restore. Reset drag state first so a leftover
        // "pressed" flag can't turn the post-double-click release into a
        // spurious move.
        pressed_ = false;
        dragging_ = false;
        if (window_->isMaximized()) window_->showNormal();
        else window_->showMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TitleBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Slim dark strip with a hairline bottom border.
    p.fillRect(rect(), QColor(kPanel));
    p.setPen(QColor(kBorder));
    p.drawLine(0, height() - 1, width(), height() - 1);

    // App mark: a small teal glyph at the left.
    const qreal cy = height() / 2.0;
    const qreal x = 12.0;
    QPen pen(QColor(kAccent), 2.0, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    // Two short diagonal strokes forming a subtle "transfer" mark.
    p.drawLine(QPointF(x, cy - 5), QPointF(x + 6, cy - 1));
    p.drawLine(QPointF(x, cy + 5), QPointF(x + 6, cy + 1));
}

}  // namespace ftpflow
