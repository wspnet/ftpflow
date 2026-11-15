// ftpflow -- which protocol engines are compiled into this core.lib.
// Part of core.lib: pure C++17, no Qt.
#pragma once

namespace ftpflow {

struct ProtocolEngine {
    bool ftpAvailable = false;           // libcurl multi + Schannel
    bool sshAvailable = false;           // libssh2 + WinCNG
    bool zstdAvailable = false;          // libzstd (BSD-3 leg)
    bool execChannelCompression = false; // SSH exec single-stream fast path
};

ProtocolEngine protocolEngineStatus();

}  // namespace ftpflow
