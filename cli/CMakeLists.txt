# ftpcli.exe -- shares core.lib with the future desktop shell.
add_executable(ftpcli src/main.cpp)
target_link_libraries(ftpcli PRIVATE ftpflow_core)
target_compile_features(ftpcli PRIVATE cxx_std_17)
