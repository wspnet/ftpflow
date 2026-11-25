// ftpflow -- frameless-dialog title bar implementation.
#include "dialog_title_bar.hpp"

#include "theme.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>

namespace ftpflow {

namespace {

// Close glyph drawn with round-capped lines, matching TitleBar's style.
QIcon makeCloseGlyph() {
    const int s = 32;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c(0xd4, 0xd4, 0xd4);
    QPen pen(c, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);

    const qreal m = 8.0;
    p.drawLine(QPointF(m, m), QPointF(s - m, s - m));
    p.drawLine(QPointF(s - m, m), QPointF(m, s - m));
    p.end();
    return QIcon(pm);
}

}  // namespace

DialogTitleBar::DialogTitleBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(preferredHeight());
    setObjectName("titleBar");

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 0, 0, 0);
    lay->setSpacing(0);

    title_ = new QLabel(this);
    title_->setObjectName("titleBarText");
    lay->addSpacing(4);
    lay->addWidget(title_);
    lay->addStretch(1);

    closeBtn_ = new QToolButton(this);
    closeBtn_->setObjectName("titleBarBtn");
    closeBtn_->setIcon(makeCloseGlyph());
    closeBtn_->setIconSize(QSize(16, 16));
    closeBtn_->setFixedSize(46, preferredHeight());
    closeBtn_->setAutoRaise(true);
    closeBtn_->setFocusPolicy(Qt::NoFocus);
    closeBtn_->setObjectName("titleBarCloseBtn");
    lay->addWidget(closeBtn_);

    connect(closeBtn_, &QToolButton::clicked, this, [this] {
        if (window_) window_->close();
    });
}

void DialogTitleBar::attachWindow(QWidget* window) {
    window_ = window;
}

void DialogTitleBar::setTitle(const QString& title) {
    title_->setText(title);
}

void DialogTitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && window_) {
        dragOffset_ = event->globalPosition().toPoint() -
                      window_->frameGeometry().topLeft();
        dragging_ = true;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DialogTitleBar::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && window_ && (event->buttons() & Qt::LeftButton)) {
        window_->move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void DialogTitleBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DialogTitleBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Slim dark strip with a hairline bottom border (matches TitleBar).
    p.fillRect(rect(), QColor(kPanel));
    p.setPen(QColor(kBorder));
    p.drawLine(0, height() - 1, width(), height() - 1);

    // App mark: a small teal glyph at the left.
    const qreal cy = height() / 2.0;
    const qreal x = 12.0;
    QPen pen(QColor(kAccent), 2.0, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    p.drawLine(QPointF(x, cy - 5), QPointF(x + 6, cy - 1));
    p.drawLine(QPointF(x, cy + 5), QPointF(x + 6, cy + 1));
}

}  // namespace ftpflow
