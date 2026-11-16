// ftpflow -- self-hosted terminal emulator widget implementation.
#include "terminal_widget.hpp"

#include <cmath>
#include <utility>

#include <QFontDatabase>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

namespace ftpflow {

namespace {

// Default colors (dark terminal, matching the app's theme).
const QColor kBg(0x1e, 0x1e, 0x1e);
const QColor kFg(0xd4, 0xd4, 0xd4);

}  // namespace

TerminalWidget::TerminalWidget(QWidget* parent)
    : QWidget(parent), buffer_(80, 24), parser_(buffer_) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::IBeamCursor);

    // A monospaced font that is widely available on Windows.
    font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font_.setPointSize(10);
    font_.setStyleHint(QFont::Monospace);
    setFont(font_);

    QFontMetricsF fm(font_);
    cellW_ = std::max(1, (int)std::ceil(fm.horizontalAdvance('M')));
    cellH_ = std::max(1, (int)std::ceil(fm.height()));
    fontDescent_ = (int)std::ceil(fm.descent());
    geometryDirty_ = true;
}

void TerminalWidget::feed(const QByteArray& bytes) {
    parser_.feed(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    update();   // repaint the changed grid
}

void TerminalWidget::recomputeGeometry() {
    const int w = width();
    const int h = height();
    const int cols = std::max(1, w / cellW_);
    const int rows = std::max(1, h / cellH_);
    if (cols != buffer_.cols() || rows != buffer_.rows()) {
        buffer_.resize(cols, rows);
        emit sizeChanged(cols, rows);
    }
}

void TerminalWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    recomputeGeometry();
}

void TerminalWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setFont(font_);

    const int cols = buffer_.cols();
    const int rows = buffer_.rows();
    const int cx = buffer_.cursor().col;
    const int cy = buffer_.cursor().row;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const TermCell& cell = buffer_.cell(c, r);
            const int x = c * cellW_;
            const int y = r * cellH_;

            // Foreground/background, honoring inverse-video.
            QColor fg = resolveColor(cell.fg, true);
            QColor bg = resolveColor(cell.bg, false);
            if (cell.flags & TermCell::kInverse) std::swap(fg, bg);

            p.fillRect(x, y, cellW_, cellH_, bg);

            // Character (skip blanks).
            if (cell.ch == 0 || cell.ch == ' ') continue;

            if (cell.flags & TermCell::kBold) {
                QFont bf = font_;
                bf.setBold(true);
                p.setFont(bf);
            }
            p.setPen(fg);

            // Render the code point (BMP only for now; surrogates ignored).
            if (cell.ch < 0x10000) {
                p.drawText(x, y + cellH_ - fontDescent_, QChar(cell.ch));
            }

            if (cell.flags & TermCell::kUnderline) {
                p.fillRect(x, y + cellH_ - 2, cellW_, 1, fg);
            }
            p.setFont(font_);
        }
    }

    // Cursor (block).
    if (buffer_.cursorVisible() && hasFocus()) {
        p.fillRect(cx * cellW_, cy * cellH_, cellW_, cellH_,
                   QColor(0x2e, 0xa8, 0xc0));   // accent, translucent look
    }
}

QColor TerminalWidget::resolveColor(const TermColor& c, bool fg) const {
    if (c.kind == TermColor::Kind::Rgb)
        return QColor(c.r, c.g, c.b);
    // Default: use the terminal's default fg/bg.
    return fg ? kFg : kBg;
}

void TerminalWidget::sendString(const QByteArray& s) {
    if (s.isEmpty()) return;
    emit dataReady(s);
}

void TerminalWidget::keyPressEvent(QKeyEvent* e) {
    const QString text = e->text();
    const int key = e->key();
    const Qt::KeyboardModifiers mods = e->modifiers();

    // Ctrl combinations produce control bytes.
    if (mods & Qt::ControlModifier && !(mods & Qt::AltModifier)) {
        QChar c;
        if (text.size() == 1) c = text[0];
        if (c.isLetter()) {
            sendString(QByteArray(1, (char)(c.toUpper().unicode() - 'A' + 1)));
            return;
        }
        // Ctrl+[ = ESC, Ctrl+\ = FS, Ctrl+] = GS, etc.
        const int codes[] = {0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x7f};
        if (key >= Qt::Key_BracketLeft && key <= Qt::Key_AsciiTilde) {
            sendString(QByteArray(1, (char)codes[key - Qt::Key_BracketLeft]));
            return;
        }
        // Fall through for other Ctrl keys.
    }

    switch (key) {
        case Qt::Key_Return: case Qt::Key_Enter:
            sendString("\r"); return;
        case Qt::Key_Backspace:
            sendString("\x7f"); return;
        case Qt::Key_Tab:
            sendString("\t"); return;
        case Qt::Key_Escape:
            sendString("\x1b"); return;
        case Qt::Key_Up:
            sendString("\x1b[A"); return;
        case Qt::Key_Down:
            sendString("\x1b[B"); return;
        case Qt::Key_Right:
            sendString("\x1b[C"); return;
        case Qt::Key_Left:
            sendString("\x1b[D"); return;
        case Qt::Key_Home:
            sendString("\x1b[H"); return;
        case Qt::Key_End:
            sendString("\x1b[F"); return;
        case Qt::Key_Insert:
            sendString("\x1b[2~"); return;
        case Qt::Key_Delete:
            sendString("\x1b[3~"); return;
        case Qt::Key_PageUp:
            sendString("\x1b[5~"); return;
        case Qt::Key_PageDown:
            sendString("\x1b[6~"); return;
        default:
            break;
    }

    // Arrow keys with modifiers (e.g. Ctrl+Left in vi/readline).
    if (key >= Qt::Key_Up && key <= Qt::Key_Left && (mods & (Qt::ControlModifier | Qt::AltModifier))) {
        const char* seq = nullptr;
        if (key == Qt::Key_Up) seq = (mods & Qt::ControlModifier) ? "\x1b[1;5A" : "\x1b[1;3A";
        else if (key == Qt::Key_Down) seq = (mods & Qt::ControlModifier) ? "\x1b[1;5B" : "\x1b[1;3B";
        else if (key == Qt::Key_Right) seq = (mods & Qt::ControlModifier) ? "\x1b[1;5C" : "\x1b[1;3C";
        else if (key == Qt::Key_Left) seq = (mods & Qt::ControlModifier) ? "\x1b[1;5D" : "\x1b[1;3D";
        if (seq) { sendString(seq); return; }
    }

    // Ordinary printable text.
    if (!text.isEmpty() && !(mods & Qt::ControlModifier)) {
        sendString(text.toUtf8());
        return;
    }

    QWidget::keyPressEvent(e);
}

void TerminalWidget::mousePressEvent(QMouseEvent* e) {
    // Keep focus in the terminal so keyboard input keeps flowing.
    if (!hasFocus()) setFocus();
    QWidget::mousePressEvent(e);
}

}  // namespace ftpflow
