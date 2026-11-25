// ftpflow -- terminal screen buffer implementation.
#include "terminal_buffer.hpp"

#include <algorithm>

namespace ftpflow {

// True for East Asian Wide / Fullwidth code points (CJK, kana, fullwidth
// forms, common emoji). These occupy two terminal cells, matching how real
// terminals keep `ls` columns aligned and avoid overlapping glyphs.
bool TerminalBuffer::isWideChar(std::uint32_t ch) {
    return (ch >= 0x1100 && ch <= 0x115F) ||
           (ch >= 0x2E80 && ch <= 0x303E) ||
           (ch >= 0x3041 && ch <= 0x33FF) ||
           (ch >= 0x3400 && ch <= 0x4DBF) ||
           (ch >= 0x4E00 && ch <= 0x9FFF) ||
           (ch >= 0xA000 && ch <= 0xA4CF) ||
           (ch >= 0xA960 && ch <= 0xA97F) ||
           (ch >= 0xAC00 && ch <= 0xD7A3) ||
           (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0xFE10 && ch <= 0xFE19) ||
           (ch >= 0xFE30 && ch <= 0xFE6F) ||
           (ch >= 0xFF00 && ch <= 0xFF60) ||
           (ch >= 0xFFE0 && ch <= 0xFFE6) ||
           (ch >= 0x1F300 && ch <= 0x1F64F) ||
           (ch >= 0x1F900 && ch <= 0x1F9FF) ||
           (ch >= 0x20000 && ch <= 0x3FFFD);
}

TerminalBuffer::TerminalBuffer(int cols, int rows) {
    resize(cols, rows);
}

void TerminalBuffer::resize(int cols, int rows) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) return;

    std::vector<TermCell> next(static_cast<std::size_t>(cols) * rows);
    for (auto& c : next) c.reset();
    const int copyCols = std::min(cols, cols_);
    const int copyRows = std::min(rows, rows_);
    for (int r = 0; r < copyRows; ++r) {
        for (int c = 0; c < copyCols; ++c) {
            next[static_cast<std::size_t>(r) * cols + c] =
                grid_[static_cast<std::size_t>(r) * cols_ + c];
        }
    }
    grid_ = std::move(next);
    cols_ = cols;
    rows_ = rows;
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    cursor_.col = std::min(cursor_.col, cols_ - 1);
    cursor_.row = std::min(cursor_.row, rows_ - 1);
    wrapPending_ = false;
    healWidePairs();   // column clipping may split wide pairs
}

void TerminalBuffer::setCursor(int col, int row) {
    cursor_.col = clampCol(col);
    cursor_.row = clampRow(row);
    wrapPending_ = false;
}

void TerminalBuffer::moveCursor(int dcol, int drow) {
    cursor_.col = clampCol(cursor_.col + dcol);
    cursor_.row = clampRow(cursor_.row + drow);
    wrapPending_ = false;
}

void TerminalBuffer::setPen(const TermColor& fg, const TermColor& bg,
                            std::uint8_t flags) {
    penFg_ = fg;
    penBg_ = bg;
    penFlags_ = flags;
}

void TerminalBuffer::setScrollRegion(int top, int bottom) {
    scrollTop_ = std::max(0, std::min(top, rows_ - 1));
    scrollBottom_ = std::max(scrollTop_, std::min(bottom, rows_ - 1));
}

void TerminalBuffer::putChar(std::uint32_t ch) {
    if (wrapPending_) {
        // Deferred wrap: the previous character ended the line; this one
        // starts the next line at column 0 (xterm semantics).
        wrapPending_ = false;
        cursor_.col = 0;
        lineDown();
    }
    if (cursor_.row >= rows_) {   // cursor past the bottom (wrap guard)
        scrollRegion(scrollTop_, scrollBottom_, 1);
        cursor_.row = scrollBottom_;
    }

    // Wide (CJK/fullwidth) characters occupy two cells. A single-column
    // buffer simply renders them narrow rather than corrupting the grid.
    const bool wide = isWideChar(ch) && cols_ >= 2;
    if (wide && cursor_.col >= cols_ - 1) {
        // Not enough room for both cells on this line: wrap first.
        cursor_.col = 0;
        lineDown();
        if (cursor_.row >= rows_) {
            scrollRegion(scrollTop_, scrollBottom_, 1);
            cursor_.row = scrollBottom_;
        }
    }

    // Writing over half of an existing wide pair must blank the partner half
    // first, otherwise a stale half glyph lingers on screen.
    resetCellAt(cursor_.col, cursor_.row);
    if (wide && cursor_.col + 1 < cols_) resetCellAt(cursor_.col + 1, cursor_.row);

    TermCell& c = cell(cursor_.col, cursor_.row);
    c.ch = ch;
    c.fg = penFg_;
    c.bg = penBg_;
    c.flags = penFlags_;

    if (wide) {
        // Second cell: a spacer (ch == 0) that keeps the background seamless.
        // ch == 0 markers are skipped by text extraction and painting.
        TermCell& pad = cell(cursor_.col + 1, cursor_.row);
        pad.reset();
        pad.ch = 0;
        pad.bg = penBg_;
    }

    const int adv = wide ? 2 : 1;
    if (cursor_.col + adv < cols_) {
        cursor_.col += adv;
    } else {
        // Line filled: stay at the last column and defer the wrap.
        cursor_.col = cols_ - 1;
        wrapPending_ = true;
    }
}

