// ftpflow -- terminal buffer + VT parser unit test (no GUI, no Qt).
//
// Feeds crafted escape sequences into the parser and asserts the resulting
// grid/cursor state, covering the primitives vi/top rely on: cursor moves,
// SGR colors, clear/erase, scroll, and the alternate screen.
#include "vt_parser.hpp"

#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failed; std::fprintf(stderr, "FAIL %s\n", what); }
}

}  // namespace

int main() {
    {
        // Plain text + newline + CR.
        ftpflow::TerminalBuffer b(10, 4);
        ftpflow::VtParser p(b);
        p.feed("ab\r\ncd");
        check(b.cell(0, 0).ch == 'a', "a at 0,0");
        check(b.cell(1, 0).ch == 'b', "b at 1,0");
        check(b.cell(0, 1).ch == 'c', "c at 0,1 after newline");
        check(b.cell(1, 1).ch == 'd', "d at 1,1");
    }

    {
        // Cursor positioning (CUP) + absolute.
        ftpflow::TerminalBuffer b(20, 10);
        ftpflow::VtParser p(b);
        p.feed("\x1b[5;3HX");   // row 5, col 3 (1-based) -> (2,4) 0-based
        check(b.cell(2, 4).ch == 'X', "CUP positions X at (col2,row4)");
    }

    {
        // Cursor movement (CUU/CUD/CUF/CUB).
        ftpflow::TerminalBuffer b(10, 10);
        ftpflow::VtParser p(b);
        p.feed("\x1b[2B\x1b[3CX");   // down 2, right 3 -> (3,2)
        check(b.cell(3, 2).ch == 'X', "CUD+CUF moves cursor");
    }

    {
        // SGR colors: bold red foreground (stored as an ANSI palette index).
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\x1b[1;31mR");
        check(b.cell(0, 0).flags & ftpflow::TermCell::kBold, "bold set");
        check(b.cell(0, 0).fg.kind == ftpflow::TermColor::Kind::Indexed,
              "red fg is Indexed");
        check(b.cell(0, 0).fg.idx == 1, "red fg index = 1");
    }

    {
        // SGR 0 resets attributes.
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\x1b[1;31mR\x1b[0mS");
        check(b.cell(0, 0).flags & ftpflow::TermCell::kBold, "R bold");
        check(!(b.cell(1, 0).flags & ftpflow::TermCell::kBold), "S not bold after SGR0");
        check(b.cell(1, 0).fg.isDefault(), "S fg reset to default");
    }

    {
        // 256-color (38;5;N) keeps the palette index.
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\x1b[38;5;196mX");   // bright red
        check(b.cell(0, 0).fg.kind == ftpflow::TermColor::Kind::Indexed,
              "256-color fg is Indexed");
        check(b.cell(0, 0).fg.idx == 196, "256-color fg index = 196");
    }

    {
        // Truecolor (38;2;r;g;b).
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\x1b[38;2;10;20;30mX");
        check(b.cell(0, 0).fg.r == 10 && b.cell(0, 0).fg.g == 20 &&
              b.cell(0, 0).fg.b == 30, "truecolor rgb");
    }

    {
        // Clear screen (ED 2) + home.
        ftpflow::TerminalBuffer b(10, 5);
        ftpflow::VtParser p(b);
        p.feed("xxxxx");
        p.feed("\x1b[2J");   // clear all
        check(b.cell(0, 0).ch == ' ', "ED2 clears screen");
        check(b.cursor().col == 0 && b.cursor().row == 0, "ED2 homes cursor");
    }

    {
        // Scroll on overflow.
        ftpflow::TerminalBuffer b(10, 3);
        ftpflow::VtParser p(b);
        p.feed("line1\r\nline2\r\nline3\r\nline4");
        check(b.cell(0, 0).ch == 'l', "line2 scrolled to top");
        check(b.cell(0, 1).ch == 'l', "line3 at row1");
        check(b.cell(0, 2).ch == 'l', "line4 at row2");
        // Distinguish the rows by their full text.
        check(b.cell(4, 0).ch == '2', "row0 = line2");
        check(b.cell(4, 1).ch == '3', "row1 = line3");
        check(b.cell(4, 2).ch == '4', "row2 = line4");
    }

    {
        // Alternate screen: content preserved across switch.
        ftpflow::TerminalBuffer b(10, 5);
        ftpflow::VtParser p(b);
        p.feed("MAIN");
        p.feed("\x1b[?1049h");   // enter alt screen
        check(b.altScreenActive(), "alt screen active");
        p.feed("ALT");
        check(b.cell(0, 0).ch == 'A', "alt content written");
        p.feed("\x1b[?1049l");   // leave alt screen
        check(!b.altScreenActive(), "alt screen inactive");
        check(b.cell(0, 0).ch == 'M', "main content restored");
    }

    {
        // Scroll region + reverse index keeps status line pinned (top-style).
        ftpflow::TerminalBuffer b(10, 5);
        ftpflow::VtParser p(b);
        p.feed("\x1b[1;4r");   // scroll region rows 1..4 (0-based 0..3)
        check(b.scrollTop() == 0 && b.scrollBottom() == 3, "scroll region set");
    }

    {
        // Erase line (EL) variants.
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("abcde");
        p.feed("\r\x1b[2K");   // clear whole line
        check(b.cell(0, 0).ch == ' ' && b.cell(4, 0).ch == ' ', "EL2 clears line");
    }

    // --- deferred wrap (xterm wrap-pending) -------------------------------
    {
        // Writing into the last column must NOT jump to the next row; the
        // next character wraps to column 0 of the next row instead.
        ftpflow::TerminalBuffer b(5, 4);
        ftpflow::VtParser p(b);
        p.feed("abcde");
        check(b.cursor().col == 4 && b.cursor().row == 0,
              "cursor stays in last column after filling the line");
        check(b.wrapPending(), "wrap pending after last column");
        p.feed("X");
        check(b.cell(4, 1).ch != 'X', "X not at last column of next row");
        check(b.cell(0, 1).ch == 'X', "X wrapped to column 0 of next row");
        check(b.cell(4, 0).ch == 'e', "first line intact");
        check(b.cursor().col == 1 && b.cursor().row == 1, "cursor after wrap");
        check(!b.wrapPending(), "wrap pending cleared by wrapped char");
    }

    {
        // Regression (screenshot bug): a long line must fold column-0-first,
        // never leaving one character per row in the last column.
        ftpflow::TerminalBuffer b(10, 12);
        ftpflow::VtParser p(b);
        p.feed("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");   // 36 chars
        check(b.cell(0, 0).ch == '0' && b.cell(9, 0).ch == '9', "row 0 full");
        check(b.cell(0, 1).ch == 'A' && b.cell(9, 1).ch == 'J', "row 1 = A-J");
        check(b.cell(0, 2).ch == 'K' && b.cell(9, 2).ch == 'T', "row 2 = K-T");
        check(b.cell(0, 3).ch == 'U' && b.cell(5, 3).ch == 'Z', "row 3 = U-Z");
        check(b.cell(9, 1).ch == 'J', "last col of row 1 is J (not stacked)");
        check(b.cell(9, 2).ch == 'T', "last col of row 2 is T");
    }

    {
        // CR cancels a pending wrap: the next char restarts the line.
        ftpflow::TerminalBuffer b(5, 3);
        ftpflow::VtParser p(b);
        p.feed("abcde\rX");
        check(b.cell(0, 0).ch == 'X', "CR after wrap writes at line start");
        check(b.cell(0, 1).ch == ' ', "no stray char on the next row");
    }

    {
        // BS cancels a pending wrap and steps left.
        ftpflow::TerminalBuffer b(5, 3);
        ftpflow::VtParser p(b);
        p.feed("abcde\bX");
        check(b.cell(3, 0).ch == 'X', "BS after wrap moves left, X at col 3");
        check(b.cell(0, 1).ch == ' ', "no wrap row created");
    }

    {
        // CUP cancels a pending wrap.
        ftpflow::TerminalBuffer b(5, 3);
        ftpflow::VtParser p(b);
        p.feed("abcde\x1b[1;1HX");
        check(b.cell(0, 0).ch == 'X', "CUP cancels wrap, X at home");
        check(b.cell(0, 1).ch == ' ', "no wrap row created");
    }

    {
        // Wrapping into the last row scrolls the region up.
        ftpflow::TerminalBuffer b(5, 2);
        ftpflow::VtParser p(b);
        p.feed("aaaaaaaaaa");   // 10 a's fill both rows
        p.feed("b");
        check(b.cell(0, 0).ch == 'a' && b.cell(4, 0).ch == 'a', "row0 scrolled up");
        check(b.cell(0, 1).ch == 'b', "b at start of new last row");
        check(b.cell(1, 1).ch == ' ', "rest of last row blank");
    }

    // --- DECSC / DECRC ----------------------------------------------------
    {
        ftpflow::TerminalBuffer b(10, 6);
        ftpflow::VtParser p(b);
        p.feed("\x1b[3;4H\x1b[1;31m");   // cursor (3,2), bold red
        p.feed("\x1b" "7");              // DECSC
        p.feed("\x1b[6;9H\x1b[0mY");     // move, reset pen, write
        p.feed("\x1b" "8");              // DECRC
        check(b.cursor().col == 3 && b.cursor().row == 2, "DECRC restores cursor");
        p.feed("Z");
        check(b.cell(3, 2).ch == 'Z', "write goes to restored position");
        check((b.cell(3, 2).flags & ftpflow::TermCell::kBold) != 0,
              "DECRC restores pen flags");
        check(b.cell(3, 2).fg.kind == ftpflow::TermColor::Kind::Indexed &&
              b.cell(3, 2).fg.idx == 1, "DECRC restores pen fg");
    }

    {
        // DECRC with no prior DECSC homes the cursor.
        ftpflow::TerminalBuffer b(10, 4);
        ftpflow::VtParser p(b);
        p.feed("hello");
        p.feed("\x1b" "8");
        check(b.cursor().col == 0 && b.cursor().row == 0,
              "bare DECRC homes cursor");
    }

    // --- IND / RI ---------------------------------------------------------
    {
        // IND keeps the column and steps down.
        ftpflow::TerminalBuffer b(10, 5);
        ftpflow::VtParser p(b);
        p.feed("\x1b[2;3H\x1b" "DX");   // cursor (2,1), IND -> (2,2), write X
        check(b.cell(2, 2).ch == 'X', "IND moves down, column kept");
    }

    {
        // IND at the scroll bottom scrolls the region up.
        ftpflow::TerminalBuffer b(10, 3);
        ftpflow::VtParser p(b);
        p.feed("aaa\r\nbbb\r\nccc");
        p.feed("\x1b[1;1H\x1b[3;1H");   // (0,2)
        p.feed("\x1b" "D");             // IND at bottom -> scroll up
        check(b.cell(0, 0).ch == 'b', "IND scrolled: row0 = bbb");
        check(b.cell(0, 1).ch == 'c', "row1 = ccc");
        check(b.cell(0, 2).ch == ' ', "row2 blank after scroll");
    }

    {
        // RI keeps the column and steps up.
        ftpflow::TerminalBuffer b(10, 5);
        ftpflow::VtParser p(b);
        p.feed("\x1b[4;3H\x1b" "MX");   // cursor (2,3), RI -> (2,2), write X
        check(b.cell(2, 2).ch == 'X', "RI moves up, column kept");
    }

    {
        // RI at the top of the region scrolls content down.
        ftpflow::TerminalBuffer b(10, 3);
        ftpflow::VtParser p(b);
        p.feed("aaa\r\nbbb\r\nccc");
        p.feed("\x1b[3;1H");            // (0,2)
        p.feed("\x1b[1;1H\x1b" "M");    // home, RI -> scroll down
        check(b.cell(0, 0).ch == ' ', "RI scrolled down: blank at top");
        check(b.cell(0, 1).ch == 'a', "row1 = aaa");
        check(b.cell(0, 2).ch == 'b', "row2 = bbb");
    }

    {
        // RI is not a scroll-up: cursor must move up, content stays.
        ftpflow::TerminalBuffer b(10, 4);
        ftpflow::VtParser p(b);
        p.feed("top");
        p.feed("\x1b[3;1H\x1b" "M");    // (0,2), RI -> (0,1)
        check(b.cursor().row == 1, "RI moved cursor up");
        check(b.cell(0, 0).ch == 't', "content untouched by RI");
    }

    // --- wrap state hygiene -----------------------------------------------
    {
        // Resize clears a pending wrap.
        ftpflow::TerminalBuffer b(5, 3);
        ftpflow::VtParser p(b);
        p.feed("abcde");
        check(b.wrapPending(), "wrap pending before resize");
        b.resize(8, 3);
        check(!b.wrapPending(), "resize clears wrap pending");
    }

    {
        // LF cancels a pending wrap.
        ftpflow::TerminalBuffer b(5, 4);
        ftpflow::VtParser p(b);
        p.feed("abcde\nX");
        check(b.wrapPending() == false, "LF clears wrap pending");
        check(b.cell(0, 1).ch == 'X', "X on the next line start");
        check(b.cell(4, 1).ch == ' ', "no stacked char in last column");
    }

    // --- scrollback history ------------------------------------------------
    {
        // Overflow scrolling saves the evicted line into history.
        ftpflow::TerminalBuffer b(10, 3);
        ftpflow::VtParser p(b);
        p.feed("line1\r\nline2\r\nline3\r\nline4");
        check(b.history().size() == 1, "one line scrolled into history");
        check(!b.history().empty() && b.history()[0].size() == 10,
              "history line keeps full width");
        check(!b.history().empty() && b.history()[0][0].ch == 'l' &&
              b.history()[0][4].ch == '1', "history holds line1");
    }

    {
        // The alternate screen never writes history (vi/top).
        ftpflow::TerminalBuffer b(10, 3);
        ftpflow::VtParser p(b);
        p.feed("\x1b[?1049h");   // enter alt screen
        p.feed("a\r\nb\r\nc\r\nd\r\ne");
        check(b.history().empty(), "alt screen keeps history empty");
        p.feed("\x1b[?1049l");   // leave alt screen
        p.feed("x\r\ny\r\nz\r\nw");
        check(b.history().size() == 1, "main screen scrolls into history");
    }

    {
        // ED 3 clears the scrollback history.
        ftpflow::TerminalBuffer b(10, 3);
        ftpflow::VtParser p(b);
        p.feed("line1\r\nline2\r\nline3\r\nline4");
        check(b.history().size() == 1, "history populated before ED3");
        p.feed("\x1b[3J");
        check(b.history().empty(), "ED3 clears history");
    }

    {
        // RI at the top (scroll down) must NOT create history entries.
        ftpflow::TerminalBuffer b(10, 3);
        ftpflow::VtParser p(b);
        p.feed("a\r\nb");
        p.feed("\x1b[2;5H");   // somewhere in the middle
        p.feed("\x1bM\x1bM\x1bM");   // RI past the top -> content scrolls down
        check(b.history().empty(), "reverse scroll keeps history empty");
    }

    // --- UTF-8 decoding ----------------------------------------------------
    {
        // Multi-byte UTF-8 sequences become single code points (this was the
        // mojibake bug: every byte used to land in its own cell).
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\xe4\xb8\xad\xe6\x96\x87");   // "中文"
        check(b.cell(0, 0).ch == 0x4E2D, "UTF-8: 中 stored as one code point");
        check(b.cell(1, 0).ch == 0, "UTF-8: 中 spacer at col 1");
        check(b.cell(2, 0).ch == 0x6587, "UTF-8: 文 at col 2");
        check(b.cell(3, 0).ch == 0, "UTF-8: 文 spacer at col 3");
    }
    {
        // Sequences split across feed() calls are reassembled.
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\xe4\xb8");        // first 2 bytes of 中
        p.feed("\xad" "X");        // completes 中, then ASCII X
        check(b.cell(0, 0).ch == 0x4E2D, "split UTF-8 reassembled across feeds");
        check(b.cell(2, 0).ch == 'X', "X after the reassembled wide char");
    }
    {
        // UTF-8 combined with SGR escapes (colored Chinese prompt output).
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\x1b[1;31m\xe6\xb5\x8b");   // bold red 测
        check(b.cell(0, 0).ch == 0x6D4B, "UTF-8 after SGR decodes");
        check((b.cell(0, 0).flags & ftpflow::TermCell::kBold) != 0,
              "pen flags survive UTF-8 assembly");
        check(b.cell(0, 0).fg.kind == ftpflow::TermColor::Kind::Indexed &&
              b.cell(0, 0).fg.idx == 1, "pen color survives UTF-8 assembly");
    }

    // --- wide (CJK) character cells ---------------------------------------
    {
        // A wide char occupies two cells and advances the cursor by 2.
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\xe4\xb8\xad");   // 中
        check(b.cell(0, 0).ch == 0x4E2D, "wide char stored at col 0");
        check(b.cell(1, 0).ch == 0, "spacer cell at col 1");
        check(b.cursor().col == 2 && b.cursor().row == 0,
              "cursor advanced 2 after wide char");
    }
    {
        // A wide char filling the last two columns defers the wrap.
        ftpflow::TerminalBuffer b(4, 2);
        ftpflow::VtParser p(b);
        p.feed("\xe5\xbd\x92");   // 归 -> cols 0-1
        p.feed("\xe5\xbd\x92");   // 归 -> cols 2-3, wrap pending
        check(b.cell(2, 0).ch == 0x5F52, "second wide char at col 2");
        check(b.cell(3, 0).ch == 0, "spacer fills the last column");
        check(b.wrapPending(), "wrap pending after wide char fills the line");
        p.feed("A");
        check(b.cell(0, 1).ch == 'A', "A wraps to the next line start");
    }
    {
        // A wide char with only one column left wraps instead of splitting.
        ftpflow::TerminalBuffer b(3, 3);
        ftpflow::VtParser p(b);
        p.feed("ab");           // cols 0-1 used
        p.feed("\xe4\xb8\xad"); // 中 needs two cells -> wraps to row 1
        check(b.cell(0, 1).ch == 0x4E2D, "wide char wrapped to row 1");
        check(b.cell(1, 1).ch == 0, "spacer on row 1");
        check(b.cell(2, 0).ch == ' ', "no partial wide char left on row 0");
    }
    {
        // Overwriting the lead half of a wide pair blanks the spacer half.
        ftpflow::TerminalBuffer b(6, 2);
        ftpflow::VtParser p(b);
        p.feed("\xe4\xb8\xad");   // 中 at cols 0-1
        p.feed("\x1b[1;1HX");     // CUP to col 0, overwrite lead
        check(b.cell(0, 0).ch == 'X', "lead overwritten");
        check(b.cell(1, 0).ch == ' ', "orphan spacer blanked");
    }
    {
        // Overwriting the spacer half blanks the lead half.
        ftpflow::TerminalBuffer b(6, 2);
        ftpflow::VtParser p(b);
        p.feed("\xe4\xb8\xad");   // 中 at cols 0-1
        p.feed("\x1b[1;2HX");     // CUP to col 1, overwrite spacer
        check(b.cell(1, 0).ch == 'X', "spacer overwritten");
        check(b.cell(0, 0).ch == ' ', "orphan lead blanked");
    }
    {
        // EL from inside a wide pair must not leave half a glyph.
        ftpflow::TerminalBuffer b(6, 2);
        ftpflow::VtParser p(b);
        p.feed("A\xe4\xb8\xad");  // A at col 0, 中 at cols 1-2
        p.feed("\x1b[1;2H\x1b[K"); // EL from the lead cell
        check(b.cell(1, 0).ch == ' ', "lead cleared by EL");
        check(b.cell(2, 0).ch == ' ', "spacer cleared by EL");
        check(b.cell(0, 0).ch == 'A', "A before EL keeps intact");
    }
    {
        // DCH (delete char) shifts cells; pairs must be healed afterwards.
        ftpflow::TerminalBuffer b(6, 2);
        ftpflow::VtParser p(b);
        p.feed("A\xe4\xb8\xad");  // A | 中[lead,spacer]
        p.feed("\x1b[1;1H\x1b[1P"); // DCH 1 at col 0
        check(b.cell(0, 0).ch == 0x4E2D, "wide lead shifted left");
        check(b.cell(1, 0).ch == 0, "spacer shifted with its lead");
        check(b.cell(2, 0).ch == ' ', "trailing cell blank");
    }

    std::printf("terminal: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
