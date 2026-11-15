# Qt Widgets desktop shell (GPL-free surface: qtbase only, LGPLv3 dynamic).
# Skipped gracefully when Qt6 is not installed so the skeleton always
# configures on a bare MSVC machine.
find_package(Qt6 COMPONENTS Widgets Concurrent QUIET)
if(NOT Qt6_FOUND)
    message(STATUS "Qt6 not found -> desktop shell skipped (skeleton mode). "
                   "Install Qt  6.12 via the online installer to enable it.")
    return()
endif()

set(CMAKE_AUTOMOC ON)
qt_add_executable(ftpflow_app
    src/main.cpp
    src/mainwindow.cpp
    src/local_fs_model.cpp
    src/remote_fs_model.cpp
    src/connect_dialog.cpp
)
target_link_libraries(ftpflow_app PRIVATE
    ftpflow_core
    Qt6::Widgets
    Qt6::Concurrent
)
target_compile_features(ftpflow_app PRIVATE cxx_std_17)
set_target_properties(ftpflow_app PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME "ftpflow"
)

# RemoteFsModel worker-thread integration test (console, Qt Core only).
# Runs against the dev pyftpdlib (127.0.0.1:2121) and the local sshd.
qt_add_executable(test_remote_model tests/test_remote_model.cpp src/remote_fs_model.cpp)
target_include_directories(test_remote_model PRIVATE src)
target_link_libraries(test_remote_model PRIVATE ftpflow_core Qt6::Core)
target_compile_features(test_remote_model PRIVATE cxx_std_17)