void TerminalBuffer::setCharAt(std::uint32_t ch, int col, int row) {
    if (col < 0 || col >= cols_ || row < 0 || row >= rows_) return;
    // Overwriting half of a wide pair must blank the partner half first.
    resetCellAt(col, row);
    TermCell& c = cell(col, row);
    c.ch = ch;
    c.fg = penFg_;
    c.bg = penBg_;
    c.flags = penFlags_;
}

// Reset one cell to blank. If the cell is one half of a wide pair, the other
// half is blanked too: a lone half glyph must never stay on screen.
void TerminalBuffer::resetCellAt(int col, int row) {
    if (col < 0 || col >= cols_ || row < 0 || row >= rows_) return;
    TermCell& c = cell(col, row);
    if (c.ch == 0 && col > 0) {
        // Spacer half of a wide pair: blank its lead cell as well.
        TermCell& lead = cell(col - 1, row);
        if (isWideChar(lead.ch)) lead.reset();
    } else if (isWideChar(c.ch) && col + 1 < cols_) {
        // Lead half of a wide pair: blank its spacer cell as well.
        TermCell& pad = cell(col + 1, row);
        if (pad.ch == 0) pad.reset();
    }
    c.reset();
}

// After ops that shift cells horizontally (ICH/DCH) or copy rows (resize),
// wide pairs may lose their partner. Blank any orphan lead or orphan spacer.
void TerminalBuffer::healWidePairs() {
    for (int r = 0; r < rows_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            TermCell& cellRef = cell(c, r);
            if (cellRef.ch == 0) {
                // Spacer: valid only right after a wide lead.
                if (c == 0 || !isWideChar(cell(c - 1, r).ch)) cellRef.reset();
            } else if (isWideChar(cellRef.ch)) {
                // Lead: valid only when its spacer follows.
                if (c + 1 >= cols_ || cell(c + 1, r).ch != 0) cellRef.reset();
            }
        }
    }
}

void TerminalBuffer::newline() {
    carriageReturn();
    lineDown();
}

void TerminalBuffer::carriageReturn() {
    cursor_.col = 0;
    wrapPending_ = false;
}

void TerminalBuffer::backspace() {
    wrapPending_ = false;
    if (cursor_.col > 0) --cursor_.col;
}

void TerminalBuffer::tab() {
    wrapPending_ = false;
    const int next = (cursor_.col / 8 + 1) * 8;
    cursor_.col = std::min(next, cols_ - 1);
}

void TerminalBuffer::index() {
    // IND (ESC D): move down one row without changing the column,
    // scrolling the region when at its bottom.
    wrapPending_ = false;
    lineDown();
}

void TerminalBuffer::reverseIndex() {
    // RI (ESC M): move up one row without changing the column; at the
    // top of the scroll region the content scrolls down instead.
    wrapPending_ = false;
    if (cursor_.row > scrollTop_) {
        --cursor_.row;
    } else if (cursor_.row == scrollTop_) {
        scrollRegion(scrollTop_, scrollBottom_, -1);
    } else if (cursor_.row > 0) {
        --cursor_.row;
    }
}

void TerminalBuffer::lineDown() {
    if (cursor_.row < scrollBottom_) {
        ++cursor_.row;
    } else if (cursor_.row == scrollBottom_) {
        scrollRegion(scrollTop_, scrollBottom_, 1);
    } else if (cursor_.row < rows_ - 1) {
        ++cursor_.row;   // below the scroll region: step down to the last row
    }
}

void TerminalBuffer::saveCursor() {
    savedCursor_ = cursor_;
    savedWrapPending_ = wrapPending_;
    savedPenFg_ = penFg_;
    savedPenBg_ = penBg_;
    savedPenFlags_ = penFlags_;
    savedValid_ = true;
}

void TerminalBuffer::restoreCursor() {
    if (savedValid_) {
        cursor_.col = clampCol(savedCursor_.col);
        cursor_.row = clampRow(savedCursor_.row);
        wrapPending_ = savedWrapPending_;
        penFg_ = savedPenFg_;
        penBg_ = savedPenBg_;
        penFlags_ = savedPenFlags_;
    } else {
        // DECRC without a prior DECSC: home with default attributes.
        cursor_ = TermPos{0, 0};
        wrapPending_ = false;
    }
}

void TerminalBuffer::clearAll() {
    for (auto& c : grid_) c.reset();
    cursor_ = TermPos{0, 0};
    wrapPending_ = false;
}

void TerminalBuffer::clearToEnd() {
    for (int c = cursor_.col; c < cols_; ++c) resetCellAt(c, cursor_.row);
    for (int r = cursor_.row + 1; r < rows_; ++r)
        for (int c = 0; c < cols_; ++c) cell(c, r).reset();
}

