// ftpflow -- VT100/ANSI escape-sequence parser implementation.
#include "vt_parser.hpp"

#include <cstdlib>
#include <vector>

namespace ftpflow {

const std::uint32_t VtParser::kPalette[16] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0,
    0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff,
};

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
    if (idx < 16) {
        const std::uint32_t rgb = kPalette[idx];
        c.kind = TermColor::Kind::Rgb;
        c.r = (rgb >> 16) & 0xff;
        c.g = (rgb >> 8) & 0xff;
        c.b = rgb & 0xff;
        return c;
    }
    if (idx < 232) {
        // 6x6x6 cube: 16 + 36r + 6g + b.
        const int n = idx - 16;
        const int r = n / 36, g = (n % 36) / 6, b = n % 6;
        const auto level = [](int v) -> std::uint8_t {
            return v == 0 ? 0 : (std::uint8_t)(55 + v * 40);
        };
        c.kind = TermColor::Kind::Rgb;
        c.r = level(r); c.g = level(g); c.b = level(b);
        return c;
    }
    // Grayscale: 232..255.
    const std::uint8_t v = (std::uint8_t)(8 + (idx - 232) * 10);
    c.kind = TermColor::Kind::Rgb;
    c.r = c.g = c.b = v;
    return c;
}

void VtParser::feed(const char* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        switch (state_) {
        case State::Ground:
            if (c == 0x1b) state_ = State::Esc;
            else dispatchChar(c);
            break;

        case State::Esc:
            if (c == '[') { state_ = State::Csi; csiParams_.clear(); }
            else if (c == ']') { state_ = State::Osc; oscBuf_.clear(); }
            else if (c == '(' || c == ')') { state_ = State::String; strBuf_.clear(); }
            else if (c == '7') { /* DECSC save cursor: not tracked (no-op) */ }
            else if (c == '8') { /* DECRC restore cursor: no-op */ }
            else if (c == 'M') buf_.scrollUp(1);      // reverse index
            else if (c == 'D') buf_.newline();        // IND
            else if (c == 'E') buf_.newline();        // NEL
            else if (c == 'c') { /* RIS reset: no-op */ }
            else if (c == '=' || c == '>') { /* keypad mode: ignored */ }
            else state_ = State::Ground;   // unknown ESC: ignore
            break;

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

        case State::String:
            // Consume a string sequence until ST (ESC \).
            if (c == 0x1b) { /* pending ST */ }
            else if (c == '\\') state_ = State::Ground;
            // else: discard
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
                case 2: case 3: buf_.clearAll(); break;
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
    // OSC 0/2 set the window title; OSC 8 hyperlinks. Neither affects the
    // grid, so ignore everything.
    (void)payload;
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
