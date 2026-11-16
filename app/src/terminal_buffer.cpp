// ftpflow -- terminal screen buffer implementation.
#include "terminal_buffer.hpp"

#include <algorithm>

namespace ftpflow {

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
}

void TerminalBuffer::setCursor(int col, int row) {
    cursor_.col = clampCol(col);
    cursor_.row = clampRow(row);
}

void TerminalBuffer::moveCursor(int dcol, int drow) {
    cursor_.col = clampCol(cursor_.col + dcol);
    cursor_.row = clampRow(cursor_.row + drow);
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
    if (cursor_.row >= rows_) {   // cursor past the bottom (wrap guard)
        scrollRegion(scrollTop_, scrollBottom_, 1);
        cursor_.row = scrollBottom_;
    }
    TermCell& c = cell(cursor_.col, cursor_.row);
    c.ch = ch;
    c.fg = penFg_;
    c.bg = penBg_;
    c.flags = penFlags_;

    if (cursor_.col < cols_ - 1) {
        ++cursor_.col;
    } else {
        // Wrap: remember pending wrap and move down on the next printable.
        cursor_.col = cols_ - 1;
        if (cursor_.row < rows_ - 1) {
            ++cursor_.row;
        } else {
            scrollRegion(scrollTop_, scrollBottom_, 1);
            cursor_.row = scrollBottom_;
        }
    }
}

void TerminalBuffer::setCharAt(std::uint32_t ch, int col, int row) {
    if (col < 0 || col >= cols_ || row < 0 || row >= rows_) return;
    TermCell& c = cell(col, row);
    c.ch = ch;
    c.fg = penFg_;
    c.bg = penBg_;
    c.flags = penFlags_;
}

void TerminalBuffer::newline() {
    carriageReturn();
    if (cursor_.row < scrollBottom_) {
        ++cursor_.row;
    } else {
        scrollRegion(scrollTop_, scrollBottom_, 1);
    }
}

void TerminalBuffer::carriageReturn() { cursor_.col = 0; }

void TerminalBuffer::backspace() {
    if (cursor_.col > 0) --cursor_.col;
}

void TerminalBuffer::tab() {
    const int next = (cursor_.col / 8 + 1) * 8;
    cursor_.col = std::min(next, cols_ - 1);
}

void TerminalBuffer::clearAll() {
    for (auto& c : grid_) c.reset();
    cursor_ = TermPos{0, 0};
}

void TerminalBuffer::clearToEnd() {
    for (int c = cursor_.col; c < cols_; ++c) cell(c, cursor_.row).reset();
    for (int r = cursor_.row + 1; r < rows_; ++r)
        for (int c = 0; c < cols_; ++c) cell(c, r).reset();
}

void TerminalBuffer::clearToBeginning() {
    for (int c = 0; c <= cursor_.col; ++c) cell(c, cursor_.row).reset();
    for (int r = 0; r < cursor_.row; ++r)
        for (int c = 0; c < cols_; ++c) cell(c, r).reset();
}

void TerminalBuffer::clearLine() {
    for (int c = cursor_.col; c < cols_; ++c) cell(c, cursor_.row).reset();
}

void TerminalBuffer::clearLineToBeginning() {
    for (int c = 0; c <= cursor_.col; ++c) cell(c, cursor_.row).reset();
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
}

void TerminalBuffer::deleteChars(int n) {
    for (int i = 0; i < n; ++i) {
        for (int c = cursor_.col; c < cols_ - 1; ++c)
            cell(c, cursor_.row) = cell(c + 1, cursor_.row);
        cell(cols_ - 1, cursor_.row).reset();
    }
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
        // Scroll up: rows move toward top.
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
        mainScrollTop_ = scrollTop_;
        mainScrollBottom_ = scrollBottom_;
        for (auto& c : grid_) c.reset();
        cursor_ = TermPos{0, 0};
        scrollTop_ = 0;
        scrollBottom_ = rows_ - 1;
        altActive_ = true;
    } else {
        grid_ = std::move(mainGrid_);
        cursor_ = mainCursor_;
        scrollTop_ = mainScrollTop_;
        scrollBottom_ = mainScrollBottom_;
        altActive_ = false;
    }
}

}  // namespace ftpflow
