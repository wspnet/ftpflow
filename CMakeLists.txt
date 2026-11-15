cmake_minimum_required(VERSION 3.21)

# vcpkg manifest toolchain must be set BEFORE project(). Enabled through
# build_native.py --full, which exports VCPKG_ROOT.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

project(ftpflow
    VERSION 0.1.0
    DESCRIPTION "FTP/SSH file transfer workbench with transparent SSH-side compression"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# ftpflow v5.1 architecture:
#   core/   -> pure C++17 + Win32, no Qt. Output name: core.lib
#   cli/    -> ftpcli.exe, links core (same binary reuses the engine)
#   app/    -> Qt Widgets desktop shell, links core (optional, needs Qt6)
#   tests/  -> assert-based test exe
#
# Protocol backends (libcurl / libssh2 / libzstd) are optional so the
# skeleton compiles with a bare MSVC toolchain; flip them on once deps
# are provisioned (vcpkg manifest in vcpkg.json, see README).
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Static CRT (/MT) everywhere, matching the Tencent Weixin reference
# practice: zero runtime redistributable issues for end users.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor /wd4100)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

option(FTPFLOW_BUILD_CLI    "Build ftpcli.exe"            ON)
option(FTPFLOW_BUILD_GUI    "Build Qt Widgets desktop app" ON)
option(FTPFLOW_BUILD_TESTS  "Build unit tests"             ON)
option(FTPFLOW_WITH_CURL    "Enable FTP/FTPS backend (libcurl)"      OFF)
option(FTPFLOW_WITH_LIBSSH2 "Enable SFTP/SSH backend (libssh2)"      OFF)
option(FTPFLOW_WITH_ZSTD    "Enable zstd compression pipeline"       OFF)

add_subdirectory(core)

if(FTPFLOW_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(FTPFLOW_BUILD_GUI)
    add_subdirectory(app)
endif()

if(FTPFLOW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Convenience summary printed at configure time.
message(STATUS "ftpflow ${PROJECT_VERSION} "
    "| CLI ${FTPFLOW_BUILD_CLI} | GUI ${FTPFLOW_BUILD_GUI} | tests ${FTPFLOW_BUILD_TESTS} "
    "| curl ${FTPFLOW_WITH_CURL} | libssh2 ${FTPFLOW_WITH_LIBSSH2} | zstd ${FTPFLOW_WITH_ZSTD}")
