// ftpflow -- protocol backend stubs.
// Compiled only when libcurl / libssh2 are not linked. They keep the
// surface area of core.lib stable so the CLI and tests build against the
// same headers today, while M0 Spike steps 4-5 replace them with the real
// engines (libcurl multi / libssh2 wincng).
#include "ftpflow/protocol_status.hpp"

namespace ftpflow {

ProtocolEngine protocolEngineStatus() {
    ProtocolEngine s;
#if defined(FTPFLOW_HAVE_ZSTD)
    s.zstdAvailable = true;
#endif
#if defined(FTPFLOW_HAVE_LIBSSH2)
    s.sshAvailable = true;
#endif
#if defined(FTPFLOW_HAVE_CURL)
    s.ftpAvailable = true;
#endif
    s.execChannelCompression = s.sshAvailable && s.zstdAvailable;
    return s;
}

}  // namespace ftpflow
