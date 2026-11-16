// ftpflow -- terminal screen buffer (cell grid + cursor + scrolling).
//
// The data model behind the self-hosted VT emulator. A grid of character
// cells, each carrying a code point plus foreground/background color and
// rendering attributes, plus a cursor and a scroll region. Pure C++17, no
// Qt, so it can be unit-tested in isolation from the widget.

#pragma once

#include <cstdint>
#include <vector>

namespace ftpflow {

// Terminal colors. We support the 16 ANSI colors plus the 256-color palette
// and a 24-bit truecolor fallback (stored as RGB, upsampled to 8-bit).
struct TermColor {
    enum class Kind : std::uint8_t { Default, Indexed, Rgb };
    Kind kind = Kind::Default;
    std::uint8_t r = 255, g = 255, b = 255;  // for Rgb
    std::uint8_t idx = 0;                    // for Indexed (0..255)
    bool isDefault() const { return kind == Kind::Default; }
};

struct TermCell {
    std::uint32_t ch = ' ';          // code point (0 for a spacer/blank)
    TermColor fg;                    // foreground
    TermColor bg;                    // background
    std::uint8_t flags = 0;
    enum : std::uint8_t {
        kNone = 0,
        kBold = 1 << 0,
        kFaint = 1 << 1,
        kItalic = 1 << 2,
        kUnderline = 1 << 3,
        kBlink = 1 << 4,
        kInverse = 1 << 5,
        kConceal = 1 << 6,
        kStrike = 1 << 7,
    };
    void reset() { ch = ' '; fg = TermColor{}; bg = TermColor{}; flags = 0; }
};

// Cursor position (0-based within the screen).
struct TermPos {
    int col = 0;
    int row = 0;
};

class TerminalBuffer {
public:
    TerminalBuffer(int cols, int rows);

    // Resize the grid, preserving top-left content (right/bottom clipped or
    // padded with blanks).
    void resize(int cols, int rows);

    int cols() const { return cols_; }
    int rows() const { return rows_; }

    // Access a cell (0-based). Bounds are the caller's responsibility.
    TermCell& cell(int col, int row) {
        return grid_[static_cast<std::size_t>(row) * cols_ + col];
    }
    const TermCell& cell(int col, int row) const {
        return grid_[static_cast<std::size_t>(row) * cols_ + col];
    }

    // --- cursor ------------------------------------------------------------
    TermPos cursor() const { return cursor_; }
    void setCursor(int col, int row);
    void moveCursor(int dcol, int drow);   // clamped to grid
    bool cursorVisible() const { return cursorVisible_; }
    void setCursorVisible(bool v) { cursorVisible_ = v; }

    // Current pen (applied to newly written cells).
    TermColor penFg() const { return penFg_; }
    TermColor penBg() const { return penBg_; }
    std::uint8_t penFlags() const { return penFlags_; }
    void setPen(const TermColor& fg, const TermColor& bg, std::uint8_t flags);

    // --- scroll region -----------------------------------------------------
    void setScrollRegion(int top, int bottom);   // inclusive; default full
    int scrollTop() const { return scrollTop_; }
    int scrollBottom() const { return scrollBottom_; }

    // --- editing primitives ------------------------------------------------
    // Write one code point at the cursor and advance it (handling the wrap
    // and scroll).
    void putChar(std::uint32_t ch);
    // Write without advancing (for terminal `revision` cases).
    void setCharAt(std::uint32_t ch, int col, int row);

    void newline();              // move to next line, scroll if at bottom
    void carriageReturn();       // cursor to column 0
    void backspace();            // cursor left one, no erase
    void tab();                  // advance to next 8-col stop

    void clearAll();             // ED 2: clear screen, cursor home
    void clearToEnd();           // ED 0: clear cursor..end
    void clearToBeginning();     // ED 1: clear start..cursor
    void clearLine();            // EL 0: clear cursor..line end
    void clearLineToBeginning(); // EL 1: clear line start..cursor
    void clearLineAll();         // EL 2: clear whole line

    void insertLines(int n);     // IL: scroll down within region
    void deleteLines(int n);     // DL: scroll up within region
    void insertChars(int n);     // ICH
    void deleteChars(int n);     // DCH

    void scrollUp(int n = 1);    // content moves up within scroll region
    void scrollDown(int n = 1);

    // --- alternate screen (vi/top full-screen apps) ------------------------
    void useAltScreen(bool use);
    bool altScreenActive() const { return altActive_; }

private:
    int clampCol(int c) const { return c < 0 ? 0 : (c >= cols_ ? cols_ - 1 : c); }
    int clampRow(int r) const { return r < 0 ? 0 : (r >= rows_ ? rows_ - 1 : r); }
    void scrollRegion(int top, int bottom, int n);   // positive n scrolls up

    int cols_ = 0;
    int rows_ = 0;
    std::vector<TermCell> grid_;

    TermPos cursor_{0, 0};
    bool cursorVisible_ = true;
    int scrollTop_ = 0;
    int scrollBottom_ = 0;

    TermColor penFg_;
    TermColor penBg_;
    std::uint8_t penFlags_ = TermCell::kNone;

    // Saved main screen + cursor when the alt screen is active.
    bool altActive_ = false;
    std::vector<TermCell> mainGrid_;
    TermPos mainCursor_{0, 0};
    int mainScrollTop_ = 0, mainScrollBottom_ = 0;
};

}  // namespace ftpflow
