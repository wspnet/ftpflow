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
        // SGR colors: bold red foreground.
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\x1b[1;31mR");
        check(b.cell(0, 0).flags & ftpflow::TermCell::kBold, "bold set");
        check(b.cell(0, 0).fg.kind == ftpflow::TermColor::Kind::Rgb,
              "red fg is RGB");
        check(b.cell(0, 0).fg.r == 0x80 && b.cell(0, 0).fg.g == 0 &&
              b.cell(0, 0).fg.b == 0, "red fg value (0x800000)");
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
        // 256-color (38;5;N).
        ftpflow::TerminalBuffer b(10, 2);
        ftpflow::VtParser p(b);
        p.feed("\x1b[38;5;196mX");   // bright red
        check(b.cell(0, 0).fg.kind == ftpflow::TermColor::Kind::Rgb,
              "256-color fg is RGB");
        check(b.cell(0, 0).fg.r == 0xff && b.cell(0, 0).fg.g == 0 &&
              b.cell(0, 0).fg.b == 0, "256-color 196 = pure red");
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

    std::printf("terminal: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
