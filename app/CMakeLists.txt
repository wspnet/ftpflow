# Qt Widgets desktop shell (GPL-free surface: qtbase only, LGPLv3 dynamic).
# Skipped gracefully when Qt6 is not installed so the skeleton always
# configures on a bare MSVC machine.
find_package(Qt6 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND)
    message(STATUS "Qt6 not found -> desktop shell skipped (skeleton mode). "
                   "Install Qt 6.12 via the online installer to enable it.")
    return()
endif()

set(CMAKE_AUTOMOC ON)
qt_add_executable(ftpflow_app
    src/main.cpp
    src/mainwindow.cpp
)
target_link_libraries(ftpflow_app PRIVATE
    ftpflow_core
    Qt6::Widgets
)
target_compile_features(ftpflow_app PRIVATE cxx_std_17)
set_target_properties(ftpflow_app PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME "ftpflow"
)
