# ftpflow custom triplet: x64-windows-static + toolchain paths.
#
# Rationale: this machine's sandbox blocks reg.exe, so vcpkg-tool cannot
# assemble its own "vcvars-equivalent" build environment for the Windows
# SDK (rc.exe / mt.exe never resolve -> CMAKE_MT-NOTFOUND -> every port
# fails its compiler ABI test). This triplet therefore:
#   1. passes our pre-assembled INCLUDE/LIB/PATH/SDK vars through
#      (values are exported by tools/build_native.py build_env()),
#   2. pins the manifest/resource tools by absolute path as a fallback.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# ftpflow only ever links release dependencies (app + CLI are /MT release);
# skipping vcpkg's default debug build of every port roughly halves
# dependency build time (OpenSSL alone saves ~10+ minutes).
set(VCPKG_BUILD_TYPE release)

set(VCPKG_ENV_PASSTHROUGH
    "INCLUDE;LIB;PATH;WindowsSDKDir;WindowsSDKVersion;UniversalCRTSdkDir;VCToolsInstallDir;VCINSTALLDir")

if(DEFINED ENV{WindowsSDKDir} AND DEFINED ENV{WindowsSDKVersion})
    file(TO_CMAKE_PATH "$ENV{WindowsSDKDir}" _sdk)
    set(_sdkver "$ENV{WindowsSDKVersion}")
    string(REPLACE "\\" "" _sdkver "${_sdkver}")
    set(VCPKG_CMAKE_CONFIGURE_OPTIONS
        "-DCMAKE_MT=${_sdk}bin/${_sdkver}/x64/mt.exe"
        "-DCMAKE_RC_COMPILER=${_sdk}bin/${_sdkver}/x64/rc.exe")
endif()
