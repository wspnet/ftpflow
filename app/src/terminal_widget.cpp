// ftpflow -- self-hosted terminal emulator widget implementation.
#include "terminal_widget.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "ftpflow/text_codec.hpp"

namespace ftpflow {

namespace {

// Default colors (dark terminal, matching the app's theme).
const QColor kBg(0x1e, 0x1e, 0x1e);
const QColor kFg(0xd4, 0xd4, 0xd4);
const QColor kCursor(0x2e, 0xa8, 0xc0);      // theme accent
const QColor kCursorText(0x1e, 0x1e, 0x1e);  // readable on the accent block
const QColor kSelectBg(0x26, 0x4f, 0x78);    // classic selection blue
const QColor kFindBg(0x61, 0x32, 0x14);      // search hit (dim orange)
const QColor kFindBgActive(0xa0, 0x60, 0x30); // current search hit

// Modern dark palette (VS Code Dark+ / One Dark family): tuned so that all
// 16 ANSI colors are clearly distinguishable on the #1e1e1e background,
// instead of the harsh legacy VGA colors.
const QRgb kAnsi16[16] = {
    0xff3e4451,  // 0 black   (visible against the background)
    0xffe06c75,  // 1 red
    0xff98c379,  // 2 green
    0xffe5c07b,  // 3 yellow
    0xff61afef,  // 4 blue
    0xffc678dd,  // 5 magenta
    0xff56b6c2,  // 6 cyan
    0xffabb2bf,  // 7 white
    0xff5c6370,  // 8 bright black (comments)
    0xffff7b86,  // 9 bright red
    0xffa9d98a,  // 10 bright green
    0xffefd88e,  // 11 bright yellow
    0xff74bfff,  // 12 bright blue
    0xffd589f0,  // 13 bright magenta
    0xff66cfe0,  // 14 bright cyan
    0xffffffff,  // 15 bright white
};

// Resolve an ANSI palette index (0..255) to a color.
QColor ansiColor(int idx) {
    if (idx < 0) idx = 0;
    if (idx < 16) return QColor::fromRgb(kAnsi16[idx]);
    if (idx < 232) {
        // 6x6x6 color cube: 16 + 36r + 6g + b.
        const int n = idx - 16;
        const int r = n / 36, g = (n % 36) / 6, b = n % 6;
        const auto level = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return QColor(level(r), level(g), level(b));
    }
    // Grayscale ramp: 232..255.
    const int v = 8 + (std::min(idx, 255) - 232) * 10;
    return QColor(v, v, v);
}

QColor blend(const QColor& fg, const QColor& bg, qreal fgWeight) {
    return QColor(
        int(fg.red() * fgWeight + bg.red() * (1 - fgWeight)),
        int(fg.green() * fgWeight + bg.green() * (1 - fgWeight)),
        int(fg.blue() * fgWeight + bg.blue() * (1 - fgWeight)));
}

}  // namespace

TerminalWidget::TerminalWidget(QWidget* parent)
    : QWidget(parent), buffer_(80, 24), parser_(buffer_) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::IBeamCursor);

    // A monospaced font that is widely available on Windows.
    font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font_.setPointSize(fontPt_);
    font_.setStyleHint(QFont::Monospace);
    setFont(font_);
    applyFontMetrics();

    // Remote OSC 0/2 title changes are surfaced to the owner (tab label).
    parser_.setTitleCallback([this](const std::string& t) {
        emit titleChanged(QString::fromStdString(t));
    });
}

void TerminalWidget::applyFontMetrics() {
    QFontMetricsF fm(font_);
    cellW_ = std::max(1, (int)std::ceil(fm.horizontalAdvance('M')));
    cellH_ = std::max(1, (int)std::ceil(fm.height()));
    fontDescent_ = (int)std::ceil(fm.descent());
}

void TerminalWidget::zoomBy(int deltaPt) {
    const int pt = std::clamp(fontPt_ + deltaPt, 6, 32);
    if (pt == fontPt_) return;
    fontPt_ = pt;
    font_.setPointSize(fontPt_);
    setFont(font_);
    applyFontMetrics();
    recomputeGeometry();
    if (findBar_ && findBar_->isVisible()) {
        findBar_->move(std::max(4, width() - findBar_->width() - 8), 8);
    }
    update();
}

void TerminalWidget::resetZoom() {
    if (fontPt_ == 10) return;
    fontPt_ = 10;
    font_.setPointSize(fontPt_);
    setFont(font_);
    applyFontMetrics();
    recomputeGeometry();
    update();
}

// --- on-screen soft keyboard ----------------------------------------------

