# Unit tests for core.lib. Zero external test frameworks: assert-based
# exes plus CTest wrappers so `build_native.py --tests` just works.
add_executable(test_compress_decide test_compress_decide.cpp)
target_link_libraries(test_compress_decide PRIVATE ftpflow_core)
target_compile_features(test_compress_decide PRIVATE cxx_std_17)

add_executable(test_vfs_local test_vfs_local.cpp)
target_link_libraries(test_vfs_local PRIVATE ftpflow_core)
target_compile_features(test_vfs_local PRIVATE cxx_std_17)

add_executable(test_compress_trial test_compress_trial.cpp)
target_link_libraries(test_compress_trial PRIVATE ftpflow_core)
target_compile_features(test_compress_trial PRIVATE cxx_std_17)

add_executable(test_sha256 test_sha256.cpp)
target_link_libraries(test_sha256 PRIVATE ftpflow_core)
target_compile_features(test_sha256 PRIVATE cxx_std_17)

add_executable(test_ftz1 test_ftz1.cpp)
target_link_libraries(test_ftz1 PRIVATE ftpflow_core)
target_compile_features(test_ftz1 PRIVATE cxx_std_17)

# Benchmark tool (not a test): 100k-entry enumeration budget from the
# blueprint. Run manually: bench_list.exe [dir] [--create N]
add_executable(bench_list bench_list.cpp)
target_link_libraries(bench_list PRIVATE ftpflow_core)
target_compile_features(bench_list PRIVATE cxx_std_17)

add_test(NAME compress_decide COMMAND test_compress_decide)
add_test(NAME vfs_local COMMAND test_vfs_local)
add_test(NAME compress_trial COMMAND test_compress_trial)
add_test(NAME sha256_ssh COMMAND test_sha256)
add_test(NAME ftz1 COMMAND test_ftz1)

# Convenience target: build + run all tests in one step.
add_custom_target(run_tests
    COMMAND $<TARGET_FILE:test_compress_decide>
    COMMAND $<TARGET_FILE:test_vfs_local>
    COMMAND $<TARGET_FILE:test_compress_trial>
    COMMAND $<TARGET_FILE:test_sha256>
    COMMAND $<TARGET_FILE:test_ftz1>
    DEPENDS test_compress_decide test_vfs_local test_compress_trial test_sha256 test_ftz1
    USES_TERMINAL
)
