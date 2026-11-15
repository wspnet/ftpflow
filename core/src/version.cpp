// ftpflow -- version stamp, part of core.lib.
#include "ftpflow/version.hpp"

namespace ftpflow {

int versionMajor() { return FTPFLOW_VERSION_MAJOR; }
int versionMinor() { return FTPFLOW_VERSION_MINOR; }
int versionPatch() { return FTPFLOW_VERSION_PATCH; }
const char* versionString() { return FTPFLOW_VERSION_STRING; }

}  // namespace ftpflow