// Lazily build the floating arrow/enter cluster. Buttons use auto-repeat so
// holding one (e.g. to walk back through shell history) keeps emitting the
// escape sequence. Clicks never steal focus, so the terminal cursor stays
// active and key input keeps flowing to the remote.
void TerminalWidget::setSoftKeyboardVisible(bool visible) {
    softKeysVisible_ = visible;
    if (visible) {
        if (!softKeys_) {
            softKeys_ = new QWidget(this);
            softKeys_->setObjectName("termSoftKeys");
            auto* lay = new QVBoxLayout(softKeys_);
            lay->setContentsMargins(4, 4, 4, 4);
            lay->setSpacing(4);

            const auto makeBtn = [&](const QString& text, const QString& tip,
                                     const QByteArray& seq) {
                auto* b = new QToolButton(softKeys_);
                b->setText(text);
                b->setToolTip(tip);
                b->setAutoRepeat(true);
                b->setAutoRepeatDelay(500);
                b->setAutoRepeatInterval(60);
                b->setMinimumSize(44, 34);
                b->setFocusPolicy(Qt::NoFocus);   // never steal terminal focus
                b->setCursor(Qt::PointingHandCursor);
                connect(b, &QToolButton::clicked, this, [this, seq] {
                    sendString(seq);
                });
                lay->addWidget(b);
                return b;
            };

            auto* up = makeBtn("↑", "向上 (发送 ↑ 方向键)", "\x1b[A");
            auto* down = makeBtn("↓", "向下 (发送 ↓ 方向键)", "\x1b[B");
            auto* enter = makeBtn("⏎", "回车 (发送 Enter)", "\r");
            Q_UNUSED(up);
            Q_UNUSED(down);
            Q_UNUSED(enter);

            softKeys_->setStyleSheet(
                "#termSoftKeys { background: rgba(30,30,30,0.85);"
                " border: 1px solid #38383d; border-radius: 8px; }"
                "QToolButton { background: #2d2d30; color: #d4d4d4;"
                " border: 1px solid #38383d; border-radius: 6px;"
                " font-size: 14pt; font-weight: 700; }"
                "QToolButton:hover { background: #3e3e42; border-color: #2ea8c0; }"
                "QToolButton:pressed { background: #264f78; }");
        }
        softKeys_->adjustSize();
        softKeys_->show();
        softKeys_->raise();
        positionSoftKeys();
    } else if (softKeys_) {
        softKeys_->hide();
    }
}

// Keep the cluster pinned to the bottom-right inside the terminal viewport.
void TerminalWidget::positionSoftKeys() {
    if (!softKeys_ || !softKeys_->isVisible()) return;
    const int margin = 10;
    softKeys_->move(width() - softKeys_->width() - margin,
                    height() - softKeys_->height() - margin);
}

void TerminalWidget::feed(const QByteArray& bytes) {
    const int histBefore = historySize();
    const QByteArray utf8 = decodeForParser(bytes);
    parser_.feed(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    const int delta = historySize() - histBefore;
    // Keep the user's scroll position stable while history grows underneath.
    if (delta > 0 && viewOffset_ > 0) {
        viewOffset_ = std::min(viewOffset_ + delta, maxViewOffset());
    }
    if (buffer_.altScreenActive()) viewOffset_ = 0;
    // Refresh in-view search hits against the new content.
    if (findBar_ && findBar_->isVisible() && !findEdit_->text().isEmpty())
        updateFindMatches(false);
    update();   // repaint the changed grid
}

// --- encoding layer --------------------------------------------------------

// Length of the trailing partial UTF-8 sequence (0 when the tail is whole).
int TerminalWidget::incompleteUtf8Tail(const QByteArray& buf) {
    int n = 0;
    while (n < static_cast<int>(buf.size()) && n < 4) {
        const unsigned char b = static_cast<unsigned char>(buf[buf.size() - 1 - n]);
        if ((b & 0xC0) == 0x80) { ++n; continue; }   // continuation byte
        int need = 0;
        if ((b & 0xE0) == 0xC0) need = 1;
        else if ((b & 0xF0) == 0xE0) need = 2;
        else if ((b & 0xF8) == 0xF0) need = 3;
        else return 0;                               // ASCII / invalid lead
        return (n < need) ? n + 1 : 0;
    }
    return 0;
}

// Length of the prefix made of complete GBK units (1 byte for ASCII,
// 2 bytes per lead+trail pair). A trailing lone lead byte is excluded so
// the caller can carry it over to the next feed().
int TerminalWidget::completeGbkPrefix(const QByteArray& buf) {
    int i = 0;
    const int sz = static_cast<int>(buf.size());
    while (i < sz) {
        const unsigned char b = static_cast<unsigned char>(buf[i]);
        if (b < 0x81) { i += 1; }                    // ASCII / control
        else if (b <= 0xFE) {
            if (i + 1 >= sz) break;                  // lone lead at the end
            i += 2;                                  // lead + trail
        } else {
            i += 1;                                  // 0xFF: not a GBK lead
        }
    }
    return i;
}

QByteArray TerminalWidget::decodeForParser(const QByteArray& raw) {
    const auto toBytes = [](const std::string& s) {
        return QByteArray(s.data(), static_cast<int>(s.size()));
    };
    const auto asStd = [](const QByteArray& b) {
        return std::string(b.constData(), static_cast<std::size_t>(b.size()));
    };

    switch (encoding_) {
    case TermEncoding::Utf8:
        // The parser assembles UTF-8 itself (including split sequences).
        lastOutputWasGbk_ = false;
        return raw;

    case TermEncoding::Gbk: {
        QByteArray buf = encCarry_ + raw;
        encCarry_.clear();
        const int complete = completeGbkPrefix(buf);
        encCarry_ = buf.mid(complete);
        lastOutputWasGbk_ = true;
        return toBytes(toUtf8(asStd(buf.left(complete)), "gbk"));
    }

    case TermEncoding::Auto:
    default: {
        QByteArray buf = encCarry_ + raw;
        encCarry_.clear();
        // Hold back a trailing partial UTF-8 sequence and see whether the
        // remainder is valid UTF-8; GBK streams almost never are.
        const int hold = incompleteUtf8Tail(buf);
        const QByteArray head = buf.left(buf.size() - hold);
        const QByteArray tail = buf.mid(buf.size() - hold);
        if (isValidUtf8(asStd(head))) {
            encCarry_ = tail;
            lastOutputWasGbk_ = false;
            return head;
        }
        // Not UTF-8: treat the whole buffer (tail rejoined) as GBK using
        // pair-boundary carry so split pairs survive to the next feed().
        buf = head + tail;
        const int complete = completeGbkPrefix(buf);
        encCarry_ = buf.mid(complete);
        lastOutputWasGbk_ = true;
        return toBytes(toUtf8(asStd(buf.left(complete)), "gbk"));
    }
    }
}

void TerminalWidget::setEncoding(TermEncoding enc) {
    if (encoding_ == enc) return;
    encoding_ = enc;
    encCarry_.clear();
    lastOutputWasGbk_ = (enc == TermEncoding::Gbk);
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
    viewOffset_ = std::clamp(viewOffset_, 0, maxViewOffset());
}

void TerminalWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    recomputeGeometry();
    if (findBar_) {
        findBar_->move(std::max(4, width() - findBar_->width() - 8), 8);
    }
    positionSoftKeys();
}

void TerminalWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // Reveal the on-screen soft keyboard whenever the terminal becomes
    // visible (unless the caller explicitly turned it off).
    setSoftKeyboardVisible(softKeysVisible_);
}

void TerminalWidget::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (softKeys_) softKeys_->hide();
}

void TerminalWidget::focusInEvent(QFocusEvent* e) {
    QWidget::focusInEvent(e);
    update();   // cursor switches from hollow to filled
}

void TerminalWidget::focusOutEvent(QFocusEvent* e) {
    QWidget::focusOutEvent(e);
    update();
}

// --- scrollback ------------------------------------------------------------

int TerminalWidget::historySize() const {
    return static_cast<int>(buffer_.history().size());
}

int TerminalWidget::maxViewOffset() const {
    return buffer_.altScreenActive() ? 0 : historySize();
}

int TerminalWidget::globalRowOf(int viewRow) const {
    return historySize() - viewOffset_ + viewRow;
}

TermCell TerminalWidget::cellAtGlobal(int col, int grow) const {
    const int h = historySize();
    if (grow < h) {
        const auto& line = buffer_.history()[static_cast<std::size_t>(grow)];
        if (col >= 0 && col < static_cast<int>(line.size()))
            return line[static_cast<std::size_t>(col)];
        return TermCell{};
    }
    const int r = grow - h;
    if (r >= 0 && r < buffer_.rows() && col >= 0 && col < buffer_.cols())
        return buffer_.cell(col, r);
    return TermCell{};
}

void TerminalWidget::scrollBy(int lines) {
    if (maxViewOffset() == 0) return;
    viewOffset_ = std::clamp(viewOffset_ + lines, 0, maxViewOffset());
    update();
}

void TerminalWidget::scrollToBottom() {
    if (viewOffset_ != 0) {
        viewOffset_ = 0;
        update();
    }
}

// --- painting --------------------------------------------------------------

void TerminalWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setFont(font_);

    const int cols = buffer_.cols();
    const int rows = buffer_.rows();
    const int h = historySize();
    const bool live = (viewOffset_ == 0);
    const bool showCursor = live && buffer_.cursorVisible();
    const int cx = buffer_.cursor().col;
    const int cy = h + buffer_.cursor().row;   // cursor in global-row space

    // Cursor highlight may cover both halves of a wide character.
    int curStart = cx, curSpan = 1;
    if (showCursor && cx >= 0 && cx < cols) {
        const TermCell cc = cellAtGlobal(cx, cy);
        if (cc.ch == 0 && cx > 0 && TerminalBuffer::isWideChar(cellAtGlobal(cx - 1, cy).ch)) {
            curStart = cx - 1;            // cursor on the spacer half
            curSpan = 2;
        } else if (TerminalBuffer::isWideChar(cc.ch) && cx + 1 < cols &&
                   cellAtGlobal(cx + 1, cy).ch == 0) {
            curSpan = 2;                  // cursor on the lead half
        }
    }
    // NB: must check the row too -- without it the cursor column gets the
    // accent background on *every* row, drawing a full-height vertical bar.
    auto cursorCovers = [&](int c, int grow) {
        return showCursor && grow == cy && c >= curStart && c < curStart + curSpan;
    };

    // Resolve a cell's foreground/background (inverse, faint, selection,
    // search hit, cursor block). Shared by both paint passes.
    auto cellColors = [&](int c, int grow, QColor& fg, QColor& bg) {
        const TermCell cell = cellAtGlobal(c, grow);
        fg = resolveColor(cell.fg, true, (cell.flags & TermCell::kBold) != 0);
        bg = resolveColor(cell.bg, false, false);
        if (cell.flags & TermCell::kInverse) std::swap(fg, bg);
        if (cell.flags & TermCell::kFaint) fg = blend(fg, bg, 0.5);

        // Selection highlight: swap to a contrast pair.
        if (isSelected(c, grow)) {
            std::swap(fg, bg);
            if (fg == bg) {
                fg = kFg;
                bg = kSelectBg;
            }
        }

        // Search hits (kept subtle so text stays readable).
        if (!matches_.empty()) {
            const FindMatch* active =
                (findCurrent_ >= 0 && findCurrent_ < static_cast<int>(matches_.size()))
                    ? &matches_[static_cast<std::size_t>(findCurrent_)]
                    : nullptr;
            for (const FindMatch& m : matches_) {
                if (m.row == grow && c >= m.col && c < m.col + m.len) {
                    bg = (&m == active) ? kFindBgActive : kFindBg;
                    break;
                }
                if (m.row > grow) break;
            }
        }

        // Cursor keeps the glyph visible on top of the accent block.
        if (cursorCovers(c, grow) && hasFocus()) {
            bg = kCursor;
            fg = kCursorText;
        }
    };

    for (int r = 0; r < rows; ++r) {
        const int grow = h - viewOffset_ + r;

        // Pass 1: paint every background first. This is what keeps the
        // spacer half of a wide (CJK) character from covering the right
        // side of its glyph -- backgrounds are down before any glyph.
        for (int c = 0; c < cols; ++c) {
            QColor fg, bg;
            cellColors(c, grow, fg, bg);
            p.fillRect(c * cellW_, r * cellH_, cellW_, cellH_, bg);
        }

        // Pass 2: glyphs and decorations on top of the backgrounds.
        for (int c = 0; c < cols; ++c) {
            const TermCell cell = cellAtGlobal(c, grow);
            if (cell.ch == 0) continue;   // wide-char spacer: glyph lives in its lead

            const int x = c * cellW_;
            const int y = r * cellH_;
            const bool wide = TerminalBuffer::isWideChar(cell.ch) &&
                              c + 1 < cols && cellAtGlobal(c + 1, grow).ch == 0;
            const int span = wide ? 2 : 1;

            QColor fg, bg;
            cellColors(c, grow, fg, bg);

            // Character (skip blanks; conceal hides the glyph entirely).
            if (cell.ch != ' ' && !(cell.flags & TermCell::kConceal)) {
                QFont cf = font_;
                if (cell.flags & TermCell::kBold) cf.setBold(true);
                if (cell.flags & TermCell::kItalic) cf.setItalic(true);
                if (cf != font_) p.setFont(cf);
                p.setPen(fg);

                const char32_t cp = cell.ch;
                const QString s = QString::fromUcs4(&cp, 1);
                if (wide) {
                    // Center the glyph in its two-cell slot so `ls` columns
                    // stay aligned even when the fallback font's CJK advance
                    // differs slightly from 2 narrow cells.
                    p.drawText(QRectF(x, y, cellW_ * 2, cellH_),
                               Qt::AlignHCenter | Qt::AlignBottom, s);
                } else {
                    p.drawText(x, y + cellH_ - fontDescent_, s);
                }
                if (cf != font_) p.setFont(font_);
            }

            const int w = cellW_ * span;
            if (cell.flags & TermCell::kUnderline) {
                p.fillRect(x, y + cellH_ - 2, w, 1, fg);
            }
            if (cell.flags & TermCell::kStrike) {
                p.fillRect(x, y + cellH_ / 2, w, 1, fg);
            }
        }

        // Unfocused cursor: hollow outline (classic behavior), drawn on top.
        if (showCursor && !hasFocus() && grow == cy) {
            p.setPen(kCursor);
            p.drawRect(curStart * cellW_, r * cellH_,
                       curSpan * cellW_ - 1, cellH_ - 1);
        }
    }
}

QColor TerminalWidget::resolveColor(const TermColor& c, bool fg, bool bold) const {
    if (c.kind == TermColor::Kind::Rgb)
        return QColor(c.r, c.g, c.b);
    if (c.kind == TermColor::Kind::Indexed) {
        int idx = c.idx;
        // xterm "bold is bright": bold + base color 0..7 -> bright variant.
        if (bold && fg && idx < 8) idx += 8;
        return ansiColor(idx);
    }
    // Default: use the terminal's default fg/bg.
    return fg ? kFg : kBg;
}

void TerminalWidget::sendString(const QByteArray& s) {
    if (s.isEmpty()) return;
    emit dataReady(s);
}

// Encode typed text for the remote. In GBK sessions (forced, or auto after
// the stream was recognized as GBK) the remote expects GBK bytes; otherwise
// UTF-8. Escape/arrow sequences stay plain ASCII in sendString().
void TerminalWidget::sendText(const QString& text) {
    if (text.isEmpty()) return;
    const bool useGbk = (encoding_ == TermEncoding::Gbk) ||
                        (encoding_ == TermEncoding::Auto && lastOutputWasGbk_);
    if (!useGbk) {
        sendString(text.toUtf8());
        return;
    }
    const QByteArray utf8 = text.toUtf8();
    const std::string gbk = fromUtf8(
        std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())),
        "gbk");
    sendString(QByteArray(gbk.data(), static_cast<int>(gbk.size())));
}

// --- keyboard --------------------------------------------------------------

