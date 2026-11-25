// Regression: ESC ( X / ESC ) X is a one-byte charset designation, not an
// open-ended string. Swallowing output until the next backslash used to eat
// whole MySQL banner lines and the "mysql> " prompt (see issue screenshot).
#include "terminal_buffer.hpp"
#include "vt_parser.hpp"

#include <cstdio>
#include <string>

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool rowContains(const ftpflow::TerminalBuffer& buf, int row,
                 const std::string& needle) {
    std::string line;
    for (int c = 0; c < buf.cols(); ++c) {
        const std::uint32_t ch = buf.cell(c, row).ch;
        line.push_back(ch && ch != ' ' ? static_cast<char>(ch) : ' ');
    }
    return line.find(needle) != std::string::npos;
}
}  // namespace

int main() {
    ftpflow::TerminalBuffer buf(120, 24);
    ftpflow::VtParser parser(buf);

    // Each \x1b(B used to put the parser into an unterminated "String" state.
    const std::string stream =
        "\x1b(B"
        "Welcome to the MySQL monitor.  Commands end with ; or \\g.\r\n"
        "Your MySQL connection id is 7\r\n"
        "Server version: 8.0.36\r\n"
        "\x1b(B"
        "Type 'help;' or '\\h' for help. Type '\\c' to clear the current input "
        "statement.\r\n"
        "\x1b)0"
        "mysql> ";
    parser.feed(stream);

    check(rowContains(buf, 0, "Welcome to the MySQL monitor."),
          "line after ESC ( B is printed");
    check(rowContains(buf, 1, "Your MySQL connection id is 7"),
          "following lines are not swallowed");
    check(rowContains(buf, 3, "Type 'help;' or '\\h' for help."),
          "line after a second ESC ( B is printed");
    check(rowContains(buf, 4, "mysql> "),
          "prompt after ESC ) 0 is printed");

    // ST-terminated strings must still consume until ESC backslash, and a
    // bare backslash inside them must NOT end the string.
    ftpflow::TerminalBuffer buf2(120, 24);
    ftpflow::VtParser p2(buf2);
    p2.feed(std::string("\x1bPpayload\\with\\backslash\x1b\\visible"));
    check(rowContains(buf2, 0, "visible"),
          "DCS string ends only at ESC backslash");

    std::printf("vt_charset: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
