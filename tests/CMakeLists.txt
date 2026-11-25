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

add_executable(test_dir_cache test_dir_cache.cpp)
target_link_libraries(test_dir_cache PRIVATE ftpflow_core)
target_compile_features(test_dir_cache PRIVATE cxx_std_17)

add_executable(test_ssh_transfer test_ssh_transfer.cpp)
target_link_libraries(test_ssh_transfer PRIVATE ftpflow_core)
target_compile_features(test_ssh_transfer PRIVATE cxx_std_17)
if(FTPFLOW_WITH_ZSTD)
    find_package(zstd CONFIG REQUIRED)
    target_link_libraries(test_ssh_transfer PRIVATE zstd::libzstd)
endif()

add_executable(test_transfer_queue test_transfer_queue.cpp)
target_link_libraries(test_transfer_queue PRIVATE ftpflow_core ws2_32)
target_compile_features(test_transfer_queue PRIVATE cxx_std_17)

add_executable(test_queue_stop test_queue_stop.cpp)
target_link_libraries(test_queue_stop PRIVATE ftpflow_core ws2_32)
target_compile_features(test_queue_stop PRIVATE cxx_std_17)

add_executable(test_tree_walk test_tree_walk.cpp)
target_link_libraries(test_tree_walk PRIVATE ftpflow_core ws2_32)
target_compile_features(test_tree_walk PRIVATE cxx_std_17)

# End-to-end same-name conflict handling over SFTP (local sshd + test key).
add_executable(test_conflict_sftp test_conflict_sftp.cpp)
target_link_libraries(test_conflict_sftp PRIVATE ftpflow_core ws2_32)
target_compile_features(test_conflict_sftp PRIVATE cxx_std_17)

add_executable(test_known_hosts test_known_hosts.cpp)
target_link_libraries(test_known_hosts PRIVATE ftpflow_core)
target_compile_features(test_known_hosts PRIVATE cxx_std_17)

add_executable(test_ftp_mlsd test_ftp_mlsd.cpp)
target_link_libraries(test_ftp_mlsd PRIVATE ftpflow_core ws2_32)
target_compile_features(test_ftp_mlsd PRIVATE cxx_std_17)

add_executable(test_text_codec test_text_codec.cpp)
target_link_libraries(test_text_codec PRIVATE ftpflow_core)
target_compile_features(test_text_codec PRIVATE cxx_std_17)

# End-to-end GBK navigation check (needs gbk_ftp_server.py on 127.0.0.1:2121).
add_executable(test_gbk_navigate test_gbk_navigate.cpp)
target_link_libraries(test_gbk_navigate PRIVATE ftpflow_core ws2_32)
target_compile_features(test_gbk_navigate PRIVATE cxx_std_17)

# End-to-end Chinese filename transfer check (needs a live FTP server).
add_executable(test_chinese_transfer test_chinese_transfer.cpp)
target_link_libraries(test_chinese_transfer PRIVATE ftpflow_core ws2_32)
target_compile_features(test_chinese_transfer PRIVATE cxx_std_17)

add_executable(test_ssh_exec test_ssh_exec.cpp)
target_link_libraries(test_ssh_exec PRIVATE ftpflow_core ws2_32)
target_compile_features(test_ssh_exec PRIVATE cxx_std_17)

# Directory archive round-trip (needs zstd; skips cleanly otherwise).
add_executable(test_dir_archive test_dir_archive.cpp)
target_link_libraries(test_dir_archive PRIVATE ftpflow_core)
target_compile_features(test_dir_archive PRIVATE cxx_std_17)
if(FTPFLOW_WITH_ZSTD)
    find_package(zstd CONFIG REQUIRED)
    target_link_libraries(test_dir_archive PRIVATE zstd::libzstd)
endif()

# End-to-end directory-archive transfer (needs local sshd + test key).
add_executable(test_dir_archive_transfer test_dir_archive_transfer.cpp)
target_link_libraries(test_dir_archive_transfer PRIVATE ftpflow_core ws2_32)
target_compile_features(test_dir_archive_transfer PRIVATE cxx_std_17)
if(FTPFLOW_WITH_ZSTD)
    find_package(zstd CONFIG REQUIRED)
    target_link_libraries(test_dir_archive_transfer PRIVATE zstd::libzstd)
endif()

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
add_test(NAME dir_cache COMMAND test_dir_cache)
add_test(NAME ssh_transfer COMMAND test_ssh_transfer)
add_test(NAME transfer_queue COMMAND test_transfer_queue)
add_test(NAME tree_walk COMMAND test_tree_walk)
add_test(NAME known_hosts COMMAND test_known_hosts)
add_test(NAME ftp_mlsd COMMAND test_ftp_mlsd)
add_test(NAME ssh_exec COMMAND test_ssh_exec)
add_test(NAME dir_archive COMMAND test_dir_archive)

# Convenience target: build + run all tests in one step.
add_custom_target(run_tests
    COMMAND $<TARGET_FILE:test_compress_decide>
    COMMAND $<TARGET_FILE:test_vfs_local>
    COMMAND $<TARGET_FILE:test_compress_trial>
    COMMAND $<TARGET_FILE:test_sha256>
    COMMAND $<TARGET_FILE:test_ftz1>
    COMMAND $<TARGET_FILE:test_dir_cache>
    COMMAND $<TARGET_FILE:test_ssh_transfer>
    COMMAND $<TARGET_FILE:test_transfer_queue>
    COMMAND $<TARGET_FILE:test_tree_walk>
    COMMAND $<TARGET_FILE:test_known_hosts>
    COMMAND $<TARGET_FILE:test_ftp_mlsd>
    DEPENDS test_compress_decide test_vfs_local test_compress_trial test_sha256 test_ftz1 test_dir_cache test_ssh_transfer test_transfer_queue test_tree_walk test_known_hosts test_ftp_mlsd
    USES_TERMINAL
)
