# Unit tests for core.lib. Zero external test frameworks: assert-based
# exes plus CTest wrappers so `build_native.py --tests` just works.
add_executable(test_compress_decide test_compress_decide.cpp)
target_link_libraries(test_compress_decide PRIVATE ftpflow_core)
target_compile_features(test_compress_decide PRIVATE cxx_std_17)

add_executable(test_vfs_local test_vfs_local.cpp)
target_link_libraries(test_vfs_local PRIVATE ftpflow_core)
target_compile_features(test_vfs_local PRIVATE cxx_std_17)

add_test(NAME compress_decide COMMAND test_compress_decide)
add_test(NAME vfs_local COMMAND test_vfs_local)

# Convenience target: build + run all tests in one step.
add_custom_target(run_tests
    COMMAND $<TARGET_FILE:test_compress_decide>
    COMMAND $<TARGET_FILE:test_vfs_local>
    DEPENDS test_compress_decide test_vfs_local
    USES_TERMINAL
)
