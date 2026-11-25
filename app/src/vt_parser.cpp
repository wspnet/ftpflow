// ftpflow -- VT100/ANSI escape-sequence parser implementation.
#include "vt_parser.hpp"

#include <cstdlib>
#include <vector>

namespace ftpflow {

namespace {

// Parse a list of CSI params separated by ';'. Missing/empty params are -1
// (meaning "default"); explicit zeros stay 0.
std::vector<int> parseParams(const std::string& s) {
    std::vector<int> out;
    std::string cur;
    for (char c : s) {
        if (c == ';') {
            out.push_back(cur.empty() ? -1 : std::atoi(cur.c_str()));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur.empty() ? -1 : std::atoi(cur.c_str()));
    return out;
}

int pAt(const std::vector<int>& v, std::size_t i, int def) {
    if (i >= v.size() || v[i] < 0) return def;
    return v[i];
}

}  // namespace

TermColor VtParser::indexedColor(int idx) {
    TermColor c;
    if (idx < 0 || idx > 255) return c;
    c.kind = TermColor::Kind::Indexed;
    c.idx = static_cast<std::uint8_t>(idx);
    return c;
}

void VtParser::feed(const char* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        switch (state_) {
        case State::Ground:
            if (c == 0x1b) {
                utf8Need_ = 0;   // abandon any half-read UTF-8 sequence
                state_ = State::Esc;
            }
            else feedByte(c);
            break;

        case State::Esc:
            // Every two-byte ESC sequence ends back in Ground; only the
            // string/CSI/OSC introducers switch to a consuming state. (The
            // final-byte handlers used to fall through without resetting,
            // which swallowed the next printable byte after ESC 7/8/M/D/E.)
            state_ = State::Ground;
            if (c == '[') { state_ = State::Csi; csiParams_.clear(); }
            else if (c == ']') { state_ = State::Osc; oscBuf_.clear(); }
            else if (c == '(' || c == ')') { state_ = State::Charset; }
            else if (c == 'P' || c == '^' || c == '_') {
                state_ = State::String; strBuf_.clear(); strTerm_ = 0;
            }
            else if (c == '7') buf_.saveCursor();       // DECSC
            else if (c == '8') buf_.restoreCursor();    // DECRC
            else if (c == 'M') buf_.reverseIndex();     // RI
            else if (c == 'D') buf_.index();            // IND
            else if (c == 'E') buf_.newline();          // NEL
            else if (c == 'c') { /* RIS reset: no-op */ }
            else if (c == '=' || c == '>') { /* keypad mode: ignored */ }
            break;   // unknown ESC: ignore

        case State::Csi:
            if (c >= 0x40 && c <= 0x7e) {   // final byte
                processCsi(static_cast<char>(c));
                state_ = State::Ground;
            } else {
                csiParams_.push_back(static_cast<char>(c));
            }
            break;

        case State::Osc:
            if (c == 0x07) { processOsc(oscBuf_); state_ = State::Ground; }
            else if (c == 0x1b) state_ = State::OscEsc;
            else oscBuf_.push_back(static_cast<char>(c));
            break;

        case State::OscEsc:
            if (c == '\\') { processOsc(oscBuf_); state_ = State::Ground; }
            else { oscBuf_.push_back(static_cast<char>(c)); state_ = State::Osc; }
            break;

        case State::Charset:
            // ESC ( X / ESC ) X selects a character set. The designator is
            // exactly ONE byte; treating this as an open-ended string used to
            // swallow everything until the next backslash (e.g. the \g in a
            // MySQL banner), making later output -- including the prompt --
            // disappear from the terminal.
            state_ = State::Ground;
            break;

        case State::String:
            // ST-terminated string (DCS/SOS/APC). Only a backslash preceded
            // by ESC ends it; a bare backslash is just data.
            if (c == 0x1b) strTerm_ = 0x1b;
            else if (strTerm_ == 0x1b && c == '\\') {
                strTerm_ = 0;
                state_ = State::Ground;
            } else {
                strTerm_ = 0;
            }
            break;
        }
    }
}

void VtParser::dispatchChar(std::uint32_t ch) {
    switch (ch) {
        case '\r': buf_.carriageReturn(); break;
        case '\n':
        case 0x0b: case 0x0c: buf_.newline(); break;
        case '\b': buf_.backspace(); break;
        case '\t': buf_.tab(); break;
        case '\a': break;                       // bell: ignore
        case 0x0e: case 0x0f: break;            // shift in/out (charset)
        case 0x7f: break;                       // DEL: ignore
        default: buf_.putChar(ch); break;
    }
}

// Assemble multi-byte UTF-8 into single code points before dispatching.
// Previously every byte became its own cell, so Chinese output rendered as
// mojibake (one Latin-1 glyph per UTF-8 byte). Sequences that straddle
// feed() calls are held in utf8Acc_/utf8Need_ until complete. Invalid or
// stray bytes fall back to the legacy Latin-1 behavior so nothing is lost.
void VtParser::feedByte(unsigned char c) {
    if (c < 0x80) {
        utf8Need_ = 0;   // control/ASCII: abandon any broken sequence
        dispatchChar(c);
        return;
    }
    if ((c & 0xC0) == 0x80) {   // continuation byte
        if (utf8Need_ > 0) {
            utf8Acc_ = (utf8Acc_ << 6) | (c & 0x3F);
            if (--utf8Need_ == 0) dispatchChar(utf8Acc_);
        } else {
            dispatchChar(c);    // stray continuation: legacy fallback
        }
        return;
    }
    // Lead byte: flush a broken sequence first, then start a new one.
    if (utf8Need_ > 0) {
        dispatchChar(0xFFFD);
        utf8Need_ = 0;
    }
    if ((c & 0xE0) == 0xC0)      { utf8Acc_ = c & 0x1F; utf8Need_ = 1; }
    else if ((c & 0xF0) == 0xE0) { utf8Acc_ = c & 0x0F; utf8Need_ = 2; }
    else if ((c & 0xF8) == 0xF0) { utf8Acc_ = c & 0x07; utf8Need_ = 3; }
    else dispatchChar(c);        // invalid lead byte: legacy fallback
}

void VtParser::processCsi(char final) {
    const std::vector<int> p = parseParams(csiParams_);
    switch (final) {
        case 'A': buf_.moveCursor(0, -pAt(p, 0, 1)); break;                 // CUU
        case 'B': buf_.moveCursor(0, pAt(p, 0, 1)); break;                  // CUD
        case 'C': buf_.moveCursor(pAt(p, 0, 1), 0); break;                  // CUF
        case 'D': buf_.moveCursor(-pAt(p, 0, 1), 0); break;                 // CUB
        case 'E': buf_.setCursor(0, buf_.cursor().row + pAt(p, 0, 1)); break;  // CNL
        case 'F': buf_.setCursor(0, buf_.cursor().row - pAt(p, 0, 1)); break;  // CPL
        case 'G': buf_.setCursor(pAt(p, 0, 1) - 1, buf_.cursor().row); break;  // CHA
        case 'H': case 'f':                                                 // CUP/HVP
            buf_.setCursor(pAt(p, 1, 1) - 1, pAt(p, 0, 1) - 1);
            break;
        case 'd': buf_.setCursor(buf_.cursor().col, pAt(p, 0, 1) - 1); break;  // VPA
        case 'J':
            switch (pAt(p, 0, 0)) {
                case 0: buf_.clearToEnd(); break;
                case 1: buf_.clearToBeginning(); break;
                case 2: buf_.clearAll(); break;
                case 3: buf_.clearAll(); buf_.clearHistory(); break;
            }
            break;
        case 'K':
            switch (pAt(p, 0, 0)) {
                case 0: buf_.clearLine(); break;
                case 1: buf_.clearLineToBeginning(); break;
                case 2: buf_.clearLineAll(); break;
            }
            break;
        case 'S': buf_.scrollUp(pAt(p, 0, 1)); break;                       // SU
        case 'T': buf_.scrollDown(pAt(p, 0, 1)); break;                     // SD
        case 'L': buf_.insertLines(pAt(p, 0, 1)); break;                    // IL
        case 'M': buf_.deleteLines(pAt(p, 0, 1)); break;                    // DL
        case '@': buf_.insertChars(pAt(p, 0, 1)); break;                    // ICH
        case 'P': buf_.deleteChars(pAt(p, 0, 1)); break;                    // DCH
        case 'm':
            if (csiParams_.empty()) {
                applySgr(0);
            } else {
                // SGR params may be chained; extended colors use 38/48 with
                // trailing args (38;5;N or 38;2;R;G;B).
                for (std::size_t i = 0; i < p.size(); ++i) {
                    const int code = p[i];
                    if (code == 38 || code == 48) {
                        const bool fg = (code == 38);
                        if (i + 1 < p.size() && p[i + 1] == 5 && i + 2 < p.size()) {
                            (fg ? fgPending_ = indexedColor(p[i + 2]) : bgPending_ = indexedColor(p[i + 2]));
                            i += 2;
                        } else if (i + 4 < p.size() && p[i + 1] == 2) {
                            TermColor c; c.kind = TermColor::Kind::Rgb;
                            c.r = (std::uint8_t)pAt(p, i + 2, 0);
                            c.g = (std::uint8_t)pAt(p, i + 3, 0);
                            c.b = (std::uint8_t)pAt(p, i + 4, 0);
                            (fg ? fgPending_ = c : bgPending_ = c);
                            i += 4;
                        }
                    } else {
                        applySgr(code);
                    }
                }
                // Flush any extended-color override.
                if (fgPending_.kind != TermColor::Kind::Default || bgPending_.kind != TermColor::Kind::Default) {
                    TermColor fg = fgPending_.kind != TermColor::Kind::Default ? fgPending_ : buf_.penFg();
                    TermColor bg = bgPending_.kind != TermColor::Kind::Default ? bgPending_ : buf_.penBg();
                    buf_.setPen(fg, bg, buf_.penFlags());
                    fgPending_ = TermColor{};
                    bgPending_ = TermColor{};
                }
            }
            break;
        case 'h': case 'l': {                                               // SM/RM
            const std::string s = csiParams_;
            const bool priv = !s.empty() && s[0] == '?';
            if (priv && (s == "?1049" || s == "?47"))
                buf_.useAltScreen(final == 'h');
            else if (priv && s == "?25") buf_.setCursorVisible(final == 'h');
            break;
        }
        case 'r': {                                                         // DECSTBM
            const int top = pAt(p, 0, 1);
            const int bot = pAt(p, 1, buf_.rows());
            buf_.setScrollRegion(top - 1, bot - 1);
            break;
        }
        case 'X': {                                                         // ECH
            const int n = pAt(p, 0, 1);
            for (int i = 0; i < n; ++i)
                buf_.setCharAt(' ', buf_.cursor().col + i, buf_.cursor().row);
            break;
        }
        default: break;                                                     // ignore unknown
    }
}

void VtParser::processOsc(const std::string& payload) {
    // OSC 0/2 set the window/tab title; everything else (OSC 8 hyperlinks,
    // color queries, ...) does not affect the grid and is ignored.
    const std::size_t semi = payload.find(';');
    if (semi == std::string::npos) return;
    const std::string code = payload.substr(0, semi);
    if ((code == "0" || code == "2") && titleCb_) {
        titleCb_(payload.substr(semi + 1));
    }
}

void VtParser::applySgr(int code) {
    if (code == 0) {
        buf_.setPen(TermColor{}, TermColor{}, TermCell::kNone);
        return;
    }
    TermColor fg = buf_.penFg();
    TermColor bg = buf_.penBg();
    std::uint8_t flags = buf_.penFlags();

    if (code == 1) flags |= TermCell::kBold;
    else if (code == 2) flags |= TermCell::kFaint;
    else if (code == 3) flags |= TermCell::kItalic;
    else if (code == 4) flags |= TermCell::kUnderline;
    else if (code == 5 || code == 6) flags |= TermCell::kBlink;
    else if (code == 7) flags |= TermCell::kInverse;
    else if (code == 8) flags |= TermCell::kConceal;
    else if (code == 9) flags |= TermCell::kStrike;
    else if (code == 21 || code == 22) flags &= ~(TermCell::kBold | TermCell::kFaint);
    else if (code == 23) flags &= ~TermCell::kItalic;
    else if (code == 24) flags &= ~TermCell::kUnderline;
    else if (code == 25) flags &= ~TermCell::kBlink;
    else if (code == 27) flags &= ~TermCell::kInverse;
    else if (code == 28) flags &= ~TermCell::kConceal;
    else if (code == 29) flags &= ~TermCell::kStrike;
    else if (code >= 30 && code <= 37) fg = indexedColor(code - 30);
    else if (code == 39) fg = TermColor{};
    else if (code >= 40 && code <= 47) bg = indexedColor(code - 40);
    else if (code == 49) bg = TermColor{};
    else if (code >= 90 && code <= 97) fg = indexedColor(code - 90 + 8);
    else if (code >= 100 && code <= 107) bg = indexedColor(code - 100 + 8);

    buf_.setPen(fg, bg, flags);
}

}  // namespace ftpflow
