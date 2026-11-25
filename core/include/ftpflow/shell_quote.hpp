// ftpflow -- POSIX-safe remote command quoting.
//
// Every remote command this app runs (SSH exec channel, `zstd` push, remote
// `tar`) is parsed by the server's login shell -- overwhelmingly /bin/sh on
// Linux. A filename like `$(rm -rf ~)` or `` `id` `` substituted raw into
// that command line executes on the server, so paths MUST be quoted.
//
// Scheme (same as the classic shlex.quote):
//   * "boring" strings -- ASCII letters/digits plus . _ - / : @ + , = and any
//     non-ASCII byte (never a shell metacharacter; keeps Chinese filenames
//     bare) -- pass through unquoted;
//   * everything else is wrapped in SINGLE quotes, with embedded single
//     quotes encoded as '\''.
//
// Single quotes neutralize $, `, \, ", ! and newlines all at once. The
// previous scheme used double quotes without escaping those, which left
// command substitution wide open. (Under cmd.exe single quotes are literal,
// so a hostile name there fails as "file not found" instead of executing --
// fail-safe, never fail-open.)
#pragma once

#include <string>

namespace ftpflow {

inline std::string shellQuote(const std::string& s) {
    bool bare = !s.empty();
    for (unsigned char c : s) {
        const bool boring =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
            c == '/' || c == ':' || c == '@' || c == '+' || c == ',' ||
            c == '=' || c >= 0x80;
        if (!boring) { bare = false; break; }
    }
    if (bare) return s;
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";   // close, escaped quote, reopen
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

}  // namespace ftpflow
