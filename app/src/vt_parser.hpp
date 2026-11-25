// ftpflow -- VT100/ANSI escape-sequence parser.
//
// Feeds raw bytes from the remote PTY into a TerminalBuffer. Handles control
// characters, ESC/CSI/OSC sequences, and the SGR subset needed to render
// vi/top and colored prompts. Pure C++17, no Qt.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "terminal_buffer.hpp"

namespace ftpflow {

class VtParser {
public:
    explicit VtParser(TerminalBuffer& buf) : buf_(buf) {}

    // Feed a chunk of bytes. May be called repeatedly; partial escape
    // sequences are buffered across calls.
    void feed(const char* data, std::size_t len);
    void feed(const std::string& s) { feed(s.data(), s.size()); }

    // OSC 0/2 window-title notifications are delivered here (raw UTF-8).
    void setTitleCallback(std::function<void(const std::string&)> cb) {
        titleCb_ = std::move(cb);
    }

private:
    void dispatchChar(std::uint32_t ch);
    void feedByte(unsigned char c);   // printable path (UTF-8 assembly)
    void processCsi(char final);
    void processOsc(const std::string& payload);
    void applySgr(int code);

    // Build an Indexed palette reference (0..255); the widget resolves it.
    static TermColor indexedColor(int idx);

    TerminalBuffer& buf_;
    std::function<void(const std::string&)> titleCb_;

    // Charset is ESC ( X / ESC ) X: one designator byte, then back to Ground.
    // String is ST-terminated data (ESC \ ... ESC \), not a charset sequence.
    enum class State : std::uint8_t { Ground, Esc, Csi, Osc, OscEsc, Charset, String };
    State state_ = State::Ground;

    std::string csiParams_;   // accumulated CSI parameter bytes
    std::string oscBuf_;      // accumulated OSC payload
    std::string strBuf_;      // accumulated DCS/SOS/etc. string (discarded)
    char strTerm_ = 0;

    // Incremental UTF-8 decode state (multi-byte sequences may straddle
    // feed() calls). utf8Need_ counts continuation bytes still expected.
    int utf8Need_ = 0;
    std::uint32_t utf8Acc_ = 0;

    // Pending extended-color (38/48) overrides, flushed after the SGR run.
    TermColor fgPending_;
    TermColor bgPending_;
};

}  // namespace ftpflow
