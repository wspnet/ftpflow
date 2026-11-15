// ftpflow -- version constants, part of core.lib.
#pragma once

#define FTPFLOW_VERSION_MAJOR 0
#define FTPFLOW_VERSION_MINOR 1
#define FTPFLOW_VERSION_PATCH 0
#define FTPFLOW_VERSION_STRING "0.1.0"

namespace ftpflow {
int versionMajor();
int versionMinor();
int versionPatch();
const char* versionString();
}  // namespace ftpflow
