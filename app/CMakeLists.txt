# Qt Widgets desktop shell (GPL-free surface: qtbase only, LGPLv3 dynamic).
# Skipped gracefully when Qt6 is not installed so the skeleton always
# configures on a bare MSVC machine.
find_package(Qt6 COMPONENTS Widgets Concurrent Network QUIET)
if(NOT Qt6_FOUND)
    message(STATUS "Qt6 not found -> desktop shell skipped (skeleton mode). "
                   "Install Qt  6.12 via the online installer to enable it.")
    return()
endif()

set(CMAKE_AUTOMOC ON)
qt_add_executable(ftpflow_app
    src/main.cpp
    src/mainwindow.cpp
    src/title_bar.cpp
    src/dialog_title_bar.cpp
    src/local_fs_model.cpp
    src/remote_fs_model.cpp
    src/file_icon.cpp
    src/connect_dialog.cpp
    src/site_store.cpp
    src/command_dialog.cpp
    src/remote_command.cpp
    src/remote_shell.cpp
    src/terminal_buffer.cpp
    src/vt_parser.cpp
    src/terminal_widget.cpp
    src/auth_client.cpp
    src/auth_dialog.cpp
    src/session_unlock_dialog.cpp
)
target_link_libraries(ftpflow_app PRIVATE
    ftpflow_core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    advapi32
    dwmapi
)
target_compile_features(ftpflow_app PRIVATE cxx_std_17)
set_target_properties(ftpflow_app PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME "ftpflow"
)

# RemoteFsModel worker-thread integration test (console, Qt Core only).
# Runs against the dev pyftpdlib (127.0.0.1:2121) and the local sshd.
qt_add_executable(test_remote_model tests/test_remote_model.cpp src/remote_fs_model.cpp src/file_icon.cpp)
target_include_directories(test_remote_model PRIVATE src)
target_link_libraries(test_remote_model PRIVATE ftpflow_core Qt6::Core Qt6::Gui)
target_compile_features(test_remote_model PRIVATE cxx_std_17)

# Inline rename at the model layer: ItemIsEditable / EditRole / setData()
# validation and the deferred filesystem mutation. Console, Qt Core + Gui.
qt_add_executable(test_inline_rename tests/test_inline_rename.cpp
    src/local_fs_model.cpp src/file_icon.cpp)
target_include_directories(test_inline_rename PRIVATE src)
target_link_libraries(test_inline_rename PRIVATE
    ftpflow_core Qt6::Core Qt6::Gui Qt6::Concurrent)
target_compile_features(test_inline_rename PRIVATE cxx_std_17)

# Sorting proxy: '..' pinned on top + folders grouped above files on every
# column/direction, and the 文件类型 column text. Console Qt Core + Gui.
qt_add_executable(test_entry_sort tests/test_entry_sort.cpp
    src/local_fs_model.cpp src/file_icon.cpp)
target_include_directories(test_entry_sort PRIVATE src)
target_link_libraries(test_entry_sort PRIVATE
    ftpflow_core Qt6::Core Qt6::Gui Qt6::Concurrent)
target_compile_features(test_entry_sort PRIVATE cxx_std_17)

# VT charset-designation regression: ESC ( B must not swallow following text.
add_executable(test_vt_charset tests/test_vt_charset.cpp src/vt_parser.cpp src/terminal_buffer.cpp)
set_target_properties(test_vt_charset PROPERTIES AUTOMOC OFF)
target_include_directories(test_vt_charset PRIVATE src)
target_compile_features(test_vt_charset PRIVATE cxx_std_17)

# Inline rename editor geometry: row-height fit + overlay of read-only columns.
qt_add_executable(test_inline_editor tests/test_inline_editor.cpp)
target_include_directories(test_inline_editor PRIVATE src)
target_link_libraries(test_inline_editor PRIVATE Qt6::Widgets)
target_compile_features(test_inline_editor PRIVATE cxx_std_17)

# Identity-scoped site-store isolation (console, Qt Core only). Uses its own
# QSettings app/org name so the real user's sites.json is never touched.
qt_add_executable(test_site_scope tests/test_site_scope.cpp
    src/site_store.cpp src/site_store.hpp)
target_include_directories(test_site_scope PRIVATE src)
target_link_libraries(test_site_scope PRIVATE Qt6::Core advapi32)
target_compile_features(test_site_scope PRIVATE cxx_std_17)

# Terminal buffer + VT parser unit test (no GUI, no Qt).
add_executable(test_terminal tests/test_terminal.cpp src/terminal_buffer.cpp src/vt_parser.cpp)
set_target_properties(test_terminal PROPERTIES AUTOMOC OFF)
target_include_directories(test_terminal PRIVATE src)
target_link_libraries(test_terminal PRIVATE ftpflow_core)
target_compile_features(test_terminal PRIVATE cxx_std_17)