void TerminalBuffer::clearToBeginning() {
    for (int c = 0; c <= cursor_.col; ++c) resetCellAt(c, cursor_.row);
    for (int r = 0; r < cursor_.row; ++r)
        for (int c = 0; c < cols_; ++c) cell(c, r).reset();
}

void TerminalBuffer::clearLine() {
    for (int c = cursor_.col; c < cols_; ++c) resetCellAt(c, cursor_.row);
}

void TerminalBuffer::clearLineToBeginning() {
    for (int c = 0; c <= cursor_.col; ++c) resetCellAt(c, cursor_.row);
}

void TerminalBuffer::clearLineAll() {
    for (int c = 0; c < cols_; ++c) cell(c, cursor_.row).reset();
}

void TerminalBuffer::insertLines(int n) {
    if (cursor_.row < scrollTop_ || cursor_.row > scrollBottom_) return;
    for (int i = 0; i < n; ++i) {
        for (int r = scrollBottom_; r > cursor_.row; --r)
            for (int c = 0; c < cols_; ++c) cell(c, r) = cell(c, r - 1);
        for (int c = 0; c < cols_; ++c) cell(c, cursor_.row).reset();
    }
}

void TerminalBuffer::deleteLines(int n) {
    if (cursor_.row < scrollTop_ || cursor_.row > scrollBottom_) return;
    for (int i = 0; i < n; ++i) {
        for (int r = cursor_.row; r < scrollBottom_; ++r)
            for (int c = 0; c < cols_; ++c) cell(c, r) = cell(c, r + 1);
        for (int c = 0; c < cols_; ++c) cell(c, scrollBottom_).reset();
    }
}

void TerminalBuffer::insertChars(int n) {
    for (int i = 0; i < n; ++i) {
        for (int c = cols_ - 1; c > cursor_.col; --c)
            cell(c, cursor_.row) = cell(c - 1, cursor_.row);
        cell(cursor_.col, cursor_.row).reset();
    }
    healWidePairs();
}

void TerminalBuffer::deleteChars(int n) {
    for (int i = 0; i < n; ++i) {
        for (int c = cursor_.col; c < cols_ - 1; ++c)
            cell(c, cursor_.row) = cell(c + 1, cursor_.row);
        cell(cols_ - 1, cursor_.row).reset();
    }
    healWidePairs();
}

void TerminalBuffer::scrollUp(int n) {
    scrollRegion(scrollTop_, scrollBottom_, n);
}

void TerminalBuffer::scrollDown(int n) {
    scrollRegion(scrollTop_, scrollBottom_, -n);
}

void TerminalBuffer::scrollRegion(int top, int bottom, int n) {
    if (n == 0) return;
    top = std::max(0, std::min(top, rows_ - 1));
    bottom = std::max(top, std::min(bottom, rows_ - 1));
    const int height = bottom - top + 1;
    n %= (height + 1);
    if (n == 0) return;
    if (n > 0) {
        // Scroll up: rows move toward top. Lines leaving the top of the
        // main screen are saved into the scrollback history first.
        if (top == 0 && !altActive_) {
            for (int r = 0; r < n && r <= bottom; ++r) {
                HistoryLine line(static_cast<std::size_t>(cols_));
                for (int c = 0; c < cols_; ++c)
                    line[static_cast<std::size_t>(c)] = cell(c, r);
                history_.push_back(std::move(line));
            }
            while (history_.size() > kMaxHistoryLines) history_.pop_front();
        }
        for (int r = top; r <= bottom - n; ++r)
            for (int c = 0; c < cols_; ++c) cell(c, r) = cell(c, r + n);
        for (int r = bottom - n + 1; r <= bottom; ++r)
            for (int c = 0; c < cols_; ++c) cell(c, r).reset();
    } else {
        // Scroll down: rows move toward bottom.
        n = -n;
        for (int r = bottom; r >= top + n; --r)
            for (int c = 0; c < cols_; ++c) cell(c, r) = cell(c, r - n);
        for (int r = top; r < top + n; ++r)
            for (int c = 0; c < cols_; ++c) cell(c, r).reset();
    }
}

void TerminalBuffer::useAltScreen(bool use) {
    if (use == altActive_) return;
    if (use) {
        // Save the main screen.
        mainGrid_ = grid_;
        mainCursor_ = cursor_;
        mainWrapPending_ = wrapPending_;
        mainScrollTop_ = scrollTop_;
        mainScrollBottom_ = scrollBottom_;
        for (auto& c : grid_) c.reset();
        cursor_ = TermPos{0, 0};
        wrapPending_ = false;
        scrollTop_ = 0;
        scrollBottom_ = rows_ - 1;
        altActive_ = true;
    } else {
        grid_ = std::move(mainGrid_);
        cursor_ = mainCursor_;
        wrapPending_ = mainWrapPending_;
        scrollTop_ = mainScrollTop_;
        scrollBottom_ = mainScrollBottom_;
        altActive_ = false;
    }
}

}  // namespace ftpflow
