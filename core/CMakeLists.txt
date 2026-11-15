# core.lib -- pure C++17 + Win32, no Qt, no third-party deps by default.
# This library is shared by ftpcli.exe and the future Qt desktop shell,
# mirroring the v5.1 blueprint layering (system layer never depends on UI).

add_library(ftpflow_core STATIC)
set_target_properties(ftpflow_core PROPERTIES OUTPUT_NAME "core")

target_include_directories(ftpflow_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(ftpflow_core PUBLIC cxx_std_17)

target_sources(ftpflow_core PRIVATE
    src/version.cpp
    src/vfs_local.cpp
    src/compress_decide.cpp
    src/compress_trial.cpp
    src/sha256.cpp
    src/ssh_exec.cpp
    src/ftp_session.cpp
    src/ftz1.cpp
    src/backend_stubs.cpp
)

if(FTPFLOW_WITH_CURL)
    find_package(CURL CONFIG REQUIRED)
    target_link_libraries(ftpflow_core PRIVATE CURL::libcurl)
    # PUBLIC: core consumers (CLI/tests/GUI) feature-probe via this macro.
    target_compile_definitions(ftpflow_core PUBLIC FTPFLOW_HAVE_CURL=1)
endif()

if(FTPFLOW_WITH_LIBSSH2)
    find_package(libssh2 CONFIG REQUIRED)
    target_link_libraries(ftpflow_core PRIVATE libssh2::libssh2)
    target_compile_definitions(ftpflow_core PUBLIC FTPFLOW_HAVE_LIBSSH2=1)
endif()

if(FTPFLOW_WITH_ZSTD)
    # vcpkg zstd port exports zstd::libzstd (usage file).
    find_package(zstd CONFIG REQUIRED)
    target_link_libraries(ftpflow_core PRIVATE zstd::libzstd)
    target_compile_definitions(ftpflow_core PUBLIC FTPFLOW_HAVE_ZSTD=1)
endif()

# FTP / SFTP backends are added as separate sources behind these guards in
# M0 Spike steps 4-5 (libcurl multi + libssh2 wincng). backend_stubs.cpp is
# always compiled; it reports engine availability via ifdefs.