void TerminalWidget::keyPressEvent(QKeyEvent* e) {
    const QString text = e->text();
    const int key = e->key();
    const Qt::KeyboardModifiers mods = e->modifiers();
    const bool ctrl = (mods & Qt::ControlModifier) && !(mods & Qt::AltModifier);
    const bool shift = (mods & Qt::ShiftModifier) != 0;

    // --- conventional copy/paste -----------------------------------------
    // Ctrl+Shift+C / Ctrl+Shift+V: terminal-style copy/paste (always).
    if (ctrl && shift && key == Qt::Key_C) { copySelection(); return; }
    if (ctrl && shift && key == Qt::Key_V) { pasteClipboard(); return; }
    // Ctrl+C: copy when there is a selection, otherwise SIGINT (Windows
    // Terminal / VS Code behavior).
    if (ctrl && !shift && key == Qt::Key_C) {
        if (hasSelection()) copySelection();
        else sendString("\x03");
        return;
    }
    // Ctrl+V: always paste.
    if (ctrl && !shift && key == Qt::Key_V) { pasteClipboard(); return; }
    // Ctrl+Insert = copy, Shift+Insert = paste.
    if (key == Qt::Key_Insert && ctrl) { copySelection(); return; }
    if (key == Qt::Key_Insert && shift && !ctrl) { pasteClipboard(); return; }

    // --- selection / find shortcuts ---------------------------------------
    if (ctrl && shift && key == Qt::Key_A) { selectAll(); return; }
    if (ctrl && shift && key == Qt::Key_F) { toggleFindBar(); return; }

    // --- font zoom (Ctrl+= / Ctrl+- / Ctrl+0, Ctrl+wheel handled below) ---
    if (ctrl && (key == Qt::Key_Equal || key == Qt::Key_Plus)) { zoomBy(+1); return; }
    if (ctrl && key == Qt::Key_Minus) { zoomBy(-1); return; }
    if (ctrl && key == Qt::Key_0) { resetZoom(); return; }

    // --- scrollback navigation -------------------------------------------
    if (shift && !(mods & (Qt::ControlModifier | Qt::AltModifier))) {
        switch (key) {
            case Qt::Key_Up: scrollBy(+1); return;
            case Qt::Key_Down: scrollBy(-1); return;
            case Qt::Key_PageUp: scrollBy(+(rows() - 1)); return;
            case Qt::Key_PageDown: scrollBy(-(rows() - 1)); return;
            case Qt::Key_Home: scrollBy(maxViewOffset()); return;
            case Qt::Key_End: scrollToBottom(); return;
            default: break;
        }
    }

    // Ctrl combinations produce control bytes.
    if (ctrl) {
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

    // Arrow keys with modifiers (e.g. Ctrl+Left word-jump in vi/readline)
    // must be checked before the plain-arrow switch. NB: Qt::Key_Left <
    // Qt::Key_Up numerically, so compare each key explicitly.
    if (key == Qt::Key_Up || key == Qt::Key_Down ||
        key == Qt::Key_Right || key == Qt::Key_Left) {
        if (mods & (Qt::ControlModifier | Qt::AltModifier)) {
            const bool c5 = (mods & Qt::ControlModifier) != 0;
            const char* seq = nullptr;
            if (key == Qt::Key_Up) seq = c5 ? "\x1b[1;5A" : "\x1b[1;3A";
            else if (key == Qt::Key_Down) seq = c5 ? "\x1b[1;5B" : "\x1b[1;3B";
            else if (key == Qt::Key_Right) seq = c5 ? "\x1b[1;5C" : "\x1b[1;3C";
            else seq = c5 ? "\x1b[1;5D" : "\x1b[1;3D";
            sendString(seq);
            return;
        }
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

    // Ordinary printable text.
    if (!text.isEmpty() && !(mods & Qt::ControlModifier)) {
        sendText(text);
        return;
    }

    QWidget::keyPressEvent(e);
}

// --- mouse -----------------------------------------------------------------

void TerminalWidget::wheelEvent(QWheelEvent* e) {
    // Ctrl+wheel: font zoom (browser convention).
    if (e->modifiers() & Qt::ControlModifier) {
        zoomBy(e->angleDelta().y() > 0 ? +1 : -1);
        e->accept();
        return;
    }
    int steps = e->angleDelta().y() / 40;
    if (steps == 0 && e->angleDelta().y() != 0)
        steps = e->angleDelta().y() > 0 ? 1 : -1;
    if (steps == 0) { QWidget::wheelEvent(e); return; }

    // In the alternate screen (vi/top/less) there is no scrollback; forward
    // the wheel as arrow keys so full-screen apps scroll naturally.
    if (buffer_.altScreenActive()) {
        const QByteArray seq = steps > 0 ? "\x1b[A" : "\x1b[B";
        for (int i = 0; i < std::abs(steps); ++i) sendString(seq);
        e->accept();
        return;
    }
    scrollBy(steps);
    e->accept();
}

void TerminalWidget::mousePressEvent(QMouseEvent* e) {
    if (!hasFocus()) setFocus();
    if (e->button() == Qt::LeftButton) {
        // Triple-click (the press right after a double-click) selects the line.
        const unsigned long long ts = (unsigned long long)e->timestamp();
        if (afterDoubleClick_ && ts - dblClickTs_ < (unsigned long long)QApplication::doubleClickInterval()) {
            afterDoubleClick_ = false;
            selectLineAt(posAt(e->pos()));
            selecting_ = false;
            update();
            e->accept();
            return;
        }
        afterDoubleClick_ = false;
        // Start a text selection at the pressed cell.
        setSelectionAnchor(posAt(e->pos()));
        selecting_ = true;
        update();
        e->accept();
        return;
    }
    if (e->button() == Qt::MiddleButton) {
        pasteClipboard();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* e) {
    if (selecting_ && (e->buttons() & Qt::LeftButton)) {
        updateSelection(posAt(e->pos()));
        update();
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && selecting_) {
        updateSelection(posAt(e->pos()));
        selecting_ = false;
        if (!selVisible_) {
            // Click without drag: leave no selection behind (classic behavior).
            selAnchor_ = TermPos{-1, -1};
            selPos_ = TermPos{-1, -1};
        }
        update();
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        selectWordAt(posAt(e->pos()));
        selecting_ = false;
        afterDoubleClick_ = true;
        dblClickTs_ = (unsigned long long)e->timestamp();
        update();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    QAction* copy = menu.addAction("复制", this, [this] { copySelection(); });
    copy->setShortcut(QKeySequence("Ctrl+C"));
    copy->setEnabled(hasSelection());
    QAction* paste = menu.addAction("粘贴", this, [this] { pasteClipboard(); });
    paste->setShortcut(QKeySequence("Ctrl+V"));
    paste->setEnabled(!QApplication::clipboard()->text().isEmpty());
    QAction* selAll = menu.addAction("全选", this, [this] { selectAll(); });
    selAll->setShortcut(QKeySequence("Ctrl+Shift+A"));
    menu.addSeparator();
    QAction* clear = menu.addAction("清屏", this, [this] { sendString("\x0c"); });
    clear->setShortcut(QKeySequence("Ctrl+L"));
    QAction* find = menu.addAction("查找...", this, [this] { toggleFindBar(); });
    find->setShortcut(QKeySequence("Ctrl+Shift+F"));
    menu.addSeparator();
    QMenu* encMenu = menu.addMenu("编码");
    const auto setEnc = [this](TermEncoding enc) {
        setEncoding(enc);
        update();
    };
    QAction* encAuto = encMenu->addAction("自动检测");
    encAuto->setCheckable(true);
    encAuto->setChecked(encoding_ == TermEncoding::Auto);
    connect(encAuto, &QAction::triggered, this, [setEnc] { setEnc(TermEncoding::Auto); });
    QAction* encUtf8 = encMenu->addAction("UTF-8");
    encUtf8->setCheckable(true);
    encUtf8->setChecked(encoding_ == TermEncoding::Utf8);
    connect(encUtf8, &QAction::triggered, this, [setEnc] { setEnc(TermEncoding::Utf8); });
    QAction* encGbk = encMenu->addAction("GBK (中文)");
    encGbk->setCheckable(true);
    encGbk->setChecked(encoding_ == TermEncoding::Gbk);
    connect(encGbk, &QAction::triggered, this, [setEnc] { setEnc(TermEncoding::Gbk); });
    menu.addSeparator();
    QAction* zoomIn = menu.addAction("放大", this, [this] { zoomBy(+1); });
    zoomIn->setShortcut(QKeySequence("Ctrl+="));
    QAction* zoomOut = menu.addAction("缩小", this, [this] { zoomBy(-1); });
    zoomOut->setShortcut(QKeySequence("Ctrl+-"));
    QAction* zoomReset = menu.addAction("重置字号", this, [this] { resetZoom(); });
    zoomReset->setShortcut(QKeySequence("Ctrl+0"));
    menu.exec(e->globalPos());
}

// --- selection helpers -----------------------------------------------------

TermPos TerminalWidget::posAt(const QPoint& pt) const {
    int c = pt.x() / cellW_;
    int r = pt.y() / cellH_;
    if (c < 0) c = 0; else if (c >= buffer_.cols()) c = buffer_.cols() - 1;
    if (r < 0) r = 0; else if (r >= buffer_.rows()) r = buffer_.rows() - 1;
    return TermPos{c, globalRowOf(r)};
}

void TerminalWidget::setSelectionAnchor(const TermPos& p) {
    selAnchor_ = p;
    selPos_ = p;
    selVisible_ = false;   // a plain click leaves no highlight until dragged
}

void TerminalWidget::updateSelection(const TermPos& p) {
    selPos_ = p;
    if (p.col != selAnchor_.col || p.row != selAnchor_.row)
        selVisible_ = true;
}

void TerminalWidget::clearSelection() {
    selAnchor_ = TermPos{-1, -1};
    selPos_ = TermPos{-1, -1};
    selecting_ = false;
    selVisible_ = false;
    update();
}

// A cell is selected if it lies within the rectangle spanned by the anchor
// and the current endpoint (inclusive).
bool TerminalWidget::isSelected(int col, int grow) const {
    if (!selVisible_ || selAnchor_.row < 0) return false;
    const int r0 = std::min(selAnchor_.row, selPos_.row);
    const int r1 = std::max(selAnchor_.row, selPos_.row);
    if (grow < r0 || grow > r1) return false;
    // For the single-row case, compare columns directly.
    if (r0 == r1) {
        const int c0 = std::min(selAnchor_.col, selPos_.col);
        const int c1 = std::max(selAnchor_.col, selPos_.col);
        return col >= c0 && col <= c1;
    }
    if (grow == r0) return col >= selAnchor_.col;
    if (grow == r1) return col <= selPos_.col;
    return true;   // full rows in between
}

// Extract the selected text in visual order (anchor -> endpoint), trimming
// trailing blanks from each line and joining with '\n' (terminal convention).
QString TerminalWidget::selectedText() const {
    if (!selVisible_ || selAnchor_.row < 0) return QString();
    const TermPos a = selAnchor_;
    const TermPos b = selPos_;
    const bool reverse = (b.row < a.row) ||
                         (b.row == a.row && b.col < a.col);
    const TermPos& from = reverse ? b : a;
    const TermPos& to = reverse ? a : b;

    QString out;
    for (int r = from.row; r <= to.row; ++r) {
        int c0 = (r == from.row) ? from.col : 0;
        int c1 = (r == to.row) ? to.col : (buffer_.cols() - 1);
        if (c0 > c1) std::swap(c0, c1);
        QString line;
        for (int c = c0; c <= c1; ++c) {
            std::uint32_t ch = cellAtGlobal(c, r).ch;
            if (ch == 0) continue;   // wide-char spacer cell: not text
            line += QChar(ch == 0 ? ' ' : ch);
        }
        // Trim trailing spaces (but keep a line of spaces as one space).
        int end = line.size();
        while (end > 0 && line[end - 1] == ' ') --end;
        out += line.left(end);
        if (r != to.row) out += '\n';
    }
    return out;
}

void TerminalWidget::copySelection() {
    const QString s = selectedText();
    if (s.isEmpty()) return;
    QApplication::clipboard()->setText(s);
}

void TerminalWidget::pasteClipboard() {
    const QString s = QApplication::clipboard()->text();
    if (s.isEmpty()) return;
    // Normalize newlines to \r so the remote shell/editor sees proper
    // carriage returns (many PTYs expect CR on paste).
    QString t = s;
    t.replace("\r\n", "\r");
    t.replace('\n', '\r');
    sendString(t.toUtf8());
}

// Select the word under the cursor (double-click). A word is a maximal run
// of alphanumeric + '_' + '-' + '.' + '/' characters.
void TerminalWidget::selectWordAt(const TermPos& p) {
    const int row = p.row;
    const int cols = buffer_.cols();
    int lo = p.col, hi = p.col;
    auto isWordChar = [](std::uint32_t ch) {
        if (ch >= 'a' && ch <= 'z') return true;
        if (ch >= 'A' && ch <= 'Z') return true;
        if (ch >= '0' && ch <= '9') return true;
        if (ch >= 0x80) return true;   // CJK and other non-ASCII runs
        if (ch == 0) return true;      // wide-char spacer cell
        return ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == ':';
    };
    while (lo > 0 && isWordChar(cellAtGlobal(lo - 1, row).ch)) --lo;
    while (hi < cols - 1 && isWordChar(cellAtGlobal(hi + 1, row).ch)) ++hi;
    selAnchor_ = TermPos{lo, row};
    selPos_ = TermPos{hi, row};
    selVisible_ = true;
}

// Triple-click: select the whole line (trailing blanks trimmed).
void TerminalWidget::selectLineAt(const TermPos& p) {
    const int cols = buffer_.cols();
    int hi = cols - 1;
    while (hi > 0 && (cellAtGlobal(hi, p.row).ch == 0 ||
                      cellAtGlobal(hi, p.row).ch == ' '))
        --hi;
    selAnchor_ = TermPos{0, p.row};
    selPos_ = TermPos{hi, p.row};
    selVisible_ = true;
}

void TerminalWidget::selectAll() {
    const int cols = buffer_.cols();
    const int lastRow = historySize() + buffer_.rows() - 1;
    selAnchor_ = TermPos{0, 0};
    selPos_ = TermPos{cols - 1, lastRow};
    selecting_ = false;
    selVisible_ = true;
    update();
}

// --- find bar --------------------------------------------------------------

QString TerminalWidget::lineText(int grow) const {
    const int cols = buffer_.cols();
    QString s;
    s.reserve(cols);
    for (int c = 0; c < cols; ++c) {
        const std::uint32_t ch = cellAtGlobal(c, grow).ch;
        if (ch == 0) continue;   // wide-char spacer: keeps search text joined
        s += QChar(ch == 0 ? ' ' : ch);
    }
    return s;
}

void TerminalWidget::toggleFindBar() {
    if (findBar_ && findBar_->isVisible()) {
        hideFindBar();
        return;
    }
    if (!findBar_) {
        findBar_ = new QWidget(this);
        findBar_->setObjectName("termFindBar");
        auto* lay = new QHBoxLayout(findBar_);
        lay->setContentsMargins(8, 4, 8, 4);
        lay->setSpacing(6);

        findEdit_ = new QLineEdit(findBar_);
        findEdit_->setPlaceholderText("查找");
        findEdit_->setClearButtonEnabled(true);
        findEdit_->setMinimumWidth(160);
        findCount_ = new QLabel("0 / 0", findBar_);
        findCount_->setMinimumWidth(48);
        auto* prev = new QToolButton(findBar_);
        prev->setText("↑");
        prev->setToolTip("上一个 (Shift+Enter)");
        auto* next = new QToolButton(findBar_);
        next->setText("↓");
        next->setToolTip("下一个 (Enter)");
        auto* close = new QToolButton(findBar_);
        close->setText("✕");
        close->setToolTip("关闭 (Esc)");

        lay->addWidget(findEdit_);
        lay->addWidget(findCount_);
        lay->addWidget(prev);
        lay->addWidget(next);
        lay->addWidget(close);

        findBar_->setStyleSheet(
            "#termFindBar { background: #2d2d30; border: 1px solid #38383d;"
            " border-radius: 6px; }"
            "QLineEdit { background: #1e1e1e; color: #d4d4d4;"
            " border: 1px solid #38383d; border-radius: 4px;"
            " padding: 3px 6px; selection-background-color: #264f78; }"
            "QLabel { color: #9d9d9d; }"
            "QToolButton { color: #d4d4d4; border: none;"
            " padding: 2px 6px; border-radius: 3px; }"
            "QToolButton:hover { background: #3e3e42; }");

        connect(findEdit_, &QLineEdit::textChanged, this,
                [this] { updateFindMatches(true); });
        connect(prev, &QToolButton::clicked, this, [this] { gotoMatch(-1); });
        connect(next, &QToolButton::clicked, this, [this] { gotoMatch(+1); });
        connect(close, &QToolButton::clicked, this, [this] { hideFindBar(); });
        findEdit_->installEventFilter(this);
        findBar_->adjustSize();
        findBar_->resize(std::max(findBar_->width(), 300), findBar_->height());
    }
    findBar_->show();
    findBar_->raise();
    findBar_->move(std::max(4, width() - findBar_->width() - 8), 8);
    findEdit_->setFocus();
    findEdit_->selectAll();
    updateFindMatches(true);
}

void TerminalWidget::hideFindBar() {
    if (findBar_) findBar_->hide();
    matches_.clear();
    findCurrent_ = -1;
    setFocus();
    update();
}

bool TerminalWidget::eventFilter(QObject* watched, QEvent* ev) {
    if (watched == findEdit_ && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) {
            hideFindBar();
            return true;
        }
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            gotoMatch(ke->modifiers() & Qt::ShiftModifier ? -1 : +1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, ev);
}

void TerminalWidget::updateFindMatches(bool resetCurrent) {
    matches_.clear();
    const QString q = findEdit_ ? findEdit_->text() : QString();
    if (!q.isEmpty()) {
        const int totalRows = historySize() + buffer_.rows();
        for (int grow = 0; grow < totalRows; ++grow) {
            // Build the row's text plus a string-index -> grid-column map
            // (wide-char spacer cells are skipped, so they differ).
            QString line;
            std::vector<int> colOf;
            const int cols = buffer_.cols();
            for (int c = 0; c < cols; ++c) {
                const std::uint32_t ch = cellAtGlobal(c, grow).ch;
                if (ch == 0) continue;
                line += QChar(ch);
                colOf.push_back(c);
            }
            int from = 0;
            while (true) {
                const int i = static_cast<int>(line.indexOf(q, from, Qt::CaseInsensitive));
                if (i < 0) break;
                const int gridCol = colOf[static_cast<std::size_t>(i)];
                const int lastCol = colOf[static_cast<std::size_t>(
                    std::min<int>(i + static_cast<int>(q.size()) - 1,
                                  static_cast<int>(colOf.size()) - 1))];
                matches_.push_back(FindMatch{grow, gridCol, lastCol - gridCol + 1});
                from = i + std::max(1, static_cast<int>(q.size()));
            }
        }
    }
    if (resetCurrent || findCurrent_ >= static_cast<int>(matches_.size()))
        findCurrent_ = matches_.empty() ? -1 : 0;

    if (findCurrent_ >= 0 && findCurrent_ < static_cast<int>(matches_.size())) {
        const FindMatch& m = matches_[static_cast<std::size_t>(findCurrent_)];
        const int h = historySize();
        const int top = h - viewOffset_;
        if (m.row < top || m.row >= top + rows()) {
            viewOffset_ = std::clamp(h - m.row + rows() / 3, 0, maxViewOffset());
        }
    }
    updateFindLabel();
    update();
}

void TerminalWidget::gotoMatch(int delta) {
    const int n = static_cast<int>(matches_.size());
    if (n == 0) return;
    findCurrent_ = ((findCurrent_ + delta) % n + n) % n;
    const FindMatch& m = matches_[static_cast<std::size_t>(findCurrent_)];
    const int h = historySize();
    const int top = h - viewOffset_;
    if (m.row < top || m.row >= top + rows()) {
        viewOffset_ = std::clamp(h - m.row + rows() / 3, 0, maxViewOffset());
    }
    updateFindLabel();
    update();
}

void TerminalWidget::updateFindLabel() {
    if (!findCount_) return;
    if (matches_.empty()) {
        findCount_->setText(findEdit_ && !findEdit_->text().isEmpty() ? "无结果" : "0 / 0");
    } else {
        findCount_->setText(QString("%1 / %2").arg(findCurrent_ + 1).arg(matches_.size()));
    }
}

}  // namespace ftpflow
