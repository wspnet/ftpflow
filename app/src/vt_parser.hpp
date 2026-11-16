// ftpflow -- VT100/ANSI escape-sequence parser.
//
// Feeds raw bytes from the remote PTY into a TerminalBuffer. Handles control
// characters, ESC/CSI/OSC sequences, and the SGR subset needed to render
// vi/top and colored prompts. Pure C++17, no Qt.

#pragma once

#include <cstdint>
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

private:
    void dispatchChar(std::uint32_t ch);
    void processCsi(char final);
    void processOsc(const std::string& payload);
    void applySgr(int code);

    // Convert an ANSI palette index (0..255) to RGB.
    static TermColor indexedColor(int idx);

    // Standard 16-color palette (0..15).
    static const std::uint32_t kPalette[16];

    TerminalBuffer& buf_;

    enum class State : std::uint8_t { Ground, Esc, Csi, Osc, OscEsc, String };
    State state_ = State::Ground;

    std::string csiParams_;   // accumulated CSI parameter bytes
    std::string oscBuf_;      // accumulated OSC payload
    std::string strBuf_;      // accumulated DCS/SOS/etc. string (discarded)
    char strTerm_ = 0;

    // Pending extended-color (38/48) overrides, flushed after the SGR run.
    TermColor fgPending_;
    TermColor bgPending_;
};

}  // namespace ftpflow
